#!/usr/bin/env python3
"""읽기 병렬화 측정 (Phase 3 / P3-04). Linux(Docker) 전용.

사용법: python3 tools/read_parallel.py --rw build/minidb-server --global build/minidb-server-global
                                       [--rows 1000000] [--clients 4] [--threads 4] [--workdir build/readpar]

같은 데이터로 두 서버를 차례로 띄워 비교한다.
  rw      정상 빌드: SELECT끼리 공유 잠금(동시에 실행)
  global  -DMINIDB_GLOBAL_LOCK 빌드: SELECT도 배타 잠금(P3-03까지의 전역 잠금과 같은 동작)

측정 (모두 HTTP로, 서버 워커 --threads개):
  1) 선형 탐색 1개만 보냈을 때 응답 시간
  2) 선형 탐색 --clients개를 동시에 보냈을 때 전체 걸린 시간(마지막 응답까지)
  3) 선형 탐색이 계속 들어오는 동안 보낸 INSERT의 응답 시간(writer 기아 관찰)

선형 탐색은 `SELECT * FROM users WHERE age = 150;`(FULL SCAN)이다. 결과는 rows/151행 정도라 응답 예산 안이다.
데이터는 users.tsv를 직접 쓴다(형식은 Phase 1과 같다: id TAB name TAB age LF).
결과는 마크다운으로 stdout에 쓴다. docs/phase3/benchmark.md(P3-05)로 옮겨 적는다.
"""
import argparse
import os
import re
import shutil
import socket
import statistics
import subprocess
import sys
import threading
import time

SCAN = b"SELECT * FROM users WHERE age = 150;"


def post(port, body, timeout=120):
    req = (b"POST /query HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(body)) + body
    t = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
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
    resp = b"".join(chunks)
    status = int(resp.split(b" ", 2)[1])
    if status != 200:
        raise RuntimeError("HTTP %d: %r" % (status, resp[:300]))
    return time.perf_counter() - t


def make_data(d, rows):
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "users.tsv")
    mark = os.path.join(d, "rows.txt")  # 같은 행 수로 이미 만들었으면 다시 쓰지 않는다
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


class Server:
    def __init__(self, binary, data_dir, threads):
        self.p = subprocess.Popen([binary, "--data-dir", data_dir, "--port", "0", "--threads", str(threads)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.start_line = ""
        while True:
            line = self.p.stderr.readline().decode("utf-8", "replace")
            if not line:
                raise RuntimeError("서버가 시작하지 않음")
            m = re.search(r"\[서버\] 시작: .*:(\d+),", line)
            if m:
                self.port = int(m.group(1))
                self.start_line = line.strip()
                break
        threading.Thread(target=lambda: self.p.stderr.read(), daemon=True).start()

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=60)


def concurrent(port, n, body):
    times = [None] * n
    errs = []

    def one(i):
        try:
            times[i] = post(port, body)
        except Exception as e:  # noqa: BLE001
            errs.append(e)
    ths = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    t = time.perf_counter()
    for th in ths:
        th.start()
    for th in ths:
        th.join()
    if errs:
        raise errs[0]
    return time.perf_counter() - t, times


INSERT_GIVE_UP = 20.0  # 이 시간 안에 INSERT 응답이 없으면 "기아"로 기록하고 그만둔다


def insert_under_load(port, clients, seconds):
    """clients개 스레드가 선형 탐색을 쉬지 않고 보내는 동안 0.2초마다 INSERT를 보내 응답 시간을 잰다.
    INSERT 하나가 INSERT_GIVE_UP초 안에 끝나지 않으면 그 값(무한대)을 남기고 멈춘다."""
    stop = threading.Event()
    errs = []

    def scanner():
        try:
            while not stop.is_set():
                post(port, SCAN)
        except Exception as e:  # noqa: BLE001
            errs.append(e)
    ths = [threading.Thread(target=scanner) for _ in range(clients)]
    for th in ths:
        th.start()
    time.sleep(0.5)
    lat = []
    end = time.perf_counter() + seconds
    i = 0
    while time.perf_counter() < end:
        try:
            lat.append(post(port, b"INSERT INTO users (name, age) VALUES ('w%d', 1);" % i, timeout=INSERT_GIVE_UP))
        except socket.timeout:
            lat.append(float("inf"))
            break
        i += 1
        time.sleep(0.2)
    stop.set()
    for th in ths:
        th.join()
    if errs:
        raise errs[0]
    return lat


def measure(binary, data_dir, clients, threads, repeat):
    s = Server(binary, data_dir, threads)
    try:
        post(s.port, SCAN)  # 페이지 캐시 데우기
        single = statistics.median(post(s.port, SCAN) for _ in range(repeat))
        walls = []
        for _ in range(repeat):
            wall, _ = concurrent(s.port, clients, SCAN)
            walls.append(wall)
        idle_ins = statistics.median(post(s.port, b"INSERT INTO users (name, age) VALUES ('idle', 1);")
                                     for _ in range(5))
        lat = insert_under_load(s.port, clients, 4.0)
        return {
            "start": s.start_line,
            "single": single,
            "wall": statistics.median(walls),
            "idle_ins": idle_ins,
            "ins_med": statistics.median(lat),
            "ins_max": max(lat),
            "ins_n": len(lat),
        }
    finally:
        s.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rw", required=True)
    ap.add_argument("--global", dest="glob", required=True)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--workdir", default="build/readpar")
    a = ap.parse_args()

    base = os.path.join(a.workdir, "base")
    t = time.perf_counter()
    make_data(base, a.rows)
    gen_s = time.perf_counter() - t
    res = {}
    for name, binary in (("global", a.glob), ("rw", a.rw)):
        d = os.path.join(a.workdir, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        shutil.copy(os.path.join(base, "users.tsv"), d)  # INSERT 측정이 데이터를 늘리므로 매번 복사본으로
        res[name] = measure(os.path.abspath(binary), d, a.clients, a.threads, a.repeat)

    def ms(v):
        if v == float("inf"):
            return "%.0f초 넘게 응답 없음" % INSERT_GIVE_UP
        return "%.0f ms" % (v * 1000) if v >= 0.01 else "%.2f ms" % (v * 1000)
    print("## 읽기 병렬화 측정 (P3-04)\n")
    print("- 행 수 %d, 동시 클라이언트 %d, 서버 워커 %d, 반복 %d회 중앙값, CPU %d개(os.cpu_count), 데이터 준비 %.1f초"
          % (a.rows, a.clients, a.threads, a.repeat, os.cpu_count() or 0, gen_s))
    print("- 선형 탐색: `%s`\n" % SCAN.decode())
    print("| 항목 | 전역 잠금 (P3-03까지) | 읽기·쓰기 잠금 (P3-04) |")
    print("|---|---|---|")
    g, r = res["global"], res["rw"]
    print("| 선형 탐색 1개 응답 | %s | %s |" % (ms(g["single"]), ms(r["single"])))
    print("| 선형 탐색 %d개 동시, 전체 시간 | %s | %s |" % (a.clients, ms(g["wall"]), ms(r["wall"])))
    print("| └ 1개 대비 | %.1f배 | %.1f배 |" % (g["wall"] / g["single"], r["wall"] / r["single"]))
    print("| INSERT 응답 (부하 없음) | %s | %s |" % (ms(g["idle_ins"]), ms(r["idle_ins"])))
    print("| INSERT 응답 (선형 탐색 %d개가 계속 도는 중), 중앙값 / 최대 (%d·%d회) | %s / %s | %s / %s |"
          % (a.clients, g["ins_n"], r["ins_n"], ms(g["ins_med"]), ms(g["ins_max"]), ms(r["ins_med"]), ms(r["ins_max"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
