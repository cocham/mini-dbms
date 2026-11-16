#!/usr/bin/env python3
"""인덱스 조회 vs 선형 탐색 벤치마크 (Phase 2 / P2-04).

사용법: python tools/bench.py [--rows N] [--repeat R] [--bin build/minidb] [--workdir build/bench]

1) tools/gen_sql.py로 N건 INSERT SQL을 만들어 빈 데이터 디렉터리에 Phase 1 처리기로 넣는다.
2) 같은 조회 묶음을 R번(프로세스 R개) --timing으로 실행하고, 문장별 시간의 중앙값을 낸다.
3) 결과를 마크다운으로 stdout에 쓴다. docs/phase2/benchmark.md에 옮겨 적는다.

측정값의 뜻:
  - 시작: table_open(파일 전체 검증 + 인덱스 구축). 프로세스마다 한 번 드는 O(N) 비용
  - 문장: executor_run 한 번(결과 출력 포함). 출력은 일반 파일로 버린다(stdout.txt)
  - 계획: 같은 조건의 EXPLAIN 출력(계획 시점 통계). 측정값이 아니다
"""
import argparse
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_sql  # noqa: E402

START_RE = re.compile(r"\[측정\] 시작: ([0-9.]+) ms \(.*?([0-9]+)행\)")
INDEX_RE = re.compile(r"\[측정\] 인덱스: (.*)")
STMT_RE = re.compile(r"\[측정\] 문장 ([0-9]+): ([0-9.]+) ms, 결과 ([0-9]+)행")


def queries(n):
    """(설명, WHERE 조건) 목록. 같은 순서로 SELECT와 EXPLAIN에 쓴다."""
    mid = n // 2
    return [
        ("id 단건 — 처음", "id = 1"),
        ("id 단건 — 가운데", "id = %d" % mid),
        ("id 단건 — 끝", "id = %d" % n),
        ("id 단건 — 없음", "id = %d" % (n + 1)),
        ("id 범위 1,000건", "id BETWEEN %d AND %d" % (mid, mid + 999)),
        ("age 단건 — 일치 많음", "age = 30"),
        ("age 단건 — 일치 없음", "age = 151"),
    ]


def run(bin_path, data_dir, sql_path, timing, sink_path=None):
    """timing이면 stdout을 일반 파일(sink_path)로 버린다. Windows의 NUL은 문자 장치라 C 런타임이
    터미널처럼 줄마다 바로 내보내(버퍼링 없음) 출력 비용이 부풀려지므로 쓰지 않는다."""
    args = [bin_path, "--data-dir", data_dir] + (["--timing"] if timing else []) + [sql_path]
    if not timing:
        return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    with open(sink_path, "wb") as sink:
        return subprocess.run(args, stdout=sink, stderr=subprocess.PIPE, check=False)


def tool_version():
    try:
        out = subprocess.run(["gcc", "--version"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             check=False).stdout.decode("utf-8", "replace")
        return out.splitlines()[0] if out else "알 수 없음"
    except OSError:
        return "알 수 없음"


def fmt_ms(v):
    if v < 1:
        return "%.4f" % v
    return "%.3f" % v if v < 100 else "%.1f" % v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--repeat", type=int, default=7)
    ap.add_argument("--bin", default=os.path.join("build", "minidb" + (".exe" if os.name == "nt" else "")))
    ap.add_argument("--workdir", default=os.path.join("build", "bench"))
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")  # Windows 콘솔·리디렉션에서도 UTF-8

    bin_path = os.path.abspath(a.bin)
    os.makedirs(a.workdir, exist_ok=True)
    data_dir = os.path.join(a.workdir, "data")
    shutil.rmtree(data_dir, ignore_errors=True)
    insert_sql = os.path.join(a.workdir, "insert.sql")
    query_sql = os.path.join(a.workdir, "query.sql")
    explain_sql = os.path.join(a.workdir, "explain.sql")
    sink_path = os.path.join(a.workdir, "stdout.txt")

    # 1) 생성 + 삽입
    t = time.perf_counter()
    with open(insert_sql, "w", encoding="utf-8", newline="\n") as f:
        gen_sql.write_sql(a.rows, f)
    gen_s = time.perf_counter() - t
    t = time.perf_counter()
    with open(sink_path, "wb") as sink:
        p = subprocess.run([bin_path, "--data-dir", data_dir, insert_sql], stdout=sink,
                           stderr=subprocess.PIPE, check=False)
    insert_s = time.perf_counter() - t
    if p.returncode != 0:
        sys.stderr.write("삽입 실패: " + p.stderr.decode("utf-8", "replace"))
        return 1
    data_bytes = os.path.getsize(os.path.join(data_dir, "users.tsv"))

    qs = queries(a.rows)
    with open(query_sql, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join("SELECT * FROM users WHERE %s;\n" % w for _, w in qs))
    with open(explain_sql, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join("EXPLAIN SELECT * FROM users WHERE %s;\n" % w for _, w in qs))

    # 2) 계획 (측정값 아님)
    p = run(bin_path, data_dir, explain_sql, timing=False)
    if p.returncode != 0:
        sys.stderr.write("EXPLAIN 실패: " + p.stderr.decode("utf-8", "replace"))
        return 1
    plans = p.stdout.decode("utf-8").splitlines()

    # 3) 반복 측정
    starts, times, rows, index_line = [], [[] for _ in qs], [None] * len(qs), ""
    for _ in range(a.repeat):
        p = run(bin_path, data_dir, query_sql, timing=True, sink_path=sink_path)
        err = p.stderr.decode("utf-8", "replace")
        if p.returncode != 0:
            sys.stderr.write("조회 실패: " + err)
            return 1
        m = START_RE.search(err)
        starts.append(float(m.group(1)))
        index_line = INDEX_RE.search(err).group(1)
        for m in STMT_RE.finditer(err):
            k = int(m.group(1)) - 1
            times[k].append(float(m.group(2)))
            rows[k] = int(m.group(3))

    w = sys.stdout.write
    w("## 측정 환경\n\n")
    w("| 항목 | 값 |\n|---|---|\n")
    w("| OS | %s |\n" % platform.platform())
    w("| CPU | %s |\n" % (platform.processor() or platform.machine()))
    w("| 컴파일러 | %s (-O2) |\n" % tool_version())
    w("| 레코드 수 | %s건 |\n" % format(a.rows, ","))
    w("| 데이터 파일 | %.1f MB |\n" % (data_bytes / 1048576.0))
    w("| 반복 | 프로세스 %d회, 문장별 중앙값 |\n\n" % a.repeat)
    w("## 삽입과 시작 비용\n\n")
    w("| 항목 | 값 |\n|---|---|\n")
    w("| SQL 생성 (Python) | %.1f s |\n" % gen_s)
    w("| %s건 INSERT (프로세스 1회, 출력 버림) | %.1f s (건당 %.1f µs) |\n"
      % (format(a.rows, ","), insert_s, insert_s * 1e6 / max(a.rows, 1)))
    w("| 시작: 파일 전체 읽기 + 인덱스 구축 (중앙값) | %s ms |\n" % fmt_ms(statistics.median(starts)))
    w("| 인덱스 | %s |\n\n" % index_line)
    w("## 조회 시간 (시작 비용 제외)\n\n")
    w("| 조회 | WHERE | 계획 (EXPLAIN) | 결과 행 | 중앙값 ms | 최소 ms | 최대 ms |\n")
    w("|---|---|---|---:|---:|---:|---:|\n")
    for (label, where), plan, ts, r in zip(qs, plans, times, rows):
        w("| %s | `%s` | %s | %s | %s | %s | %s |\n" % (
            label, where, plan.split(" (")[0], format(r, ","), fmt_ms(statistics.median(ts)),
            fmt_ms(min(ts)), fmt_ms(max(ts))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
