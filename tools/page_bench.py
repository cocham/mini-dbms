#!/usr/bin/env python3
"""커서 페이지 조회 확인·측정 (Phase 5 / P5-03, DB-P5-D10). Linux(Docker) 전용.

사용법: python3 tools/page_bench.py --server build/minidb-server [--rows 1000000] [--repeat 30]
                                    [--workdir /tmp/minidb_page] [--out build/page_result.md]

변경 없는 데이터(기본 100만 행)로 서버를 기본 설정(응답 상한 8 MiB·100,000행)으로 띄우고 세 가지를 본다.
(--rows가 100,000 이하이면 전체 조회가 상한에 걸리지 않으므로 --max-rows를 rows/2로 낮춰 띄운다. 최소 2000행)
  1. 전체 조회: SELECT * FROM users; 가 RESULT_TOO_LARGE(400)로 실패하는지, 걸린 시간과 응답 바이트
  2. 전체 순회: GET /users?limit=1000 으로 커서 0부터 has_more가 false가 될 때까지 따라가 받은 행이
     users.tsv와 순서·값까지 같은지(중복·누락 없음). 순회 시간은 참고로만 적는다
  3. 페이지 응답 시간과 바이트: 첫·중간·마지막 페이지, limit 100·1000. 같은 요청을 여러 번 보내 중앙값

응답 시간은 이 스크립트(파이썬)가 잰다: 연결 → 요청 보내기 → 응답 끝까지 받기. 요청마다 새 연결이다
(서버는 keep-alive를 지원하지 않는다). 파이썬 처리 시간과 연결 비용이 들어가므로 서버 내부 처리 시간보다 크다.
클라이언트와 서버가 같은 컨테이너에서 CPU를 나눠 쓴다. 숫자는 이 환경에서의 비교로만 읽는다.
"""
import argparse
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import time

PAGE_RE = re.compile(r"페이지 상한 (\d+)행\(기본 (\d+)\)")


def make_data(d, rows):
    """tools/load.py와 같은 형식: id TAB userNNNNNN TAB id%151"""
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "users.tsv")
    mark = os.path.join(d, "rows.txt")
    if os.path.exists(path) and os.path.exists(mark) and open(mark).read().strip() == str(rows):
        return path
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
    return path


class Server:
    def __init__(self, binary, data_dir, log_path, extra):
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


def http(port, method, path, body=b""):
    """(상태, 본문 바이트, 걸린 ms). 연결부터 응답 끝까지 잰다."""
    req = ("%s %s HTTP/1.1\r\nHost: b\r\nContent-Length: %d\r\n\r\n" % (method, path, len(body))).encode() + body
    t0 = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", port), timeout=120)
    try:
        s.sendall(req)
        chunks = []
        while True:
            c = s.recv(1 << 20)
            if not c:
                break
            chunks.append(c)
    finally:
        s.close()
    ms = (time.perf_counter() - t0) * 1000
    raw = b"".join(chunks)
    head, _, data = raw.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    m = re.search(rb"Content-Length: (\d+)", head)
    if m is None or int(m.group(1)) != len(data):
        raise RuntimeError("응답이 잘림: %r" % head[:200])
    return status, data, ms


def check_full_select(port, repeat):
    times, last = [], None
    for _ in range(repeat):
        st, data, ms = http(port, "POST", "/query", b"SELECT * FROM users;")
        times.append(ms)
        last = (st, data)
    st, data = last
    r = json.loads(data)
    code = r.get("error", {}).get("code")
    return {"status": st, "code": code, "bytes": len(data), "median_ms": statistics.median(times),
            "message": r.get("error", {}).get("message", "")}


def check_walk(port, tsv_path, limit):
    want = open(tsv_path, "rb").read().decode("utf-8").splitlines()
    got_n, pages, after, mismatch, first_bad = 0, 0, 0, 0, None
    t0 = time.perf_counter()
    while True:
        st, data, _ = http(port, "GET", "/users?after_id=%d&limit=%d" % (after, limit))
        if st != 200:
            raise RuntimeError("페이지 %d: %d %s" % (pages, st, data[:200]))
        r = json.loads(data)
        pages += 1
        for row in r["rows"]:
            line = "%d\t%s\t%d" % (row[0], row[1], row[2])
            if got_n >= len(want) or want[got_n] != line:
                mismatch += 1
                if first_bad is None:
                    first_bad = (got_n, line)
            got_n += 1
        if r["rows"]:
            if r["next_cursor"] != r["rows"][-1][0]:
                raise RuntimeError("next_cursor가 마지막 행 id가 아님")
            after = r["next_cursor"]
        if not r["has_more"]:
            break
    sec = time.perf_counter() - t0
    return {"rows": got_n, "want": len(want), "pages": pages, "mismatch": mismatch, "first_bad": first_bad,
            "seconds": sec}


def time_page(port, after, limit, repeat):
    times, size, n, more = [], 0, 0, None
    for _ in range(repeat):
        st, data, ms = http(port, "GET", "/users?after_id=%d&limit=%d" % (after, limit))
        if st != 200:
            raise RuntimeError("%d %s" % (st, data[:200]))
        times.append(ms)
        size = len(data)
        r = json.loads(data)
        n, more = len(r["rows"]), r["has_more"]
    times.sort()
    return {"median_ms": statistics.median(times), "p90_ms": times[int(len(times) * 0.9) - 1], "bytes": size,
            "rows": n, "has_more": more}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--repeat", type=int, default=30)
    ap.add_argument("--workdir", default="/tmp/minidb_page")
    ap.add_argument("--out", default="build/page_result.md")
    a = ap.parse_args()
    if a.rows < 2000:
        sys.exit("--rows는 2000 이상이어야 합니다 (1000행 페이지 순회와 전체 조회 실패를 함께 보려면)")
    extra = [] if a.rows > 100000 else ["--max-rows", str(a.rows // 2)]

    data_dir = os.path.join(a.workdir, "data")
    print("데이터 준비: %d행 (%s)" % (a.rows, data_dir), flush=True)
    tsv = make_data(data_dir, a.rows)
    srv = Server(a.server, data_dir, os.path.join(a.workdir, "server.log"), extra)
    out = []
    try:
        print(srv.start_line, flush=True)
        m = PAGE_RE.search(srv.start_line)
        # 데우기: 파일을 페이지 캐시에 올린다 (측정에 넣지 않는다)
        http(srv.port, "POST", "/query", b"SELECT * FROM users WHERE age = 150;")

        sel = check_full_select(srv.port, 5)
        print("전체 조회:", sel, flush=True)
        walk = check_walk(srv.port, tsv, 1000)
        print("전체 순회:", walk, flush=True)
        n = a.rows
        cases = []
        for limit in (100, 1000):
            for label, after in (("첫 페이지", 0), ("중간 페이지", n // 2), ("마지막 페이지", max(0, n - limit))):
                r = time_page(srv.port, after, limit, a.repeat)
                cases.append((limit, label, after, r))
                print(limit, label, after, r, flush=True)
    finally:
        srv.stop()

    out.append("## 커서 페이지 조회 확인 (%d행, 변경 없음)\n" % a.rows)
    out.append("- 서버 시작 줄: `%s`" % srv.start_line)
    if extra:
        out.append("- 행 수가 기본 상한(100,000) 이하라 `%s`로 띄웠다" % " ".join(extra))
    if m:
        out.append("- 페이지 상한 %s행, 기본 limit %s" % (m.group(1), m.group(2)))
    out.append("\n### 1. 전체 조회 `SELECT * FROM users;`\n")
    out.append("| 상태 | 코드 | 응답 바이트 | 응답 시간 중앙값(5회) | 메시지 |")
    out.append("|---|---|---|---|---|")
    out.append("| %d | %s | %d | %.1f ms | %s |" % (sel["status"], sel["code"], sel["bytes"], sel["median_ms"],
                                                 sel["message"]))
    out.append("\n### 2. 전체 순회 `GET /users?limit=1000` (커서 0부터)\n")
    out.append("| 받은 행 | 파일의 행 | 페이지 수 | 파일과 다른 행 | 순회 시간(참고) |")
    out.append("|---|---|---|---|---|")
    out.append("| %d | %d | %d | %d | %.1f s |" % (walk["rows"], walk["want"], walk["pages"], walk["mismatch"],
                                                 walk["seconds"]))
    if walk["first_bad"]:
        out.append("\n첫 불일치: %r" % (walk["first_bad"],))
    out.append("\n### 3. 페이지 응답 시간과 바이트 (각 %d회)\n" % a.repeat)
    out.append("| limit | 위치 | after_id | 행 | has_more | 응답 바이트 | 중앙값 | p90 |")
    out.append("|---|---|---|---|---|---|---|---|")
    for limit, label, after, r in cases:
        out.append("| %d | %s | %d | %d | %s | %d | %.2f ms | %.2f ms |" % (
            limit, label, after, r["rows"], "true" if r["has_more"] else "false", r["bytes"], r["median_ms"],
            r["p90_ms"]))
    text = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    open(a.out, "w", encoding="utf-8").write(text)
    print("\n" + text)
    ok = sel["status"] == 400 and sel["code"] == "RESULT_TOO_LARGE" and walk["mismatch"] == 0 and \
        walk["rows"] == walk["want"]
    print("확인:", "통과" if ok else "실패")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
