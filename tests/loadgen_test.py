#!/usr/bin/env python3
"""부하 생성기 회귀 테스트 (Phase 3 / P3-05, POSIX 전용).

사용법: loadgen_test.py <loadgen>

가짜 서버(파이썬)가 정해진 방식으로 답하게 하고, loadgen의 집계가 맞는지 본다.
  - 정상 응답만 성공으로 센다. 잘린 200 응답·Content-Length 없는 응답은 '불완전'이다
  - 측정 구간이 끝난 뒤 완료된 요청은 성공에도 처리량에도 넣지 않는다('늦음')
  - 처리량 = 구간 안 성공 수 / 구간 길이 (tools/load.py의 parse_loadgen)
P3-05 Codex 재검증에서 재현된 두 문제(잘린 응답을 성공 처리, 구간 뒤 완료를 성공 수에 넣고 고정 시간으로 나눔)의 회귀 검사다.
"""
import os
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from load import parse_loadgen  # noqa: E402

LOADGEN = os.path.abspath(sys.argv[1])
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Fail(Exception):
    pass


def eq(actual, expected, what):
    if actual != expected:
        raise Fail("%s\n    기대: %r\n    실제: %r" % (what, expected, actual))


def ok(cond, what):
    if not cond:
        raise Fail(what)


def read_request(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            return False
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    n = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1])
    while len(body) < n:
        chunk = c.recv(4096)
        if not chunk:
            return False
        body += chunk
    return True


def response(status, body, length=None, reason=b"OK"):
    head = b"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n" % (status, reason)
    if length is not False:
        head += b"Content-Length: %d\r\n" % (len(body) if length is None else length)
    return head + b"Connection: close\r\n\r\n" + body


class FakeServer:
    """연결마다 요청을 읽은 뒤 behave(conn)을 부르고 닫는다."""

    def __init__(self, behave):
        self.behave = behave
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", 0))
        self.s.listen(256)
        self.port = self.s.getsockname()[1]
        self.stop = False
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while not self.stop:
            try:
                c, _ = self.s.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        try:
            if read_request(c):
                self.behave(c)
        except OSError:
            pass
        finally:
            c.close()

    def close(self):
        self.stop = True
        self.s.close()


def run(behave, clients, seconds):
    srv = FakeServer(behave)
    try:
        t = time.time()
        p = subprocess.run([LOADGEN, str(srv.port), "id", str(clients), str(seconds), "100"],
                           capture_output=True, text=True, timeout=60)
        took = time.time() - t
    finally:
        srv.close()
    eq(p.returncode, 0, "loadgen 종료 코드: " + p.stderr)
    return parse_loadgen(p.stdout), took


BODY = b'{"ok":true,"results":[]}'


@test
def complete_responses_are_success():
    r, _ = run(lambda c: c.sendall(response(200, BODY)), 2, 0.3)
    ok(r["ok"] > 0, "정상 응답은 성공: %r" % r)
    eq((r["incomplete"], r["connfail"], r["other"], r["busy"]), (0, 0, 0, 0), "다른 분류 없음")
    eq(r["rps"], r["ok"] / 0.3, "처리량 = 성공 / 구간")


@test
def body_in_several_pieces_is_success():
    def behave(c):
        msg = response(200, b"x" * 5000)
        for i in range(0, len(msg), 700):  # 조각으로 나눠 보낸다
            c.sendall(msg[i:i + 700])
            time.sleep(0.001)
    r, _ = run(behave, 1, 0.3)
    ok(r["ok"] > 0 and r["incomplete"] == 0, "여러 조각으로 온 완전한 응답은 성공: %r" % r)


@test
def truncated_200_is_not_success():
    # Codex 재현: Content-Length 1000이라고 해 놓고 본문 1바이트만 보내고 닫는다
    r, _ = run(lambda c: c.sendall(response(200, b"x", length=1000)), 2, 0.2)
    eq(r["ok"], 0, "잘린 200은 성공이 아니다: %r" % r)
    ok(r["incomplete"] > 0, "불완전으로 센다: %r" % r)
    eq(r["rps"], 0.0, "처리량 0")


@test
def missing_content_length_is_not_success():
    r, _ = run(lambda c: c.sendall(response(200, BODY, length=False)), 1, 0.2)
    eq(r["ok"], 0, "Content-Length 없는 응답은 성공이 아니다: %r" % r)
    ok(r["incomplete"] > 0, "불완전으로 센다: %r" % r)


@test
def headers_cut_is_not_success():
    r, _ = run(lambda c: c.sendall(b"HTTP/1.1 200 OK\r\nContent-Le"), 1, 0.2)
    eq(r["ok"], 0, "헤더가 잘린 응답은 성공이 아니다: %r" % r)
    ok(r["incomplete"] > 0, "불완전으로 센다: %r" % r)


@test
def busy_is_counted_separately():
    r, _ = run(lambda c: c.sendall(response(503, b'{"ok":false}', reason=b"Service Unavailable")), 1, 0.2)
    eq(r["ok"], 0, "503은 성공이 아니다")
    ok(r["busy"] > 0, "503으로 센다: %r" % r)


@test
def completion_after_window_is_excluded():
    # Codex 재현: 0.1초 구간에 시작한 요청이 1초 뒤에 정상 응답 → 성공 0, 처리량 0, 늦음 1
    def slow(c):
        time.sleep(1.0)
        c.sendall(response(200, BODY))
    r, took = run(slow, 1, 0.1)
    eq((r["ok"], r["late"]), (0, 1), "구간 뒤 완료는 성공에서 뺀다: %r" % r)
    eq(r["rps"], 0.0, "처리량 0 (10건/초가 아님)")
    ok(took >= 1.0, "드레인은 기다리지만 집계하지 않는다: %.2f초" % took)


@test
def window_boundary_counts_only_completed_inside():
    # 응답마다 0.4초: 0.4·0.8초에 완료(구간 1초 안) → 성공 2, 1.2초 완료 → 늦음 1
    def slow(c):
        time.sleep(0.4)
        c.sendall(response(200, BODY))
    r, _ = run(slow, 1, 1.0)
    eq((r["ok"], r["late"]), (2, 1), "구간 안 완료만 성공: %r" % r)
    eq(r["rps"], 2.0, "처리량 = 2 / 1초")
    ok(0.39 <= r["p50"] <= 0.6, "응답 시간은 성공 요청만: %r" % r["p50"])


@test
def connection_refused_is_failure():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()  # 아무도 듣지 않는 포트
    p = subprocess.run([LOADGEN, str(port), "id", "1", "0.1", "100"], capture_output=True, text=True, timeout=30)
    r = parse_loadgen(p.stdout)
    eq(r["ok"], 0, "연결 실패는 성공이 아니다")
    ok(r["connfail"] > 0, "실패로 센다: %r" % r)


def main():
    failed = 0
    for fn in TESTS:
        try:
            fn()
            print("PASS", fn.__name__)
        except Exception as e:  # noqa: BLE001
            failed += 1
            print("FAIL", fn.__name__, "-", e)
    print("부하 생성기 테스트: %d개 중 %d개 실패" % (len(TESTS), failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
