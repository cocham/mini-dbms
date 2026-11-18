#!/usr/bin/env python3
"""API 서버 테스트 (Phase 3, POSIX 전용).

사용법: server_test.py <minidb-server> <minidb-server-test> <minidb>
  minidb-server-test: MINIDB_TESTING 빌드. MINIDB_TEST_FAULT=index|write로 저장 후 실패를 재현한다.
  minidb: 같은 데이터 디렉터리를 CLI로 읽어 서버가 쓴 파일을 확인한다.
환경 변수 MINIDB_SERVER_WRAP: 서버 명령 앞에 붙일 것(TSan의 setarch -R 등).
"""
import json
import os
import random
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
def bad_bytes_in_target_or_headers_execute_nothing():
    # 경로에 NUL이 있으면 문자열 비교가 "/query"까지만 보고 SQL을 실행하던 문제(P3-02 검증)
    d = fresh("badtarget")
    with Server(d) as s:
        eq(query(s.port, ins("before", 1))[0], 200)
        path = os.path.join(d, "users.tsv")
        with open(path, "rb") as f:
            before = f.read()
        body = ins("x", 2).encode("utf-8")
        cases = [
            b"POST /query\x00/invalid HTTP/1.1",
            b"POST /query\x00 HTTP/1.1",
            b"POST /query?a=\x00 HTTP/1.1",
            b"POST /query\x01 HTTP/1.1",
            b"POST /query\t HTTP/1.1",
            b"POST /query\x7f HTTP/1.1",
            b"POST /query\xff HTTP/1.1",
        ]
        for line in cases:
            req = line + b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body
            st, h, b = parse(raw(s.port, req))
            eq((st, json.loads(b)["error"]["code"]), (400, "BAD_REQUEST"), repr(line))
        for hdr in (b"X-A: a\x00b\r\n", b"X-A\x00: v\r\n", b"X-A: \x01\r\n"):
            req = b"POST /query HTTP/1.1\r\n" + hdr + b"Content-Length: %d\r\n\r\n" % len(body) + body
            st, h, b = parse(raw(s.port, req))
            eq((st, json.loads(b)["error"]["code"]), (400, "BAD_REQUEST"), repr(hdr))
        st, h, b = request(s.port, "GET", "/health")
        eq(json.loads(b)["rows"], 1, "INSERT가 실행되지 않음")
        st, h, b = request(s.port, "POST", "/query", ins("ok", 3), extra=b"X-Tab: a\tb\r\n")
        eq(st, 200, "헤더 값의 탭은 허용")
    with open(path, "rb") as f:
        after = f.read()
    ok(after.startswith(before) and after.count(b"\n") == before.count(b"\n") + 1,
       "거부된 요청은 파일을 바꾸지 않음")


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
                 ["--host", "localhost"], ["--threads", "0"], ["--threads", "257"], ["--queue", "0"],
                 ["--queue", "4097"], ["--queue", "x"], ["--data-dir"]):
        code, out, err = run_start_fail(d, *args)
        eq(code, 2, "사용법 오류 %r: %s" % (args, err))
        ok("사용법" in err, err)
    p = subprocess.run(WRAP + [SERVER, "--help"], capture_output=True, timeout=30)
    eq(p.returncode, 0)
    f = os.path.join(TMP, "not_a_dir")
    open(f, "w").close()
    code, out, err = run_start_fail(f, "--port", "0")
    eq(code, 1, "시작 실패: " + err)


# ---------- UPDATE·DELETE (Phase 4) ----------

@test
def update_delete_syntax_errors_execute_nothing():
    with Server(fresh("p4_syntax")) as s:
        # WHERE 없는 DELETE는 구문 오류 → 같은 요청의 앞 INSERT도 실행되지 않는다(전체 선파싱)
        st, r = query(s.port, ins("a", 1) + "DELETE FROM users;")
        eq((st, r["error"]["statement"], r["error"]["code"], r["results"]), (400, 2, "SQL_SYNTAX", []))
        ok("WHERE 조건이 필요" in r["error"]["message"], r["error"]["message"])
        st, r = query(s.port, ins("a", 1) + "UPDATE users SET id = 3 WHERE id = 1;")
        eq((st, r["error"]["statement"], r["error"]["code"]), (400, 2, "SQL_SYNTAX"))
        st, r = query(s.port, "SELECT * FROM users;")
        eq(r["results"][0]["rows"], [], "아무것도 저장되지 않음")


@test
def update_delete_json_results():
    # D5: {"type":"update","count":n}을 받고 이어서 SELECT로 바뀐 결과를 본다
    d = fresh("p4_json")
    with Server(d) as s:
        query(s.port, ins("a", 10) + ins("b", 20) + ins("c", 20) + ins("d", 30))
        st, r = query(s.port, "UPDATE users SET age = 21 WHERE age = 20;"
                              "UPDATE users SET name = 'O''Brien' WHERE id = 1;"
                              "DELETE FROM users WHERE id = 4;"
                              "DELETE FROM users WHERE id = 99;"
                              "SELECT * FROM users;")
        eq(st, 200)
        eq(r["results"][:4], [{"type": "update", "count": 2}, {"type": "update", "count": 1},
                              {"type": "delete", "count": 1}, {"type": "delete", "count": 0}])
        eq(r["results"][4]["rows"], [[1, "O'Brien", 10], [2, "b", 21], [3, "c", 21]])
        st, h, b = request(s.port, "GET", "/health")
        eq(json.loads(b)["rows"], 3)
        # 가장 큰 id(4)를 지웠으므로 다음 INSERT는 5
        st, r = query(s.port, ins("e", 1))
        eq(r["results"], [{"type": "insert", "id": 5}])
        # 실행 오류: 앞 UPDATE는 반영되고 응답에 남는다. 값 오류인 UPDATE는 아무것도 바꾸지 않는다
        st, r = query(s.port, "UPDATE users SET age = 1 WHERE id = 2; UPDATE users SET age = 151 WHERE id = 3;")
        eq((st, r["error"]["statement"], r["error"]["code"]), (400, 2, "SQL_ERROR"))
        eq(r["results"], [{"type": "update", "count": 1}])
    eq(cli_rows(d), ["1\tO'Brien\t10", "2\tb\t1", "3\tc\t21", "5\te\t1"], "재시작(CLI)해도 같음")


@test
def retried_delete_can_hit_rows_added_in_between():
    # spec "실패와 재시도": 같은 DELETE를 다시 보내는 것은 안전하지 않다. 사이에 들어온 행도 지우고 행 수도 달라진다
    with Server(fresh("p4_retry")) as s:
        query(s.port, ins("lee", 30) + ins("park", 30) + ins("kim", 25))
        st, r = query(s.port, "DELETE FROM users WHERE age = 30;")
        eq(r["results"], [{"type": "delete", "count": 2}])
        st, r = query(s.port, ins("new", 30))  # 다른 클라이언트
        eq(r["results"], [{"type": "insert", "id": 4}])
        st, r = query(s.port, "DELETE FROM users WHERE age = 30; SELECT * FROM users;")
        eq(r["results"][0], {"type": "delete", "count": 1}, "재시도가 새 행을 지움")
        eq(r["results"][1]["rows"], [[3, "kim", 25]])
        # 사이에 쓰기가 없으면 데이터는 같지만 행 수는 다르다(0)
        st, r = query(s.port, "DELETE FROM users WHERE age = 30;")
        eq(r["results"], [{"type": "delete", "count": 0}])


@test
def update_result_space_reserved_before_execution():
    # INSERT처럼 결과 자리를 먼저 확보한다: 응답에 빠진 UPDATE는 실행되지 않았다
    with Server(fresh("p4_reserve"), "--max-response", "4096") as s:
        query(s.port, ins("a", 1))
        n = 300
        st, h, data = request(s.port, "POST", "/query",
                              "".join("UPDATE users SET name = 'v%d' WHERE id = 1;" % i for i in range(n)))
        ok(len(data) <= 4096, "응답 %d바이트" % len(data))
        r = json.loads(data)
        eq((st, r["error"]["code"]), (400, "RESULT_TOO_LARGE"))
        executed = len(r["results"])
        ok(0 < executed < n, "실행된 문장 %d개" % executed)
        eq(r["error"]["statement"], executed + 1)
        st, r = query(s.port, "SELECT * FROM users;")
        eq(r["results"][0]["rows"], [[1, "v%d" % (executed - 1), 1]], "응답에 보인 마지막 UPDATE까지만 반영")


# ---------- 스레드 풀 · 큐 · 기한 · 종료 (P3-03) ----------

def cli_rows(d):
    q = os.path.join(TMP, "rows.sql")
    open(q, "w", encoding="utf-8").write("SELECT * FROM users;")
    p = subprocess.run([CLI, "--data-dir", d, q], capture_output=True, timeout=30)
    eq(p.returncode, 0, p.stderr.decode("utf-8", "replace"))
    return p.stdout.decode("utf-8").splitlines()[1:]


def read_all(sock, timeout):
    """닫힐 때까지 읽는다. 연결 재설정도 '닫힘'으로 본다."""
    sock.settimeout(timeout)
    chunks = []
    try:
        while True:
            c = sock.recv(65536)
            if not c:
                break
            chunks.append(c)
    except (ConnectionResetError, BrokenPipeError):
        pass
    return b"".join(chunks)


def timed_health(port):
    t = time.time()
    st, h, b = request(port, "GET", "/health")
    return st, time.time() - t


def in_thread(fn, *args):
    box = {}

    def run():
        try:
            box["value"] = fn(*args)
        except Exception as e:  # noqa: BLE001
            box["error"] = e
    th = threading.Thread(target=run, daemon=True)
    th.start()
    return th, box


@test
def slow_client_does_not_block_others():
    with Server(fresh("slow")) as s:
        ok("워커 4개, 큐 64" in "".join(s.log), "시작 줄: " + "".join(s.log))
        slow = socket.create_connection(("127.0.0.1", s.port))
        slow.sendall(b"POST /query HTTP/1.1\r\nContent-Length: 100\r\n\r\nSELECT")  # 나머지를 영영 안 보냄
        time.sleep(0.3)
        st, took = timed_health(s.port)
        eq(st, 200)
        ok(took < 2.0, "느린 연결이 있어도 다른 요청은 바로 처리: %.2f초" % took)
        eq(query(s.port, ins("a", 1))[0], 200)
        slow.close()


@test
def busy_is_immediate_when_queue_full():
    with Server(fresh("busy"), "--threads", "1", "--queue", "1") as s:
        hold = socket.create_connection(("127.0.0.1", s.port))  # 워커 하나를 붙잡는다(아무것도 안 보냄)
        time.sleep(0.3)
        queued = socket.create_connection(("127.0.0.1", s.port))  # 큐의 한 칸을 채운다
        queued.sendall(b"GET /health HTTP/1.1\r\n\r\n")
        time.sleep(0.3)
        t = time.time()
        extra = socket.create_connection(("127.0.0.1", s.port))  # 큐가 가득 참
        resp = read_all(extra, 10)
        took = time.time() - t
        st, h, b = parse(resp)
        eq((st, json.loads(b)), (503, {"ok": False, "error": {"code": "BUSY", "message": "서버가 바쁩니다"}}))
        ok(took < 2.0, "503은 기다리지 않음: %.2f초" % took)
        ok(len(resp) < 200, "고정 응답 200바이트 미만: %d" % len(resp))
        # 붙잡힌 워커는 호출 기한(5초)이 지나면 풀려나고, 큐에서 기다리던 요청이 처리된다
        st, h, b = parse(read_all(queued, 15))
        eq(st, 200, "큐에서 기다린 요청은 처리됨")
        hold.close()
        extra.close()
        queued.close()
    ok(any("503 BUSY" in ln for ln in s.log), "BUSY 로그")


@test
def concurrent_inserts_over_http():
    clients, per = 16, 15
    d = fresh("concurrent")
    with Server(d) as s:
        def worker(c):
            seen = []
            for i in range(per):
                name = "c%d_%d" % (c, i)
                st, r = query(s.port, ins(name, c))
                eq(st, 200, name)
                new_id = r["results"][0]["id"]
                # 성공 응답을 받은 INSERT는 그 뒤의 SELECT에 보인다 (C3)
                st, r = query(s.port, "SELECT * FROM users WHERE id = %d;" % new_id)
                eq(r["results"][0]["rows"], [[new_id, name, c]], "방금 넣은 행")
                seen.append(new_id)
            return seen
        threads = [in_thread(worker, c) for c in range(clients)]
        ids = []
        for th, box in threads:
            th.join(120)
            ok(not th.is_alive(), "클라이언트가 끝나지 않음")
            if "error" in box:
                raise box["error"]
            ids += box["value"]
        eq(sorted(ids), list(range(1, clients * per + 1)), "id 중복·빠짐 없음 (C1)")
        st, h, b = request(s.port, "GET", "/health")
        eq(json.loads(b)["rows"], clients * per)
    eq(len(cli_rows(d)), clients * per, "재시작(CLI)해도 같은 행 수 (C4)")


@test
def concurrent_mixed_writes_over_http():
    # D5: 여러 클라이언트가 INSERT·UPDATE·DELETE·SELECT를 섞는다. 각자 자기 행만 고치고 지우므로
    # 결과를 정확히 예상할 수 있다. 끝나면 각자의 기록과 실제 내용, 재시작 뒤 내용을 비교한다
    clients, ops = 8, 25
    d = fresh("p4_mixed")
    with Server(d) as s:
        def worker(c):
            rng = random.Random(c)
            mine = {}  # id → (name, age) 살아 있어야 할 행
            issued = []
            for i in range(ops):
                k = rng.random()
                if k < 0.4 or not mine:
                    name, age = "c%d_%d" % (c, i), rng.randint(0, 150)
                    st, r = query(s.port, ins(name, age))
                    eq(st, 200, name)
                    new_id = r["results"][0]["id"]
                    mine[new_id] = (name, age)
                    issued.append(new_id)
                    continue
                target = rng.choice(sorted(mine))
                if k < 0.6:
                    age = rng.randint(0, 150)
                    st, r = query(s.port, "UPDATE users SET age = %d WHERE id = %d;" % (age, target))
                    eq((st, r["results"]), (200, [{"type": "update", "count": 1}]), "UPDATE")
                    mine[target] = (mine[target][0], age)
                elif k < 0.75:
                    st, r = query(s.port, "DELETE FROM users WHERE id = %d;" % target)
                    eq((st, r["results"]), (200, [{"type": "delete", "count": 1}]), "DELETE")
                    del mine[target]
                    st, r = query(s.port, "DELETE FROM users WHERE id = %d;" % target)
                    eq(r["results"], [{"type": "delete", "count": 0}], "이미 지운 행")
                else:
                    st, r = query(s.port, "SELECT * FROM users WHERE id = %d;" % target)
                    name, age = mine[target]
                    eq(r["results"][0]["rows"], [[target, name, age]], "내 행")
            return mine, issued
        threads = [in_thread(worker, c) for c in range(clients)]
        alive, issued = {}, []
        for th, box in threads:
            th.join(180)
            ok(not th.is_alive(), "클라이언트가 끝나지 않음")
            if "error" in box:
                raise box["error"]
            alive.update(box["value"][0])
            issued += box["value"][1]
        eq(sorted(issued), list(range(1, len(issued) + 1)), "id 중복·빠짐 없음")
        want = ["%d\t%s\t%d" % (i, alive[i][0], alive[i][1]) for i in sorted(alive)]
        st, r = query(s.port, "SELECT * FROM users;")
        eq(["%d\t%s\t%d" % tuple(row) for row in r["results"][0]["rows"]], want, "최종 내용")
    eq(cli_rows(d), want, "재시작(CLI)해도 같음")


@test
def receive_deadlines():
    with Server(fresh("deadline")) as s:
        def stalled():  # 헤더 일부만 보내고 멈춤 → 호출 하나의 기한(5초)
            sock = socket.create_connection(("127.0.0.1", s.port))
            t = time.time()
            sock.sendall(b"GET /health HTTP/1.1\r\nX-A: ")
            resp = read_all(sock, 20)
            sock.close()
            return resp, time.time() - t

        def slowloris():  # 1초마다 1바이트씩 → 호출 기한은 안 넘지만 전체 기한(10초)에서 끊김
            import select
            sock = socket.create_connection(("127.0.0.1", s.port))
            t = time.time()
            payload = b"GET /health HTTP/1.1\r\nX-Pad: " + b"a" * 100
            i = 0
            while time.time() - t < 20:
                r, _, _ = select.select([sock], [], [], 1.0)
                if r:
                    break
                try:
                    sock.sendall(payload[i:i + 1])
                except (BrokenPipeError, ConnectionResetError):
                    break
                i += 1
            resp = read_all(sock, 5)
            sock.close()
            return resp, time.time() - t

        a = in_thread(stalled)
        b = in_thread(slowloris)
        time.sleep(1)
        st, took = timed_health(s.port)  # 느린 연결 둘이 있어도 다른 요청은 바로 처리
        eq(st, 200)
        ok(took < 2.0, "다른 요청 %.2f초" % took)
        for (th, box), lo, hi, what in ((a, 4.5, 8.0, "호출 기한"), (b, 9.5, 14.0, "전체 기한")):
            th.join(30)
            if "error" in box:
                raise box["error"]
            resp, took = box["value"]
            st, h, body = parse(resp)
            eq((st, json.loads(body)["error"]["code"]), (408, "TIMEOUT"), what)
            ok(lo <= took <= hi, "%s: %.2f초" % (what, took))


@test
def shutdown_finishes_running_and_rejects_queued():
    d = fresh("shutdown_queue")
    s = Server(d, "--threads", "1", "--queue", "4", binary=SERVER_TEST,
               env={"MINIDB_TEST_STMT_DELAY_MS": "300"})
    running = in_thread(query, s.port, ins("a1", 1) + ins("a2", 2) + ins("a3", 3))  # 약 0.9초 걸림
    time.sleep(0.2)
    queued = socket.create_connection(("127.0.0.1", s.port))
    body = ins("b", 9).encode()
    queued.sendall(b"POST /query HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    time.sleep(0.2)
    s.proc.send_signal(signal.SIGTERM)
    st, h, b = parse(read_all(queued, 10))
    eq((st, json.loads(b)["error"]["code"]), (503, "SHUTTING_DOWN"), "큐에서 기다리던 요청")
    queued.close()
    th, box = running
    th.join(30)
    if "error" in box:
        raise box["error"]
    st, r = box["value"]
    eq((st, [x["id"] for x in r["results"]]), (200, [1, 2, 3]), "처리 중이던 요청은 끝까지")
    eq(s.proc.wait(timeout=30), 0, "종료 코드 0")
    s.reader.join(timeout=5)
    ok(any("대기하던 연결 1개에 503" in ln for ln in s.log), "".join(s.log))
    eq(cli_rows(d), ["1\ta1\t1", "2\ta2\t2", "3\ta3\t3"], "큐에서 거절된 INSERT는 저장 안 됨")


@test
def shutdown_grace_stops_at_statement_boundary():
    d = fresh("shutdown_grace")
    s = Server(d, binary=SERVER_TEST, env={"MINIDB_TEST_GRACE_MS": "2000", "MINIDB_TEST_STMT_DELAY_MS": "1500"})
    # 문장 1은 1.5초, 문장 2는 3.0초에 시작할 차례. 유예는 신호(0.3초) + 2초 = 2.3초에 끝난다.
    sock = socket.create_connection(("127.0.0.1", s.port))
    body = (ins("s1", 1) + ins("s2", 2) + ins("s3", 3)).encode()
    sock.sendall(b"POST /query HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    time.sleep(0.3)
    t = time.time()
    s.proc.send_signal(signal.SIGTERM)
    eq(s.proc.wait(timeout=30), 0, "종료 코드 0")
    took = time.time() - t
    s.reader.join(timeout=5)
    eq(read_all(sock, 5), b"", "유예 뒤에는 연결을 끊는다(응답 없음 → 결과 불명)")
    sock.close()
    ok(1.8 <= took <= 6.0, "유예 2초 + 실행 중 문장: %.2f초" % took)
    ok(any("유예 시간이 지나 연결 1개를 끊음" in ln for ln in s.log), "".join(s.log))
    eq(cli_rows(d), ["1\ts1\t1"], "문장 경계에서 멈춤: 문장 1만 저장")


@test
def shutdown_grace_closes_slow_connection():
    s = Server(fresh("shutdown_slow"), binary=SERVER_TEST, env={"MINIDB_TEST_GRACE_MS": "500"})
    slow = socket.create_connection(("127.0.0.1", s.port))
    slow.sendall(b"POST /query HTTP/1.1\r\nContent-Length: 100\r\n\r\nSELECT")
    time.sleep(0.3)
    t = time.time()
    s.proc.send_signal(signal.SIGTERM)
    eq(s.proc.wait(timeout=30), 0)
    took = time.time() - t
    ok(took < 3.0, "느린 연결의 기한(5·10초)까지 기다리지 않음: %.2f초" % took)
    slow.close()


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
