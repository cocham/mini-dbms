#!/usr/bin/env python3
"""age 조건 페이지 조회 대표 실험 (Phase 6 / P6-03·P6-05, DB-P6-D11·D12·D17). Linux(Docker) 전용.

사용법:
  python3 tools/age_bench.py --server build/minidb-server
        [--rows 1000000] [--every 1000] [--warmup 5] [--repeat 30]
        [--order scan,index,index,scan] [--workdir /tmp/minidb_age] [--out build/age_result.md] [--commit 라벨]

데이터(변경 없음): id 1..rows, 이름 userNNNNNN.
  age 25: id를 every로 나눈 나머지가 1인 행 (기본 1, 1001, …, 999001 → 1,000행, 0.1%)
  나머지: age = 20 + (id mod 50) (조회 나이와 같으면 70으로 변경)
요청: GET /users?age=25&after_id=0&limit=100 (--age·--after·--limit로 바꿀 수 있다)

--order의 항목마다 같은 실행 파일·같은 데이터 디렉터리·같은 설정으로 서버를 새로 띄운다. 다른 것은 --age-path 하나다.
  scan|index  → --age-path scan|index 로 띄운다
  default     → --age-path 없이 띄운다(서버 기본값). 응답의 X-Access-Path로 실제 경로를 판단한다
실행마다: 워밍업 warmup회(측정에서 뺌) → repeat회 측정. 요청마다 새 연결(서버는 keep-alive 없음).

자동 검사(하나라도 어긋나면 종료 코드 1):
  - 응답 본문이 데이터에서 계산한 기대 본문과 바이트 단위로 같다(그래서 두 경로의 본문도 서로 같다)
  - 실제 경로(X-Access-Path)가 요청한 경로와 같다
  - 계측 헤더 E(X-Rows-Examined)·T(X-Rows-Examined-After-Last)·K(X-Age-Index-Keys)가 경로별 기대값과 같다
  - 같은 실행 안에서 모든 요청의 본문·계측이 같다

시간: 응답 시간은 이 스크립트가 잰다(연결 → 요청 보내기 → 응답 끝까지 받기, ms). 엔진 시간은 서버의
X-Engine-Us(db_page 호출 앞뒤, µs)다. 클라이언트와 서버가 같은 컨테이너에서 CPU를 나눠 쓴다.
숫자는 이 환경에서의 비교로만 읽는다.
"""
import argparse
import bisect
import os
import platform
import re
import socket
import statistics
import subprocess
import sys
import time

DATA_VERSION = "age-bench-v2"  # 생성 규칙이 바뀌면 올린다(같은 디렉터리의 옛 데이터를 다시 쓰지 않도록)


def age_of(i, every, age):
    if i % every == 1:
        return age
    filler_age = 20 + i % 50
    return 70 if filler_age == age else filler_age


def make_data(d, rows, every, age):
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "users.tsv")
    mark = os.path.join(d, "rows.txt")
    want_mark = "%s rows=%d every=%d age=%d" % (DATA_VERSION, rows, every, age)
    if os.path.exists(path) and os.path.exists(mark) and open(mark).read().strip() == want_mark:
        return path
    for name in ("users.next",):
        p = os.path.join(d, name)
        if os.path.exists(p):
            os.remove(p)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        buf = []
        for i in range(1, rows + 1):
            buf.append("%d\tuser%06d\t%d\n" % (i, i, age_of(i, every, age)))
            if len(buf) >= 20000:
                f.write("".join(buf))
                buf.clear()
        f.write("".join(buf))
    with open(mark, "w") as f:
        f.write(want_mark)
    return path


def expected(rows, every, age, after, limit):
    """데이터 규칙에서 기대값을 계산한다. (기대 본문, {경로: (E, T, K)}, 반환 id 목록, has_more)"""
    matches = [i for i in range(1, rows + 1) if age_of(i, every, age) == age]
    start = bisect.bisect_right(matches, after)
    picks = matches[start:start + limit]
    more = len(matches) > start + limit
    # 선형 탐색: after+1부터 limit+1번째로 맞는 행(있으면) 또는 끝까지 읽는다
    if more:
        stop = matches[start + limit]
    else:
        stop = rows
    e_scan = max(0, stop - after)  # id가 1..rows로 빈틈없으므로 줄 수 = id 차이
    t_scan = (stop - picks[-1]) if picks else e_scan
    r = len(picks)
    body = ('{"ok":true,"columns":["id","name","age"],"rows":[' +
            ",".join('[%d,"user%06d",%d]' % (i, i, age) for i in picks) +
            '],"has_more":%s,"next_cursor":%s}' % ("true" if more else "false", str(picks[-1]) if picks else "null"))
    return body.encode(), {"scan": (e_scan, t_scan, 0), "age-index": (r, 0, r + (1 if more else 0))}, picks, more


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
            self.p.kill()
            raise RuntimeError("서버가 시작하지 않음: " + open(log_path, encoding="utf-8", errors="replace").read())

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=120)
        self.log.close()


def http_get(port, path):
    """(상태, 헤더 dict, 본문 바이트, 걸린 ms). 연결부터 응답 끝까지 잰다."""
    req = ("GET %s HTTP/1.1\r\nHost: b\r\nContent-Length: 0\r\n\r\n" % path).encode()
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
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        headers[k.strip().lower()] = v.strip()
    if int(headers.get("content-length", "-1")) != len(body):
        raise RuntimeError("응답이 잘림: %r" % head[:200])
    return status, headers, body, ms


def metrics(h):
    return (int(h["x-rows-examined"]), int(h["x-rows-examined-after-last"]), int(h["x-age-index-keys"]))


def pct(xs, q):
    xs = sorted(xs)
    return xs[max(0, int(round(q * len(xs))) - 1)]


def one_run(args, data_dir, n, mode, path_qs, want_body, want_m, problems):
    extra = [] if mode == "default" else ["--age-path", mode]
    srv = Server(args.server, data_dir, os.path.join(args.workdir, "server_%d_%s.log" % (n, mode)), extra)
    try:
        for _ in range(args.warmup):
            http_get(srv.port, path_qs)
        times, engine, seen = [], [], set()
        last = None
        for _ in range(args.repeat):
            st, h, body, ms = http_get(srv.port, path_qs)
            if st != 200:
                raise RuntimeError("실행 %d: %d %s" % (n, st, body[:200]))
            times.append(ms)
            engine.append(int(h["x-engine-us"]))
            seen.add((body, metrics(h), h["x-access-path"]))
            last = (h, body)
    finally:
        srv.stop()
    h, body = last
    used = h["x-access-path"]
    m = metrics(h)
    if len(seen) != 1:
        problems.append("실행 %d(%s): 요청마다 본문·계측이 다름" % (n, mode))
    if mode == "scan" and used != "scan" or mode == "index" and used != "age-index":
        problems.append("실행 %d: --age-path %s인데 X-Access-Path가 %s" % (n, mode, used))
    if body != want_body:
        problems.append("실행 %d(%s): 응답 본문이 기대 본문과 다름" % (n, used))
    if used not in want_m:
        problems.append("실행 %d: 모르는 경로 %s" % (n, used))
    elif m != want_m[used]:
        problems.append("실행 %d(%s): 계측 E,T,K=%s, 기대 %s" % (n, used, m, want_m[used]))
    return {"n": n, "mode": mode, "used": used, "start": srv.start_line, "times": times, "engine": engine,
            "median_ms": statistics.median(times), "p90_ms": pct(times, 0.9), "min_ms": min(times),
            "engine_median_us": statistics.median(engine), "E": m[0], "T": m[1], "K": m[2],
            "bytes": len(body), "body": body}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--rows", type=int, default=1000000)
    ap.add_argument("--every", type=int, default=1000)
    ap.add_argument("--age", type=int, default=25)
    ap.add_argument("--after", type=int, default=0)
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--repeat", type=int, default=30)
    ap.add_argument("--order", default="scan,index,index,scan")
    ap.add_argument("--workdir", default="/tmp/minidb_age")
    ap.add_argument("--out", default="build/age_result.md")
    ap.add_argument("--commit", default="")
    a = ap.parse_args()
    order = [x.strip() for x in a.order.split(",") if x.strip()]
    if not order or any(x not in ("scan", "index", "default") for x in order):
        sys.exit("--order는 scan, index, default를 쉼표로 이은 것이어야 합니다")
    if not (0 <= a.age <= 150):
        sys.exit("--age는 0~150 값이어야 합니다")
    if a.repeat < 1 or a.warmup < 0 or a.every < 2 or a.rows < a.every:
        sys.exit("--repeat ≥ 1, --warmup ≥ 0, --every ≥ 2, --rows ≥ --every 이어야 합니다")

    data_dir = os.path.join(a.workdir, "data")
    os.makedirs(a.workdir, exist_ok=True)
    print("데이터 준비: %d행, age %d인 행은 id %% %d = 1인 행 (%s)" % (a.rows, a.age, a.every, data_dir), flush=True)
    t0 = time.time()
    make_data(data_dir, a.rows, a.every, a.age)
    print("  %.1f s" % (time.time() - t0), flush=True)
    want_body, want_m, picks, more = expected(a.rows, a.every, a.age, a.after, a.limit)
    nmatch = len(range(1, a.rows + 1, a.every))
    path_qs = "/users?age=%d&after_id=%d&limit=%d" % (a.age, a.after, a.limit)
    print("요청: GET %s" % path_qs)
    print("기대: R=%d has_more=%s next_cursor=%s | scan E,T,K=%s | age-index E,T,K=%s" % (
        len(picks), more, picks[-1] if picks else None, want_m["scan"], want_m["age-index"]), flush=True)

    problems, runs = [], []
    for n, mode in enumerate(order, 1):
        r = one_run(a, data_dir, n, mode, path_qs, want_body, want_m, problems)
        runs.append(r)
        print("실행 %d %-7s 경로=%-9s 응답 중앙값 %.3f ms (p90 %.3f, 최소 %.3f) | 엔진 중앙값 %.0f µs | "
              "R=%d E=%d T=%d K=%d | 본문 %d B" % (r["n"], r["mode"], r["used"], r["median_ms"], r["p90_ms"],
                                                  r["min_ms"], r["engine_median_us"], len(picks), r["E"], r["T"],
                                                  r["K"], r["bytes"]), flush=True)
    bodies_same = len(set(r["body"] for r in runs)) == 1
    if not bodies_same:
        problems.append("실행 사이에 응답 본문이 다름")

    # ---- 결과 문서 ----
    out = []
    out.append("## age 조건 페이지 조회 대표 실험\n")
    out.append("| 항목 | 값 |")
    out.append("|---|---|")
    out.append("| 커밋(빌드) | %s |" % (a.commit or "(기록 안 함: --commit)"))
    out.append("| 환경 | %s, CPU %s개, Python %s |" % (platform.platform(), os.cpu_count(), platform.python_version()))
    out.append("| 데이터 | %d행, 변경 없음, `%s/users.tsv` |" % (a.rows, data_dir))
    out.append("| age %d 행 | %d행 (%.2f%%), id %% %d = 1 (1, %d, …), 나머지는 age 20~69에서 조회 나이와 겹치면 70으로 변경 |" % (
        a.age, nmatch, 100.0 * nmatch / a.rows, a.every, 1 + a.every))
    out.append("| 요청 | `GET %s` |" % path_qs)
    out.append("| 서버 | 같은 실행 파일·데이터·기본 설정, 실행마다 새로 띄움. 다른 것은 `--age-path`뿐 |")
    out.append("| 반복 | 실행마다 워밍업 %d회(제외) + 측정 %d회, 순서 %s |" % (a.warmup, a.repeat, " → ".join(order)))
    out.append("| 응답 시간 | 클라이언트(파이썬): 연결 → 요청 → 응답 끝까지, 요청마다 새 연결 |")
    out.append("| 엔진 시간 | 서버 `X-Engine-Us`: `db_page` 호출 앞뒤(잠금 대기·탐색·맞는 행 JSON 추가 포함) |")
    out.append("\n### 실행별\n")
    out.append("| 실행 | 옵션 | 실제 경로 | R | has_more | E 파일 행 확인 | T 마지막 반환 뒤 | K age 인덱스 항목 | "
               "응답 중앙값 | p90 | 최소 | 엔진 중앙값 | 본문 바이트 |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in runs:
        out.append("| %d | %s | %s | %d | %s | %d | %d | %d | %.3f ms | %.3f ms | %.3f ms | %.0f µs | %d |" % (
            r["n"], r["mode"], r["used"], len(picks), "true" if more else "false", r["E"], r["T"], r["K"],
            r["median_ms"], r["p90_ms"], r["min_ms"], r["engine_median_us"], r["bytes"]))
    out.append("\n### 경로별 (같은 경로의 실행에서 잰 값을 모두 합친 중앙값)\n")
    out.append("| 경로 | 실행 수 | 측정 수 | E | K | 응답 중앙값 | 엔진 중앙값 |")
    out.append("|---|---|---|---|---|---|---|")
    for used in ("scan", "age-index", "id"):
        rs = [r for r in runs if r["used"] == used]
        if not rs:
            continue
        ts = [t for r in rs for t in r["times"]]
        es = [e for r in rs for e in r["engine"]]
        out.append("| %s | %d | %d | %d | %d | %.3f ms | %.0f µs |" % (
            used, len(rs), len(ts), rs[0]["E"], rs[0]["K"], statistics.median(ts), statistics.median(es)))
    out.append("\n### 자동 검사\n")
    out.append("- 기대값(데이터에서 계산): R=%d, has_more=%s, next_cursor=%s, scan E,T,K=%s, age-index E,T,K=%s" % (
        len(picks), "true" if more else "false", picks[-1] if picks else None, want_m["scan"], want_m["age-index"]))
    out.append("- 응답 본문이 기대 본문과 바이트 단위로 같음: %s" % ("예" if not any("본문" in p for p in problems) else "아니오"))
    out.append("- 모든 실행의 응답 본문이 서로 같음: %s" % ("예" if bodies_same else "아니오"))
    out.append("- 계측이 기대값과 같음: %s" % ("예" if not any("계측" in p for p in problems) else "아니오"))
    for p in problems:
        out.append("- **문제:** " + p)
    out.append("\n서버 시작 줄:\n")
    for r in runs:
        out.append("- 실행 %d: `%s`" % (r["n"], r["start"]))
    text = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    open(a.out, "w", encoding="utf-8").write(text)
    print("\n" + text)
    print("확인:", "통과" if not problems else "실패 (%d건)" % len(problems))
    sys.exit(0 if not problems else 1)


if __name__ == "__main__":
    main()
