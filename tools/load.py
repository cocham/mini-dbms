#!/usr/bin/env python3
"""부하 측정 (Phase 3 / P3-05). Linux(Docker) 전용.

사용법: python3 tools/load.py --server build/minidb-server --global build/minidb-server-global --cli build/minidb
                              --loadgen build/loadgen
                              [--rows 1000000] [--seconds 5] [--workdir /tmp/minidb_load] [--shared-dir build/load_shared] [--out build/load_result.md]

같은 데이터(100만 행)로 서버를 여러 설정으로 띄우고, 부하 생성기(tools/loadgen.c)로 요청을 보낸다.
이 스크립트는 서버 띄우기·설정 바꾸기·표 만들기만 한다. 요청은 C로 만든 loadgen이 보낸다:
처음에 파이썬 프로세스로 클라이언트를 만들었더니 클라이언트가 요청마다 쓰는 CPU가 커서, 워커를 늘려도
처리량이 그대로였다(서버가 아니라 클라이언트를 잰 셈). loadgen은 클라이언트마다 스레드 하나이고,
응답을 받자마자 다음 요청을 보낸다(닫힌 고리). 응답 시간은 클라이언트에서 잰다:
연결 → 요청 보내기 → 응답 끝까지 받기. 큐에서 기다린 시간도 여기에 들어간다(서버 로그에는 안 보이는 시간).

측정 묶음:
  1. Phase 2와 비교: CLI 한 번 실행(시작 비용 포함)으로 id 하나 조회 vs 떠 있는 서버에 id 하나 조회
  2. 워커 수: id 조회만, 클라이언트 16개, 워커 1·2·4·8개
  3. 요청 종류 × 잠금: id 조회 / 섞기(id 80%, 범위 10%, INSERT 10%) / 선형 탐색, 전역 잠금 vs 읽기·쓰기 잠금
  4. 큐 크기: 워커 1개, 클라이언트 16개, 큐 1칸 vs 64칸 → 503 BUSY 수

주의: 클라이언트와 서버가 같은 컴퓨터(같은 컨테이너)에서 CPU를 나눠 쓴다. 숫자는 이 환경의 상대 비교로만 읽는다.
"""
import argparse
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import sys
import time

SCAN_SQL = b"SELECT * FROM users WHERE age = 150;"
LOADGEN = None  # main에서 정한다


def one_request(port, body, timeout=60):
    """데우기용 요청 하나(파이썬). 측정에는 쓰지 않는다."""
    req = (b"POST /query HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(body)) + body
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall(req)
        while s.recv(1 << 16):
            pass
    finally:
        s.close()


def parse_loadgen(out):
    """loadgen 출력 한 줄 → 사전. 처리량은 '구간 안에서 완료된 성공 수 / 구간 길이'(loadgen이 정한 구간).
    구간이 끝난 뒤 완료된 요청(late)은 성공 수에도 분모에도 넣지 않는다."""
    v = dict(kv.split("=") for kv in out.split())
    num = lambda k: float(v[k]) / 1000.0 if float(v[k]) >= 0 else float("nan")  # ms → 초
    window = float(v["window"])
    return {
        "ok": int(v["ok"]), "busy": int(v["busy"]), "other": int(v["other"]),
        "incomplete": int(v["incomplete"]), "connfail": int(v["connfail"]), "late": int(v["late"]),
        "window": window, "rps": int(v["ok"]) / window,
        "p50": num("p50"), "p95": num("p95"), "p99": num("p99"), "max": num("max"),
    }


def loadgen(port, kind, clients, seconds, rows):
    p = subprocess.run([LOADGEN, str(port), kind, str(clients), str(seconds), str(rows)],
                       capture_output=True, text=True, timeout=seconds + 300)
    if p.returncode != 0:
        raise RuntimeError("loadgen 실패: " + p.stderr)
    return parse_loadgen(p.stdout)


# ---------- 서버 ----------

class Server:
    def __init__(self, binary, data_dir, log_path, extra):
        self.log_path = log_path
        self.log = open(log_path, "w")
        self.p = subprocess.Popen([binary, "--data-dir", data_dir, "--port", "0"] + extra,
                                  stdout=subprocess.DEVNULL, stderr=self.log)
        self.port = None
        self.start_line = ""
        deadline = time.time() + 120
        while time.time() < deadline and self.port is None:
            time.sleep(0.05)
            with open(log_path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.search(r"\[서버\] 시작: .*:(\d+),", line)
                    if m:
                        self.port = int(m.group(1))
                        self.start_line = line.strip()
            if self.p.poll() is not None:
                break
        if self.port is None:
            raise RuntimeError("서버가 시작하지 않음: " + open(log_path, encoding="utf-8", errors="replace").read())

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=120)
        self.log.close()


def fresh_copy(base, workdir, name):
    d = os.path.join(workdir, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy(os.path.join(base, "users.tsv"), d)
    return d


def time_wait_count():
    """이 컨테이너(네트워크 이름공간)의 TIME_WAIT 연결 수 (/proc/net/tcp의 상태 06)."""
    n = 0
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(path) as f:
                next(f)
                n += sum(1 for line in f if line.split()[3] == "06")
        except OSError:
            pass
    return n


TW_QUIET = 2000  # 이보다 적어질 때까지 기다린 뒤 측정을 시작한다
REPEAT = 3       # 설정마다 같은 서버에 이만큼 반복해 처리량이 가운데인 회차를 쓴다


def wait_time_wait():
    """서버가 먼저 닫으므로 요청마다 서버 쪽에 TIME_WAIT 연결이 60초 남는다. 앞 측정이 남긴 것이 쌓여
    상한(이 환경 32,768)에 닿으면 처리량이 절반 가까이 떨어졌다. 그래서 측정마다 빠질 때까지 기다린다."""
    t = time.time()
    while time_wait_count() > TW_QUIET and time.time() - t < 90:
        time.sleep(1)
    return time.time() - t


def run_load(binary, base, workdir, name, extra, kind, clients, seconds, rows):
    wait_time_wait()
    d = fresh_copy(base, workdir, name)
    s = Server(binary, d, os.path.join(workdir, name + ".log"), extra)
    try:
        one_request(s.port, SCAN_SQL if kind == "scan" else b"SELECT * FROM users WHERE id = 1;")  # 데우기
        runs = [loadgen(s.port, kind, clients, seconds, rows) for _ in range(REPEAT)]
    finally:
        s.stop()
    runs.sort(key=lambda r: r["rps"])
    med = dict(runs[len(runs) // 2])  # 처리량이 가운데인 회차를 그대로 보고한다
    med["spread"] = (runs[0]["rps"], runs[-1]["rps"])
    return med


# ---------- 데이터 ----------

def make_data(d, rows):
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "users.tsv")
    mark = os.path.join(d, "rows.txt")
    if os.path.exists(path) and os.path.exists(mark) and open(mark).read().strip() == str(rows):
        return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        buf = []
        for i in range(1, rows + 1):
            buf.append("%d\tuser%06d\t%d\n" % (i, i, i % 151))
            if len(buf) >= 20000:
                f.write("".join(buf))
                buf.clear()
        f.write("".join(buf))
    with open(mark, "w") as f:
        f.write(str(rows))


# ---------- 출력 ----------

def ms(v):
    if v != v:  # nan
        return "-"
    v *= 1000
    return "%.0f" % v if v >= 100 else ("%.1f" % v if v >= 10 else "%.2f" % v)


def row(label, r):
    lo, hi = r["spread"]
    return "| %s | %.0f | %.0f ~ %.0f | %s | %s | %s | %s | %d | %d | %d |" % (
        label, r["rps"], lo, hi, ms(r["p50"]), ms(r["p95"]), ms(r["p99"]), ms(r["max"]), r["busy"],
        r["incomplete"] + r["connfail"] + r["other"], r["late"])


HEAD = ("| 설정 | 처리량 (성공/초) | 3회 범위 | p50 ms | p95 ms | p99 ms | 최대 ms | 503 | 불완전·실패·기타 | 늦음(제외) |\n"
        "|---|---|---|---|---|---|---|---|---|---|")


def cli_once(cli, d, sql_path):
    t = time.perf_counter()
    p = subprocess.run([cli, "--data-dir", d, sql_path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode("utf-8", "replace"))
    return time.perf_counter() - t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--global", dest="glob", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--loadgen", required=True)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--workdir", default=os.path.join(tempfile.gettempdir(), "minidb_load"))
    ap.add_argument("--shared-dir", default="build/load_shared",
                    help="비교용: 호스트와 공유하는 폴더(Docker 바인드 마운트). 빈 문자열이면 비교를 건너뛴다")
    ap.add_argument("--out", default="build/load_result.md")
    a = ap.parse_args()

    global LOADGEN
    LOADGEN = os.path.abspath(a.loadgen)
    server, glob, cli = (os.path.abspath(p) for p in (a.server, a.glob, a.cli))
    os.makedirs(a.workdir, exist_ok=True)
    base = os.path.join(a.workdir, "base")
    make_data(base, a.rows)
    out = []
    say = lambda s="": (out.append(s), print(s, flush=True))
    t_all = time.perf_counter()

    say("## 부하 측정 (P3-05)\n")
    say("- 환경: %s, CPU %d개(os.cpu_count), Python %s. 클라이언트와 서버가 같은 컨테이너에서 CPU를 나눠 쓴다"
        % (platform.platform(), os.cpu_count() or 0, platform.python_version()))
    say("- 데이터: %d행. 측정마다 같은 원본 파일의 복사본으로 서버를 새로 띄운다. 묶음마다 %.0f초 동안 잰다" % (a.rows, a.seconds))
    say("- 설정마다 같은 서버에 %d회 반복하고 처리량이 가운데인 회차의 값을 쓴다(3회 범위도 함께 적는다)" % REPEAT)
    say("- 요청마다 새 연결(keep-alive 없음). 부하 생성기는 응답을 끝까지 받은 뒤 연결을 재설정(RST)으로 닫아"
        " TIME_WAIT 연결이 쌓이지 않게 한다(쌓이면 측정끼리 영향을 준다. benchmark.md 참고)."
        " 연결 정리 방식이 정상 FIN 종료 클라이언트와 다르므로 그 조건의 처리량과 같다고 보지 않는다")
    say("- 성공 = 상태 200이고 헤더와 Content-Length만큼의 본문을 끝까지 받은 응답. 덜 받고 끊기거나 30초 기한을 넘기면"
        " '불완전', 연결·전송 실패는 '실패'로 따로 센다")
    say("- 처리량 = 측정 구간(%.0f초) 안에서 **완료된** 성공 수 / 구간 길이. 구간이 끝나면 새 요청을 보내지 않고,"
        " 그때 진행 중이던 요청은 끝까지 받되 집계하지 않는다('늦음(제외)', 드레인 시간은 분모에 넣지 않음)" % a.seconds)
    say("- p50·p95·p99는 구간 안에서 완료된 성공 요청만의 응답 시간(연결부터 응답 끝까지, 큐 대기 포함)."
        " 503·불완전·늦음 요청의 대기 시간은 들어가지 않는다\n")

    # 1. Phase 2 비교
    d = fresh_copy(base, a.workdir, "cli")
    q = os.path.join(a.workdir, "q.sql")
    open(q, "w").write("SELECT * FROM users WHERE id = %d;\n" % (a.rows // 2))
    cli_t = statistics.median(cli_once(cli, d, q) for _ in range(3))
    s = Server(server, d, os.path.join(a.workdir, "single.log"), [])
    try:
        one_request(s.port, b"SELECT * FROM users WHERE id = 1;")
        single = loadgen(s.port, "id", 1, 2, a.rows)
        start_ms = re.search(r"시작: ([0-9.]+) ms", s.start_line).group(1)
    finally:
        s.stop()
    say("### 1. Phase 2와 비교: id 하나 조회\n")
    say("| 방식 | 걸린 시간 |")
    say("|---|---|")
    say("| CLI 한 번 실행 (`minidb q.sql`, 파일 전체 읽기·인덱스 구축 포함, 3회 중앙값) | %s ms |" % ms(cli_t))
    say("| 떠 있는 서버에 HTTP 요청 (클라이언트 1개, 2초 동안 %d회, 중앙값 / p99) | %s ms / %s ms |"
        % (single["ok"], ms(single["p50"]), ms(single["p99"])))
    say("| (참고) 서버 시작 비용: 같은 파일 읽기·인덱스 구축, 한 번만 | %s ms |\n" % start_ms)

    # 2. 워커 수
    say("### 2. 워커 수: id 조회만, 클라이언트 16개\n")
    say(HEAD)
    for th in (1, 2, 4, 8):
        r = run_load(server, base, a.workdir, "w%d" % th, ["--threads", str(th)], "id", 16, a.seconds, a.rows)
        say(row("워커 %d개" % th, r))
    say("")

    # 3. 요청 종류 × 잠금
    say("### 3. 요청 종류 × 잠금 방식: 워커 4개, 클라이언트 8개\n")
    say(HEAD)
    for kind, label, clients in (("id", "id 조회만", 8), ("mix", "섞기: id 80% · 범위 10% · INSERT 10%", 8),
                                 ("scan", "선형 탐색만", 4)):
        for bname, binary in (("전역 잠금", glob), ("읽기·쓰기 잠금", server)):
            r = run_load(binary, base, a.workdir, "k_%s_%s" % (kind, "g" if binary == glob else "rw"),
                         ["--threads", "4"], kind, clients, a.seconds, a.rows)
            say(row("%s — %s%s" % (label, bname, " (클라이언트 4개)" if clients != 8 else ""), r))
    say("")

    # 4. 큐 크기
    say("### 4. 큐 크기: 워커 1개, 클라이언트 16개, id 조회만\n")
    say(HEAD)
    for qn in (1, 64):
        r = run_load(server, base, a.workdir, "q%d" % qn, ["--threads", "1", "--queue", str(qn)], "id", 16,
                     a.seconds, a.rows)
        say(row("큐 %d칸" % qn, r))
    say("")

    # 5. 환경: 데이터·로그를 호스트 공유 폴더에 둘 때
    if a.shared_dir:
        say("### 5. 환경: 데이터·로그 위치 (워커 4개, 클라이언트 16개, id 조회만)\n")
        say(HEAD)
        r = run_load(server, base, a.workdir, "env_tmp", ["--threads", "4"], "id", 16, a.seconds, a.rows)
        say(row("컨테이너 안 임시 폴더 (%s)" % a.workdir, r))
        os.makedirs(a.shared_dir, exist_ok=True)
        r = run_load(server, base, a.shared_dir, "env_shared", ["--threads", "4"], "id", 16, a.seconds, a.rows)
        say(row("호스트 공유 폴더 (%s, Docker 바인드 마운트)" % a.shared_dir, r))
        shutil.rmtree(a.shared_dir, ignore_errors=True)
        say("")
    say("(전체 측정 %.0f초)" % (time.perf_counter() - t_all))

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
