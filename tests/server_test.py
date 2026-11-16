#!/usr/bin/env python3
"""API 서버 테스트 (Phase 3, POSIX 전용).

사용법: server_test.py <minidb-server> <minidb-server-test> <minidb>
  minidb-server-test: MINIDB_TESTING 빌드. MINIDB_TEST_FAULT=index|write로 저장 후 실패를 재현한다.
  minidb: 같은 데이터 디렉터리를 CLI로 읽어 서버가 쓴 파일을 확인한다.
환경 변수 MINIDB_SERVER_WRAP: 서버 명령 앞에 붙일 것(TSan의 setarch -R 등).
"""
import json
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SERVER, SERVER_TEST, CLI = (os.path.abspath(p) for p in sys.argv[1:4])
WRAP = shlex.split(os.environ.get("MINIDB_SERVER_WRAP", ""))
TMP = tempfile.mkdtemp(prefix="minidb_server_test_")
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Fail(Exception):
    pass


def eq(actual, expected, what=""):
    if actual != expected:
        raise Fail("%s\n    기대: %r\n    실제: %r" % (what, expected, actual))


def ok(cond, what):
    if not cond:
        raise Fail(what)


# ---------- 서버 띄우기 ----------

class Server:
    def __init__(self, data_dir, *args, binary=None, env=None):
        e = dict(os.environ)
        if env:
            e.update(env)
        self.proc = subprocess.Popen(WRAP + [binary or SERVER, "--data-dir", data_dir, "--port", "0"] + list(args),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=e)
        self.log = []
        self.port = None
        deadline = time.time() + 30
        while time.time() < deadline:
            line = self.proc.stderr.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace")
            self.log.append(text)
            m = re.search(r"\[서버\] 시작: .*:(\d+),", text)
            if m:
                self.port = int(m.group(1))
                break
        if self.port is None:
            self.proc.kill()
            rest = self.proc.stderr.read().decode("utf-8", "replace")
            raise Fail("서버가 시작하지 않음: " + "".join(self.log) + rest)
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

    def _drain(self):
        for line in self.proc.stderr:
            self.log.append(line.decode("utf-8", "replace"))

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        code = self.proc.wait(timeout=30)
        self.reader.join(timeout=5)
        return code

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.proc.poll() is None:
            self.stop()


def run_start_fail(data_dir, *args, binary=None):
    p = subprocess.run(WRAP + [binary or SERVER, "--data-dir", data_dir] + list(args),
                       capture_output=True, timeout=30)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def fresh(name):
    d = os.path.join(TMP, name)
    shutil.rmtree(d, ignore_errors=True)
    return d


# ---------- HTTP (날것의 소켓) ----------

def raw(port, data, read=True):
    s = socket.create_connection(("127.0.0.1", port), timeout=15)
    try:
        s.sendall(data)
        if not read:
            return b""
        chunks = []
        while True:
            c = s.recv(65536)
            if not c:
                break
            chunks.append(c)
        return b"".join(chunks)
    finally:
        s.close()


def parse(resp):
    head, _, body = resp.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        headers[k.strip().lower()] = v.strip()
    eq(int(headers.get("content-length", "-1")), len(body), "Content-Length")
    return status, headers, body


def request(port, method, path, body=b"", extra=b""):
    if isinstance(body, str):
        body = body.encode("utf-8")
    req = ("%s %s HTTP/1.1\r\nHost: t\r\nContent-Length: %d\r\n" % (method, path, len(body))).encode() + extra + b"\r\n" + body
    status, headers, data = parse(raw(port, req))
    return status, headers, data


def query(port, sql):
    status, headers, data = request(port, "POST", "/query", sql)
    eq(headers.get("content-type"), "application/json; charset=utf-8", "Content-Type")
    return status, json.loads(data.decode("utf-8"))


def ins(name, age):
    return "INSERT INTO users (name, age) VALUES ('%s', %d);" % (name.replace("'", "''"), age)


# ---------- 기본 동작 ----------

@test
def health_and_empty_body():
    with Server(fresh("health")) as s:
        st, h, body = request(s.port, "GET", "/health")
        eq((st, json.loads(body)), (200, {"ok": True, "state": "ok", "rows": 0}))
        eq(query(s.port, ""), (200, {"ok": True, "results": []}), "빈 본문")
        eq(query(s.port, "  -- 주석만\n"), (200, {"ok": True, "results": []}), "주석만")


@test
def insert_select_explain_match_cli():
    d = fresh("basic")
    sql = "".join([ins("민수", 25), ins("Kim", 30), ins("지영", 25)])
    with Server(d) as s:
        st, r = query(s.port, sql)
        eq(st, 200)
        eq(r, {"ok": True, "results": [{"type": "insert", "id": 1}, {"type": "insert", "id": 2},
                                       {"type": "insert", "id": 3}]})
        st, r = query(s.port, "SELECT * FROM users WHERE id = 2;\n"
                              "SELECT * FROM users WHERE age = 25;\n"
                              "SELECT * FROM users WHERE id BETWEEN 2 AND 9;\n"
                              "SELECT * FROM users WHERE id = 99;\n"
                              "EXPLAIN SELECT * FROM users WHERE id >= 2;\n"
                              "EXPLAIN SELECT * FROM users;")
        eq(st, 200)
        sel = lambda rows: {"type": "select", "columns": ["id", "name", "age"], "rows": rows}
        eq(r["results"], [
            sel([[2, "Kim", 30]]),
            sel([[1, "민수", 25], [3, "지영", 25]]),
            sel([[2, "Kim", 30], [3, "지영", 25]]),
            sel([]),
            {"type": "explain", "plan": "INDEX RANGE SCAN", "condition": "id >= 2", "tree_height": 1, "total_rows": 3},
            {"type": "explain", "plan": "FULL SCAN", "condition": "조건 없음", "tree_height": 1, "total_rows": 3},
        ])
        ok(any("POST /query → 200, 문장 6개" in ln for ln in s.log), "요청 로그: " + "".join(s.log))
    # 서버가 쓴 파일을 CLI가 그대로 읽는다
    q = os.path.join(TMP, "q.sql")
    open(q, "w", encoding="utf-8").write("SELECT * FROM users;")
    p = subprocess.run([CLI, "--data-dir", d, q], capture_output=True)
    eq(p.stdout.decode("utf-8"), "id\tname\tage\n1\t민수\t25\n2\tKim\t30\n3\t지영\t25\n", "CLI로 읽기")


@test
def query_string_ignored_and_routes():
    with Server(fresh("routes")) as s:
        eq(query(s.port, "")[0], 200)
        st, h, body = request(s.port, "POST", "/query?x=1", ins("a", 1))
        eq(st, 200, "쿼리 문자열 무시")
        st, h, body = request(s.port, "GET", "/query")
        eq((st, h.get("allow"), json.loads(body)["error"]["code"]), (405, "POST", "METHOD"))
        st, h, body = request(s.port, "POST", "/health")
        eq((st, h.get("allow")), (405, "GET"))
        st, h, body = request(s.port, "GET", "/nope")
        eq((st, json.loads(body)["error"]["code"]), (404, "NOT_FOUND"))


# ---------- 전체 파싱 · 부분 실행 ----------

@test
def syntax_error_executes_nothing():
    with Server(fresh("syntax")) as s:
        st, r = query(s.port, ins("a", 1) + ins("b", 2) + "SELEC * FROM users;" + ins("c", 3))
        eq(st, 400)
        eq(r["ok"], False)
        eq((r["error"]["statement"], r["error"]["code"], r["results"]), (3, "SQL_SYNTAX", []))
        st, r = query(s.port, "SELECT * FROM users;")
        eq(r["results"][0]["rows"], [], "아무것도 저장되지 않음")
        st, r = query(s.port, "SELECT * FROM users\x00;")
        eq((st, r["error"]["code"]), (400, "SQL_SYNTAX"), "NUL")


@test
def runtime_error_keeps_earlier_results():
    with Server(fresh("runtime")) as s:
        st, r = query(s.port, ins("a", 1) + ins("b", 200) + ins("c", 3))
        eq(st, 400)
        eq((r["error"]["statement"], r["error"]["code"]), (2, "SQL_ERROR"))
        ok("150" in r["error"]["message"], r["error"]["message"])
        ok("stored_id" not in r["error"], "실행 전 실패에는 stored_id 없음")
        eq(r["results"], [{"type": "insert", "id": 1}])
        st, r = query(s.port, "SELECT * FROM users;")
        eq(r["results"][0]["rows"], [[1, "a", 1]], "앞 INSERT만 저장")


@test
def json_escaping_and_invalid_utf8():
    with Server(fresh("escape")) as s:
        body = (ins('따옴표"역슬래시\\', 1) + ins("ctl\x01x", 2)).encode("utf-8")
        body += b"INSERT INTO users (name, age) VALUES ('bad\xffbyte', 3);"
        st, h, data = request(s.port, "POST", "/query", body)
        eq(st, 200)
        st, r = query(s.port, "SELECT * FROM users;")
        eq([row[1] for row in r["results"][0]["rows"]], ['따옴표"역슬래시\\', "ctl\x01x", "bad�byte"])
        st, h, data = request(s.port, "POST", "/query", "SELECT * FROM users WHERE id = 2;")
        ok(b"ctl\\u0001x" in data, "제어 문자 이스케이프: %r" % data)


# ---------- 응답 예산 ----------

@test
def row_budget_counts_whole_request():
    with Server(fresh("rows"), "--max-rows", "3") as s:
        query(s.port, "".join(ins("u%d" % i, i) for i in range(1, 5)))
        st, r = query(s.port, "SELECT * FROM users WHERE id <= 2; SELECT * FROM users WHERE id >= 3;")
        eq(st, 400)
        eq((r["error"]["statement"], r["error"]["code"]), (2, "RESULT_TOO_LARGE"))
        ok("3행" in r["error"]["message"], r["error"]["message"])
        eq(r["results"], [{"type": "select", "columns": ["id", "name", "age"], "rows": [[1, "u1", 1], [2, "u2", 2]]}],
           "넘긴 SELECT는 통째로 버리고 앞 결과만")
        st, r = query(s.port, "SELECT * FROM users;")
        eq((st, r["error"]["statement"], r["results"]), (400, 1, []))
        st, r = query(s.port, "SELECT * FROM users WHERE id <= 3;")
        eq(st, 200, "딱 상한까지는 성공")


@test
def byte_budget_and_insert_reservation():
    with Server(fresh("bytes"), "--max-response", "4096") as s:
        # INSERT 결과만으로 예산을 채운다: 결과를 쓸 자리가 없으면 그 INSERT는 실행하지 않는다
        st, h, data = request(s.port, "POST", "/query", "".join(ins("n%d" % i, 1) for i in range(300)))
        ok(len(data) <= 4096, "응답 %d바이트" % len(data))
        r = json.loads(data)
        eq((st, r["error"]["code"]), (400, "RESULT_TOO_LARGE"))
        executed = len(r["results"])
        eq(r["error"]["statement"], executed + 1)
        eq([x["id"] for x in r["results"]], list(range(1, executed + 1)))
        st, h, data = request(s.port, "GET", "/health")
        eq(json.loads(data)["rows"], executed, "응답에 보인 INSERT만 저장됨")
        # 큰 SELECT는 상한을 넘기면 결과를 버린다
        query(s.port, "".join(ins("x" * 200, 2) for _ in range(30)))
        st, h, data = request(s.port, "POST", "/query", "SELECT * FROM users WHERE id = 1; SELECT * FROM users;")
        r = json.loads(data)
        ok(len(data) <= 4096, "응답 %d바이트" % len(data))
        eq((st, r["error"]["statement"], r["error"]["code"]), (400, 2, "RESULT_TOO_LARGE"))
        ok("4096" in r["error"]["message"], r["error"]["message"])
        eq(len(r["results"]), 1)


# ---------- HTTP 오류 ----------

@test
def http_level_errors():
    with Server(fresh("http")) as s:
        st, h, b = parse(raw(s.port, b"POST /query HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n"))
        eq((st, json.loads(b)["error"]["code"]), (413, "BODY_TOO_LARGE"))
        st, h, b = parse(raw(s.port, b"GET /health HTTP/1.1\r\nX-Big: " + b"a" * 9000 + b"\r\n\r\n"))
        eq((st, json.loads(b)["error"]["code"]), (431, "HEADER_TOO_LARGE"))
        st, h, b = parse(raw(s.port, b"HELLO\r\n\r\n"))
        eq((st, json.loads(b)["error"]["code"]), (400, "BAD_REQUEST"))
        st, h, b = parse(raw(s.port, b"POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"))
        eq((st, json.loads(b)["error"]["code"]), (400, "BAD_REQUEST"))
        st, h, b = parse(raw(s.port, b"POST /query HTTP/1.1\r\nContent-Length: 1x\r\n\r\n"))
        eq(st, 400)
        st, h, b = parse(raw(s.port, b"POST /query HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd"))
        eq(st, 400, "서로 다른 Content-Length")
        # LF만 쓴 요청과 HTTP/1.0도 받는다
        st, h, b = parse(raw(s.port, b"GET /health HTTP/1.0\n\n"))
        eq(st, 200)
        eq(h.get("connection"), "close")


@test
def expect_100_continue():
    with Server(fresh("expect")) as s:
        body = ins("e", 5).encode()
        sock = socket.create_connection(("127.0.0.1", s.port), timeout=15)
        sock.sendall(b"POST /query HTTP/1.1\r\nContent-Length: %d\r\nExpect: 100-continue\r\n\r\n" % len(body))
        first = sock.recv(100)
        eq(first, b"HTTP/1.1 100 Continue\r\n\r\n")
        sock.sendall(body)
        data = b""
        while True:
            c = sock.recv(65536)
            if not c:
                break
            data += c
        sock.close()
        st, h, b = parse(data)
        eq((st, json.loads(b)["results"]), (200, [{"type": "insert", "id": 1}]))


@test
def client_disconnect_does_not_stop_server():
    with Server(fresh("disconnect")) as s:
        raw(s.port, b"POST /query HTTP/1.1\r\nContent-Length: 100\r\n\r\nSELECT", read=False)
        sock = socket.create_connection(("127.0.0.1", s.port))
        sock.close()
        eq(query(s.port, "SELECT * FROM users;")[0], 200)


# ---------- 종료 · 재시작 · 사용법 ----------

@test
def shutdown_and_restart_keep_data():
    d = fresh("restart")
    s = Server(d)
    query(s.port, ins("a", 1) + ins("b", 2))
    eq(s.stop(), 0, "SIGTERM이면 종료 코드 0")
    ok(any("[서버] 종료" in ln for ln in s.log), "종료 줄")
    with Server(d) as s2:
        st, r = query(s2.port, "SELECT * FROM users;")
        eq(r["results"][0]["rows"], [[1, "a", 1], [2, "b", 2]])
        eq(query(s2.port, ins("c", 3))[1]["results"], [{"type": "insert", "id": 3}], "다음 id 이어짐")


@test
def usage_and_start_errors():
    d = fresh("usage")
    for args in (["--port", "70000"], ["--port", "x"], ["--max-rows", "0"], ["--max-response", "100"],
                 ["--host", "localhost"], ["--threads", "4"], ["--data-dir"]):
        code, out, err = run_start_fail(d, *args)
        eq(code, 2, "사용법 오류 %r: %s" % (args, err))
        ok("사용법" in err, err)
    p = subprocess.run(WRAP + [SERVER, "--help"], capture_output=True, timeout=30)
    eq(p.returncode, 0)
    f = os.path.join(TMP, "not_a_dir")
    open(f, "w").close()
    code, out, err = run_start_fail(f, "--port", "0")
    eq(code, 1, "시작 실패: " + err)


# ---------- 저장 후 실패 (테스트 전용 빌드) ----------

@test
def index_failure_halts_and_restart_recovers():
    d = fresh("halt_index")
    s = Server(d, binary=SERVER_TEST, env={"MINIDB_TEST_FAULT": "index"})
    st, r = query(s.port, "".join(ins("u%d" % i, 1) for i in range(1, 64)))
    eq((st, len(r["results"])), (200, 63))
    st, r = query(s.port, ins("first", 1) + ins("lost-index", 2))
    eq(st, 500)
    eq((r["error"]["statement"], r["error"]["code"], r["error"]["stored_id"]), (1, "INDEX_FAILED", 64))
    eq(r["results"], [])
    st, r = query(s.port, "SELECT * FROM users;")
    eq((st, r["error"]["code"], r["results"]), (503, "HALTED", []))
    ok("index_failed" in r["error"]["message"], r["error"]["message"])
    st, h, b = request(s.port, "GET", "/health")
    eq((st, json.loads(b)), (503, {"ok": False, "state": "halted", "reason": "index_failed"}))
    eq(s.stop(), 0)
    with Server(d) as s2:
        st, r = query(s2.port, "SELECT * FROM users WHERE id = 64; SELECT * FROM users WHERE id = 65;")
        eq(r["results"][0]["rows"], [[64, "first", 1]], "기록 완료된 줄은 재시작 뒤 인덱스로 보임")
        eq(r["results"][1]["rows"], [], "그 뒤 문장은 실행되지 않음")


@test
def write_failure_halts_and_restart_is_refused():
    d = fresh("halt_write")
    s = Server(d, binary=SERVER_TEST, env={"MINIDB_TEST_FAULT": "write"})
    st, r = query(s.port, ins("w", 1))
    eq((st, r["error"]["code"]), (500, "WRITE_FAILED"))
    ok("stored_id" not in r["error"], "쓰기 결과를 모르면 stored_id 없음")
    st, r = query(s.port, "SELECT * FROM users;")
    eq((st, r["error"]["code"]), (503, "HALTED"))
    st, h, b = request(s.port, "GET", "/health")
    eq(json.loads(b)["reason"], "write_failed")
    eq(s.stop(), 0)
    code, out, err = run_start_fail(d, "--port", "0")
    eq(code, 1)
    ok("데이터 파일 손상" in err, err)


def main():
    failed = 0
    for fn in TESTS:
        try:
            fn()
            print("PASS", fn.__name__)
        except Exception as e:  # noqa: BLE001
            failed += 1
            print("FAIL", fn.__name__, "-", e)
    shutil.rmtree(TMP, ignore_errors=True)
    print("서버 테스트: %d개 중 %d개 실패" % (len(TESTS), failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
