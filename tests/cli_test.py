#!/usr/bin/env python3
"""CLI 통합 테스트.

실제 minidb 프로세스를 실행해 stdout, stderr, 종료 코드, 저장 파일을 바이트 단위로 확인한다.
각 테스트는 새 임시 디렉터리에서 실행한다.

사용법: python tests/cli_test.py <minidb 실행 파일>
"""
import os
import random
import re
import subprocess
import sys
import tempfile
import traceback

BIN = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else None
HEADER = b"id\tname\tage\n"
SANITIZER_MARKERS = (
    b"AddressSanitizer",
    b"LeakSanitizer",
    b"UndefinedBehaviorSanitizer",
    b"runtime error:",
)

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def u(s):
    return s.encode("utf-8")


class Env:
    """테스트 하나가 쓰는 임시 작업 공간."""

    def __init__(self, root):
        self.root = root
        self.data_dir = os.path.join(root, "data")
        self.data_file = os.path.join(self.data_dir, "users.tsv")
        self.counter = 0

    def sql_file(self, content):
        self.counter += 1
        path = os.path.join(self.root, "q%d.sql" % self.counter)
        with open(path, "wb") as f:
            f.write(content if isinstance(content, bytes) else u(content))
        return path

    def run_args(self, args):
        proc = subprocess.run([BIN] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              cwd=self.root, timeout=30)
        for marker in SANITIZER_MARKERS:
            assert marker not in proc.stderr, "검사 도구 보고:\n" + proc.stderr.decode("utf-8", "replace")
        return proc.returncode, proc.stdout, proc.stderr

    def run(self, sql):
        return self.run_args(["--data-dir", self.data_dir, self.sql_file(sql)])

    def data(self):
        if not os.path.exists(self.data_file):
            return b""
        with open(self.data_file, "rb") as f:
            return f.read()


def eq(actual, expected, what):
    assert actual == expected, "%s\n  기대: %r\n  실제: %r" % (what, expected, actual)


def expect_ok(result, stdout):
    code, out, err = result
    eq(err, b"", "stderr")
    eq(code, 0, "종료 코드")
    eq(out, stdout, "stdout")


def expect_fail(result, *needles, stdout=b""):
    code, out, err = result
    assert code != 0, "실패 종료 코드를 기대했지만 0"
    eq(out, stdout, "stdout")
    for needle in needles:
        assert u(needle) in err, "stderr에 %r 없음: %r" % (needle, err.decode("utf-8", "replace"))


TWO_INSERTS = (
    "INSERT INTO users (name, age) VALUES ('민수', 25);\n"
    "INSERT INTO users (age, name) VALUES (30, 'O''Brien');\n"
)
TWO_ROWS = u("1\t민수\t25\n2\tO'Brien\t30\n")


# ---------- A1: 추가 후 전체 조회 ----------

@test
def a1_insert_then_select(env):
    expect_ok(env.run(TWO_INSERTS + "SELECT * FROM users;\n"),
              b"INSERT 1\nINSERT 1\n" + HEADER + TWO_ROWS)
    eq(env.data(), TWO_ROWS, "저장 파일")


# ---------- A2: id / age 조건 조회 ----------

FOUR_PEOPLE = (
    "INSERT INTO users (name, age) VALUES ('민수', 25);"
    "INSERT INTO users (name, age) VALUES ('지영', 30);"
    "INSERT INTO users (name, age) VALUES ('O''Brien', 25);"
    "INSERT INTO users (name, age) VALUES ('현우', 40);"
)
ROW = {
    1: u("1\t민수\t25\n"),
    2: u("2\t지영\t30\n"),
    3: u("3\tO'Brien\t25\n"),
    4: u("4\t현우\t40\n"),
}


@test
def a2_where_id(env):
    expect_ok(env.run(FOUR_PEOPLE), b"INSERT 1\n" * 4)
    expect_ok(env.run("SELECT * FROM users WHERE id = 3;"), HEADER + ROW[3])        # 한 건
    expect_ok(env.run("SELECT * FROM users WHERE id = 1;"), HEADER + ROW[1])        # 첫 행
    expect_ok(env.run("SELECT * FROM users WHERE id = 4;"), HEADER + ROW[4])        # 마지막 행
    expect_ok(env.run("SELECT * FROM users WHERE id = 5;"), HEADER)                 # 없음
    expect_ok(env.run("SELECT * FROM users WHERE id = 0;"), HEADER)
    expect_ok(env.run("SELECT * FROM users WHERE id = -1;"), HEADER)


@test
def a2_where_age(env):
    expect_ok(env.run(FOUR_PEOPLE), b"INSERT 1\n" * 4)
    expect_ok(env.run("SELECT * FROM users WHERE age = 25;"), HEADER + ROW[1] + ROW[3])  # 여러 건, 저장 순서
    expect_ok(env.run("SELECT * FROM users WHERE age = 40;"), HEADER + ROW[4])
    expect_ok(env.run("SELECT * FROM users WHERE age = 31;"), HEADER)
    expect_ok(env.run("SELECT * FROM users WHERE age = 151;"), HEADER)
    expect_ok(env.run("SELECT * FROM users WHERE age = 4294967321;"), HEADER)    # 2^32 + 25: 잘려서 25가 되면 안 됨


@test
def a2_where_syntax_variants_and_mixed_statements(env):
    sql = (
        FOUR_PEOPLE
        + "select * from USERS where ID=2;\n"
        + "SELECT * FROM users\n  WHERE /* 조건 */ Age -- 나이\n  = 25\n;"
        + "INSERT INTO users (name, age) VALUES ('새사람', 25);"
        + "SELECT * FROM users WHERE age = 25;"
    )
    expect_ok(env.run(sql),
              b"INSERT 1\n" * 4
              + HEADER + ROW[2]
              + HEADER + ROW[1] + ROW[3]
              + b"INSERT 1\n"
              + HEADER + ROW[1] + ROW[3] + u("5\t새사람\t25\n"))


@test
def a2_where_empty_table(env):
    expect_ok(env.run("SELECT * FROM users WHERE id = 1; SELECT * FROM users WHERE age = 1;"),
              HEADER + HEADER)


@test
def a2_where_after_restart(env):
    expect_ok(env.run(FOUR_PEOPLE), b"INSERT 1\n" * 4)
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('다섯', 30);"), b"INSERT 1\n")
    expect_ok(env.run("SELECT * FROM users WHERE age = 30;"), HEADER + ROW[2] + u("5\t다섯\t30\n"))
    expect_ok(env.run("SELECT * FROM users WHERE id = 5;"), HEADER + u("5\t다섯\t30\n"))


# ---------- A3: 재실행 후 데이터 유지와 다음 ID ----------

@test
def a3_restart_keeps_data_and_next_id(env):
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    expect_ok(env.run("SELECT * FROM users;"), HEADER + TWO_ROWS)
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('지영', 41);\nSELECT * FROM users;"),
              b"INSERT 1\n" + HEADER + TWO_ROWS + u("3\t지영\t41\n"))
    eq(env.data(), TWO_ROWS + u("3\t지영\t41\n"), "저장 파일")


# ---------- A4: 한글·작은따옴표·문자열 안 세미콜론·여러 줄 ----------

@test
def a4_text_handling(env):
    sql = (
        "insert into USERS (Name, AGE)\n"
        "    values ('a;b', 1);\n"
        "INSERT\n\tINTO users\n(\n  age,\n  name\n)\nVALUES\n(\n  2,\n  '김''s 노트; 메모'\n)\n;\n"
        "INSERT INTO users (name, age) VALUES ('''', 0);"
        "select * from users;"
    )
    expect_ok(env.run(sql),
              b"INSERT 1\nINSERT 1\nINSERT 1\n" + HEADER
              + u("1\ta;b\t1\n2\t김's 노트; 메모\t2\n3\t'\t0\n"))


@test
def a4_utf8_bom_is_skipped(env):
    expect_ok(env.run(b"\xef\xbb\xbf" + u("SELECT * FROM users;")), HEADER)


# ---------- SQL 주석 ----------

@test
def comment_only_file(env):
    expect_ok(env.run("-- 주석만 있는 파일\n/* 블록\n주석; */\n-- 끝"), b"")
    eq(env.data(), b"", "저장 파일")


@test
def comments_before_between_after(env):
    sql = (
        "-- 앞 주석\n"
        "/* 앞 블록 */ INSERT INTO users (name, age) -- 줄 끝\n"
        "VALUES (/* 값 앞 */ 'a', 1); /* 문장 사이 */\n"
        "-- INSERT INTO users (name, age) VALUES ('주석 처리됨', 2);\n"
        "/* INSERT INTO users (name, age) VALUES ('블록 처리됨', 3); */\n"
        "SELECT * FROM users; -- 뒤 주석"
    )
    expect_ok(env.run(sql), b"INSERT 1\n" + HEADER + b"1\ta\t1\n")
    eq(env.data(), b"1\ta\t1\n", "주석 안의 INSERT는 실행되지 않음")


@test
def comment_symbols_inside_string_kept(env):
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('a--b /* c */ d', 1);"
                      "SELECT * FROM users;"),
              b"INSERT 1\n" + HEADER + b"1\ta--b /* c */ d\t1\n")


@test
def unterminated_block_comment(env):
    sql = "INSERT INTO users (name, age) VALUES ('a', 1);\n/* 닫히지 않음\nSELECT * FROM users;"
    expect_fail(env.run(sql), "문장 2", "블록 주석", stdout=b"INSERT 1\n")
    eq(env.data(), b"1\ta\t1\n", "앞 문장은 유지")


# ---------- A5: 잘못된 SQL·컬럼·타입·범위 ----------

A5_CASES = [
    ("INSERT INTO users (name, email) VALUES ('a', 'b');", "컬럼 'email'"),
    ("INSERT INTO users (name) VALUES ('a');", "age 컬럼"),
    ("INSERT INTO users (id, name, age) VALUES (1, 'a', 1);", "자동"),
    ("INSERT INTO users (name, age) VALUES (1, 1);", "문자열"),
    ("INSERT INTO users (name, age) VALUES ('a', 'b');", "정수"),
    ("INSERT INTO users (name, age) VALUES ('a', 151);", "범위"),
    ("INSERT INTO users (name, age) VALUES ('a', -1);", "범위"),
    ("INSERT INTO users (name, age) VALUES ('a', 99999999999999999999);", "범위"),
    ("INSERT INTO users (name, age) VALUES ('', 1);", "빈 문자열"),
    ("INSERT INTO users (name, age) VALUES ('" + "a" * 256 + "', 1);", "255바이트"),
    ("INSERT INTO users (name, age) VALUES ('a\tb', 1);", "탭"),
    ("INSERT INTO users (name, age) VALUES ('a\nb', 1);", "줄바꿈"),
    ("INSERT INTO users (name, age) VALUES ('a', 1)", "세미콜론"),
    ("INSERT INTO users (name, age) VALUES ('a, 1);", "닫히지"),
    ("INSERT INTO orders (name, age) VALUES ('a', 1);", "테이블 'orders'"),
    ("SELECT name FROM users;", "SELECT *"),
    ("SELECT * FROM users WHERE name = 'a';", "id 또는 age"),
    ("SELECT * FROM users WHERE age > 1;", "'='"),
    ("SELECT * FROM users WHERE age = '1';", "정수 값이 필요"),
    ("SELECT * FROM users WHERE id = 1 AND age = 1;", "AND/OR"),
    ("SELECT * FROM users WHERE id = 9223372036854775808;", "64비트"),
    ("SELECT * FROM users WHERE id = 1", "세미콜론"),
    ("DELETE FROM users;", "WHERE 조건이 필요"),
]


@test
def a5_invalid_statements_fail_without_saving(env):
    for sql, needle in A5_CASES:
        expect_fail(env.run(sql), "문장 1", needle)
        eq(env.data(), b"", "실패 후 저장 파일 (%s)" % sql[:40])


@test
def a5_name_255_bytes_boundary_ok(env):
    name = "가" * 85  # 255바이트 (UTF-8 한 글자 3바이트)
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('%s', 150);" % name), b"INSERT 1\n")
    eq(env.data(), u("1\t%s\t150\n" % name), "저장 파일")


# ---------- A6: 중간 문장 실패 ----------

@test
def a6_second_statement_fails(env):
    sql = (
        "INSERT INTO users (name, age) VALUES ('first', 1);\n"
        "INSERT INTO users (name, nickname) VALUES ('second', 'x');\n"
        "INSERT INTO users (name, age) VALUES ('third', 3);\n"
        "SELECT * FROM users;\n"
    )
    expect_fail(env.run(sql), "문장 2", "nickname", stdout=b"INSERT 1\n")
    eq(env.data(), b"1\tfirst\t1\n", "저장 파일: 첫 문장만 유지")
    expect_ok(env.run("SELECT * FROM users;"), HEADER + b"1\tfirst\t1\n")


@test
def a6_select_output_kept_before_failure(env):
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('a', 1);"), b"INSERT 1\n")
    expect_fail(env.run("SELECT * FROM users WHERE age = 1; SELECT * FROM users WHERE age > 1;"
                        "INSERT INTO users (name, age) VALUES ('b', 2);"),
                "문장 2", "'='", stdout=HEADER + b"1\ta\t1\n")
    eq(env.data(), b"1\ta\t1\n", "실패 뒤 INSERT는 실행되지 않음")


# ---------- A7: 빈 입력·빈 테이블·파일 오류 ----------

@test
def a7_empty_and_blank_sql(env):
    expect_ok(env.run(""), b"")
    expect_ok(env.run("  \n\t\r\n"), b"")


@test
def a7_select_empty_table_prints_header(env):
    expect_ok(env.run("SELECT * FROM users;"), HEADER)


@test
def a7_missing_sql_file(env):
    expect_fail(env.run_args(["--data-dir", env.data_dir, os.path.join(env.root, "nope.sql")]),
                "SQL 파일을 열 수 없습니다")


@test
def a7_corrupted_data_file(env):
    os.makedirs(env.data_dir)
    with open(env.data_file, "wb") as f:
        f.write(b"1\tkim\t25\n2\tlee\n")
    expect_fail(env.run("SELECT * FROM users;"), "손상", "2번째 줄")
    expect_fail(env.run("INSERT INTO users (name, age) VALUES ('a', 1);"), "손상")
    eq(env.data(), b"1\tkim\t25\n2\tlee\n", "손상 파일은 그대로")


@test
def a7_data_dir_unusable(env):
    blocker = os.path.join(env.root, "blocker")
    with open(blocker, "wb") as f:
        f.write(b"x")
    expect_fail(env.run_args(["--data-dir", blocker, env.sql_file("SELECT * FROM users;")]),
                "데이터 파일을 열 수 없습니다")


@test
def a7_data_file_is_directory(env):
    os.makedirs(env.data_file)  # users.tsv 자리에 디렉터리
    expect_fail(env.run("SELECT * FROM users;"), "데이터 파일을 열 수 없습니다")


@test
def a7_corrupted_line_after_match_still_fails(env):
    # Phase 2: id 조회는 한 줄만 읽지만, 시작할 때(table_open) 파일 전체를 검증하며
    # 인덱스를 만들기 때문에 뒤쪽 손상은 여전히 보고된다.
    os.makedirs(env.data_dir)
    with open(env.data_file, "wb") as f:
        f.write(b"1\ta\t25\n2\tb\t30\n3\tc\n")
    expect_fail(env.run("SELECT * FROM users WHERE id = 1;"), "손상", "3번째 줄")


@test
def a7_stdout_write_failure(env):
    # Linux 전용: /dev/full은 모든 쓰기가 실패한다(ENOSPC).
    if not os.path.exists("/dev/full"):
        return
    path = env.sql_file("INSERT INTO users (name, age) VALUES ('a', 1); SELECT * FROM users;")
    with open("/dev/full", "wb") as full:
        proc = subprocess.run([BIN, "--data-dir", env.data_dir, path], stdout=full,
                              stderr=subprocess.PIPE, cwd=env.root, timeout=30)
    for marker in SANITIZER_MARKERS:
        assert marker not in proc.stderr, proc.stderr.decode("utf-8", "replace")
    assert proc.returncode != 0, "출력 실패인데 종료 코드 0"
    assert u("출력") in proc.stderr, proc.stderr.decode("utf-8", "replace")


@test
def a7_nul_byte_in_sql(env):
    expect_fail(env.run(b"SELECT * FROM users;\x00"), "NUL")


# ---------- Phase 2 / P2-01: id 조회가 B+ 트리 인덱스를 거친다 ----------

def many_people(n):
    """이름 길이·문자 종류가 제각각인 n명. 바이트 위치가 줄마다 달라지게 한다."""
    people = []
    for i in range(1, n + 1):
        name = ("민수" if i % 3 == 0 else "a" * (1 + i % 40)) + str(i)
        if i % 7 == 0:
            name = "O'Brien-" + name
        people.append((i, name, i % 151))
    return people


def insert_sql(people):
    return "".join("INSERT INTO users (name, age) VALUES ('%s', %d);\n" % (n.replace("'", "''"), a)
                   for _, n, a in people)


def row_bytes(person):
    i, n, a = person
    return u("%d\t%s\t%d\n" % (i, n, a))


@test
def p2_id_lookup_after_restart(env):
    # 다른 실행에서 저장한 데이터: 시작할 때 파일에서 인덱스를 다시 만들어 찾는다.
    people = many_people(300)
    expect_ok(env.run(insert_sql(people)), b"INSERT 1\n" * 300)
    for i in (1, 2, 150, 299, 300):
        expect_ok(env.run("SELECT * FROM users WHERE id = %d;" % i), HEADER + row_bytes(people[i - 1]))
    for i in (0, -5, 301, 9223372036854775807):
        expect_ok(env.run("SELECT * FROM users WHERE id = %d;" % i), HEADER)


@test
def p2_id_lookup_same_run_after_insert(env):
    # 같은 실행 안에서 INSERT 직후의 id도 인덱스에 등록돼 있어야 한다.
    sql = ("INSERT INTO users (name, age) VALUES ('첫째', 10);"
           "SELECT * FROM users WHERE id = 1;"
           "INSERT INTO users (name, age) VALUES ('둘째', 20);"
           "SELECT * FROM users WHERE id = 2;"
           "SELECT * FROM users WHERE id = 3;")
    expect_ok(env.run(sql), b"INSERT 1\n" + HEADER + u("1\t첫째\t10\n") +
              b"INSERT 1\n" + HEADER + u("2\t둘째\t20\n") + HEADER)
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('셋째', 30); SELECT * FROM users WHERE id = 3;"),
              b"INSERT 1\n" + HEADER + u("3\t셋째\t30\n"))


@test
def p2_index_result_matches_full_scan(env):
    # 인덱스 경로의 결과는 전체 조회에서 같은 id를 골라낸 것과 바이트 단위로 같아야 한다.
    people = many_people(120)
    expect_ok(env.run(insert_sql(people)), b"INSERT 1\n" * 120)
    code, out, err = env.run("SELECT * FROM users;")
    eq((code, err), (0, b""), "전체 조회")
    lines = out.split(b"\n")[1:-1]
    by_id = {int(line.split(b"\t")[0]): line + b"\n" for line in lines}
    sql = "".join("SELECT * FROM users WHERE id = %d;" % i for i in range(1, 121, 9))
    expected = b"".join(HEADER + by_id[i] for i in range(1, 121, 9))
    expect_ok(env.run(sql), expected)


@test
def p2_id_lookup_with_gaps_in_file(env):
    # 파일의 id는 증가 순서만 지키면 된다. 빈 번호는 인덱스에도 없다.
    os.makedirs(env.data_dir)
    with open(env.data_file, "wb") as f:
        f.write(u("1\ta\t1\n4\t넷\t4\n10\tten\t100\n"))
    expect_ok(env.run("SELECT * FROM users WHERE id = 4; SELECT * FROM users WHERE id = 5;"
                      "SELECT * FROM users WHERE id = 10;"),
              HEADER + u("4\t넷\t4\n") + HEADER + HEADER + b"10\tten\t100\n")
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('b', 2); SELECT * FROM users WHERE id = 11;"),
              b"INSERT 1\n" + HEADER + b"11\tb\t2\n")


# ---------- 사용법 ----------

# ---------- Phase 2 / P2-02: EXPLAIN 실행 계획 ----------

@test
def p2_explain_paths_and_stats(env):
    # 빈 테이블: 리프 하나뿐인 트리(높이 1), 행 0
    expect_ok(env.run("EXPLAIN SELECT * FROM users;"
                      "EXPLAIN SELECT * FROM users WHERE id = 1;"),
              u("FULL SCAN (조건 없음, 전체 행 수 0)\n"
                "INDEX LOOKUP (id = 1, B+ 트리 높이 1)\n"))
    # 기본 차수 64: 키 64개에서 첫 분할 → 높이 2. 같은 실행 안의 INSERT를 바로 반영한다
    people = many_people(64)
    expect_ok(env.run(insert_sql(people[:63]) +
                      "EXPLAIN SELECT * FROM users WHERE id = 63;" +
                      insert_sql(people[63:]) +
                      "explain select * from users where ID = 64;"
                      "EXPLAIN SELECT * FROM users WHERE age = 25;"
                      "EXPLAIN SELECT * FROM users;"),
              b"INSERT 1\n" * 63 +
              u("INDEX LOOKUP (id = 63, B+ 트리 높이 1)\n") +
              b"INSERT 1\n" +
              u("INDEX LOOKUP (id = 64, B+ 트리 높이 2)\n"
                "FULL SCAN (age = 25, 전체 행 수 64)\n"
                "FULL SCAN (조건 없음, 전체 행 수 64)\n"))
    # 재시작 후: 파일에서 다시 만든 인덱스의 통계. 없는 id·음수도 경로는 INDEX LOOKUP
    expect_ok(env.run("EXPLAIN SELECT * FROM users WHERE id = 999;"
                      "EXPLAIN SELECT * FROM users WHERE id = -5;"
                      "EXPLAIN SELECT * FROM users WHERE age = 151;"),
              u("INDEX LOOKUP (id = 999, B+ 트리 높이 2)\n"
                "INDEX LOOKUP (id = -5, B+ 트리 높이 2)\n"
                "FULL SCAN (age = 151, 전체 행 수 64)\n"))


@test
def p2_explain_does_not_execute(env):
    # 행도 헤더도 출력하지 않고, 파일도 바꾸지 않는다. 일반 SELECT와 섞여도 각자 규칙대로 출력한다
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    expect_ok(env.run("EXPLAIN SELECT * FROM users WHERE id = 1;"
                      "SELECT * FROM users WHERE id = 1;"
                      "EXPLAIN SELECT * FROM users;"),
              u("INDEX LOOKUP (id = 1, B+ 트리 높이 1)\n") + HEADER + u("1\t민수\t25\n") +
              u("FULL SCAN (조건 없음, 전체 행 수 2)\n"))
    eq(env.data(), TWO_ROWS, "EXPLAIN 후 파일")


@test
def p2_explain_errors(env):
    # EXPLAIN은 SELECT에만. 실패한 문장 앞의 출력과 INSERT는 남는다(fail-fast)
    expect_fail(env.run("INSERT INTO users (name, age) VALUES ('a', 1);"
                        "EXPLAIN SELECT * FROM users;"
                        "EXPLAIN INSERT INTO users (name, age) VALUES ('b', 2);"
                        "INSERT INTO users (name, age) VALUES ('c', 3);"),
                "문장 3", "SELECT 문에만",
                stdout=u("INSERT 1\nFULL SCAN (조건 없음, 전체 행 수 1)\n"))
    eq(env.data(), b"1\ta\t1\n", "실패 후 파일")
    expect_fail(env.run("EXPLAIN;"), "문장 1", "EXPLAIN 뒤에 SELECT")
    expect_fail(env.run("EXPLAIN EXPLAIN SELECT * FROM users;"), "EXPLAIN 뒤에 SELECT")
    expect_fail(env.run("EXPLAIN SELECT * FROM users WHERE id <> 1;"), "BETWEEN만 지원")


# ---------- Phase 2 / P2-03: id 범위 조회 ----------

@test
def p2_range_matches_full_scan(env):
    # 인덱스 범위 조회 결과는 전체 조회에서 같은 범위를 골라낸 것과 바이트 단위로 같아야 한다.
    people = many_people(300)
    expect_ok(env.run(insert_sql(people)), b"INSERT 1\n" * 300)
    code, out, err = env.run("SELECT * FROM users;")
    eq((code, err), (0, b""), "전체 조회")
    lines = out.split(b"\n")[1:-1]
    by_id = {int(line.split(b"\t")[0]): line + b"\n" for line in lines}

    def rows(lo, hi):
        return b"".join(by_id[i] for i in sorted(by_id) if lo <= i <= hi)

    cases = [
        ("id BETWEEN 10 AND 20", 10, 20), ("id BETWEEN 1 AND 300", 1, 300),
        ("id BETWEEN 0 AND 1", 0, 1), ("id BETWEEN 300 AND 999", 300, 999),
        ("id BETWEEN 63 AND 65", 63, 65), ("id BETWEEN 20 AND 10", 1, 0),
        ("id BETWEEN 301 AND 400", 1, 0), ("id < 5", -2**63, 4), ("id <= 5", -2**63, 5),
        ("id > 295", 296, 2**63 - 1), ("id >= 295", 295, 2**63 - 1), ("id < 1", 1, 0),
        ("id > 300", 1, 0), ("id <= -9223372036854775808", 1, 0),
        ("id < -9223372036854775808", 1, 0), ("id > 9223372036854775807", 1, 0),
        ("id >= -9223372036854775808", -2**63, 2**63 - 1),
    ]
    sql = "".join("SELECT * FROM users WHERE %s;" % c for c, _, _ in cases)
    expected = b"".join(HEADER + rows(lo, hi) for _, lo, hi in cases)
    expect_ok(env.run(sql), expected)


@test
def p2_range_same_run_and_restart(env):
    # 같은 실행에서 방금 넣은 레코드도 범위에 보이고, 재시작 후에도 같다
    expect_ok(env.run(TWO_INSERTS + "SELECT * FROM users WHERE id >= 2;"
                      "INSERT INTO users (name, age) VALUES ('c', 3);"
                      "SELECT * FROM users WHERE id between 2 and 3;"),
              b"INSERT 1\nINSERT 1\n" + HEADER + u("2\tO'Brien\t30\n") +
              b"INSERT 1\n" + HEADER + u("2\tO'Brien\t30\n3\tc\t3\n"))
    expect_ok(env.run("SELECT * FROM users WHERE id <= 2;"), HEADER + TWO_ROWS)


@test
def p2_range_with_gaps_in_file(env):
    os.makedirs(env.data_dir)
    with open(env.data_file, "wb") as f:
        f.write(u("1\ta\t1\n4\t넷\t4\n10\tten\t100\n"))
    expect_ok(env.run("SELECT * FROM users WHERE id BETWEEN 2 AND 9;"
                      "SELECT * FROM users WHERE id BETWEEN 5 AND 9;"
                      "SELECT * FROM users WHERE id > 1;"),
              HEADER + u("4\t넷\t4\n") + HEADER + HEADER + u("4\t넷\t4\n10\tten\t100\n"))


@test
def p2_range_explain(env):
    expect_ok(env.run(TWO_INSERTS +
                      "EXPLAIN SELECT * FROM users WHERE id BETWEEN 10 AND 20;"
                      "explain select * from users where id between 20 and 10;"
                      "EXPLAIN SELECT * FROM users WHERE id<5;"
                      "EXPLAIN SELECT * FROM users WHERE id <= 5;"
                      "EXPLAIN SELECT * FROM users WHERE id > -1;"
                      "EXPLAIN SELECT * FROM users WHERE id >= 9223372036854775807;"
                      "EXPLAIN SELECT * FROM users WHERE id = 5;"),
              b"INSERT 1\nINSERT 1\n" +
              u("INDEX RANGE SCAN (id BETWEEN 10 AND 20, B+ 트리 높이 1)\n"
                "INDEX RANGE SCAN (id BETWEEN 20 AND 10, B+ 트리 높이 1)\n"
                "INDEX RANGE SCAN (id < 5, B+ 트리 높이 1)\n"
                "INDEX RANGE SCAN (id <= 5, B+ 트리 높이 1)\n"
                "INDEX RANGE SCAN (id > -1, B+ 트리 높이 1)\n"
                "INDEX RANGE SCAN (id >= 9223372036854775807, B+ 트리 높이 1)\n"
                "INDEX LOOKUP (id = 5, B+ 트리 높이 1)\n"))


RANGE_ERROR_CASES = [
    ("SELECT * FROM users WHERE id <> 1;", "BETWEEN만 지원"),
    ("SELECT * FROM users WHERE id != 1;", "BETWEEN만 지원"),
    ("SELECT * FROM users WHERE id NOT BETWEEN 1 AND 2;", "BETWEEN만 지원"),
    ("SELECT * FROM users WHERE age BETWEEN 1 AND 2;", "age 조건은 '='"),
    ("SELECT * FROM users WHERE age < 3;", "age 조건은 '='"),
    ("SELECT * FROM users WHERE id BETWEEN 1;", "BETWEEN의 AND"),
    ("SELECT * FROM users WHERE id BETWEEN 1 AND 2 AND age = 1;", "AND/OR"),
    ("SELECT * FROM users WHERE id BETWEEN 1 AND 9223372036854775808;", "64비트"),
]


@test
def p2_range_errors(env):
    for sql, needle in RANGE_ERROR_CASES:
        expect_fail(env.run("INSERT INTO users (name, age) VALUES ('a', 1);" + sql +
                            "INSERT INTO users (name, age) VALUES ('b', 2);"),
                    "문장 2", needle, stdout=b"INSERT 1\n")
    eq(env.data(), b"".join(u("%d\ta\t1\n" % (i + 1)) for i in range(len(RANGE_ERROR_CASES))),
       "실패한 문장 뒤 INSERT는 실행되지 않음")


# ---------- Phase 2 / P2-04: --timing, 대량 생성, 벤치마크 ----------

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
TIMING_START = re.compile(u(r"^\[측정\] 시작: [0-9]+\.[0-9]{3} ms \(파일 전체 읽기 \+ 인덱스 구축, ([0-9]+)행\)$"))
TIMING_INDEX = re.compile(u(r"^\[측정\] 인덱스: 높이 ([0-9]+), 리프 ([0-9]+)개, 내부 노드 ([0-9]+)개, 약 [0-9]+\.[0-9] MB$"))
TIMING_STMT = re.compile(u(r"^\[측정\] 문장 ([0-9]+): [0-9]+\.[0-9]{4} ms, (결과 ([0-9]+)행|추가 1행|계획만)$"))


def timing_lines(err):
    lines = err.split(b"\n")
    assert lines[-1] == b"", "stderr가 줄바꿈으로 끝나지 않음: %r" % err
    return lines[:-1]


@test
def p2_timing_keeps_stdout(env):
    # --timing은 stderr에만 쓴다. stdout은 옵션이 없을 때와 바이트 단위로 같다.
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    q = env.sql_file("SELECT * FROM users WHERE id = 2;"
                     "SELECT * FROM users WHERE age = 25;"
                     "EXPLAIN SELECT * FROM users;"
                     "SELECT * FROM users WHERE id BETWEEN 1 AND 2;")
    code, plain, err = env.run_args(["--data-dir", env.data_dir, q])
    eq((code, err), (0, b""), "옵션 없음")
    code, timed, err = env.run_args(["--timing", "--data-dir", env.data_dir, q])
    eq(code, 0, "--timing 종료 코드")
    eq(timed, plain, "--timing stdout")
    lines = timing_lines(err)
    eq(len(lines), 6, "측정 줄 수")
    m = TIMING_START.match(lines[0])
    assert m and m.group(1) == b"2", lines[0]
    m = TIMING_INDEX.match(lines[1])
    assert m and m.groups() == (b"1", b"1", b"0"), lines[1]
    expected = [(b"1", b"1"), (b"2", b"1"), (b"3", None), (b"4", b"2")]
    for line, (no, rows) in zip(lines[2:], expected):
        m = TIMING_STMT.match(line)
        assert m, line
        eq(m.group(1), no, "문장 번호")
        if rows is None:
            eq(m.group(2), u("계획만"), "EXPLAIN 측정")
        else:
            eq(m.group(3), rows, "결과 행 수")


@test
def p2_timing_insert_and_failure(env):
    # 새 디렉터리: 시작 0행. INSERT는 "추가 1행". 실패한 문장은 측정 줄 없이 오류만
    q = env.sql_file("INSERT INTO users (name, age) VALUES ('a', 1);"
                     "SELECT * FROM users WHERE age > 1;")
    code, out, err = env.run_args([q, "--timing", "--data-dir", env.data_dir])
    assert code == 1, code
    eq(out, b"INSERT 1\n", "stdout")
    lines = timing_lines(err)
    eq(len(lines), 4, "줄 수: 시작, 인덱스, 문장 1, 오류")
    assert TIMING_START.match(lines[0]).group(1) == b"0"
    assert TIMING_INDEX.match(lines[1])
    m = TIMING_STMT.match(lines[2])
    assert m and m.group(1) == b"1" and m.group(2) == u("추가 1행"), lines[2]
    assert lines[3].startswith(u("오류: 문장 2:")), lines[3]
    eq(env.data(), b"1\ta\t1\n", "파일")


@test
def p2_timing_usage(env):
    code, out, _ = env.run_args(["--help"])
    eq(code, 0, "--help")
    assert b"--timing" in out
    code, _, err = env.run_args(["--timing"])
    eq(code, 2, "SQL 파일 없음")
    code, _, err = env.run_args(["--timings", "x.sql"])
    eq(code, 2, "비슷한 이름의 알 수 없는 옵션")


@test
def p2_gen_sql_feeds_cli(env):
    gen = subprocess.run([sys.executable, os.path.join(TOOLS, "gen_sql.py"), "152"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    eq((gen.returncode, gen.stderr), (0, b""), "gen_sql 실행")
    assert b"\r" not in gen.stdout, "LF로만 써야 함"
    expect_ok(env.run(gen.stdout), b"INSERT 1\n" * 152)
    data = env.data().split(b"\n")
    eq(data[0], b"1\tuser000001\t1", "첫 줄")
    eq(data[150], b"151\tuser000151\t0", "151번째: age = 151 % 151")
    eq(data[151], b"152\tuser000152\t1", "마지막 줄")
    bad = subprocess.run([sys.executable, os.path.join(TOOLS, "gen_sql.py"), "x"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    eq(bad.returncode, 2, "잘못된 인자")


@test
def p2_bench_smoke(env):
    # 작은 건수로 벤치마크 스크립트 전체 흐름(생성 → 삽입 → EXPLAIN → --timing 반복 → 표)을 확인
    work = os.path.join(env.root, "bench")
    p = subprocess.run([sys.executable, os.path.join(TOOLS, "bench.py"), "--rows", "300", "--repeat", "2",
                        "--bin", BIN, "--workdir", work],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    eq((p.returncode, p.stderr), (0, b""), "bench.py 실행")
    out = p.stdout.decode("utf-8")
    for needle in ("| 레코드 수 | 300건 |", "INDEX LOOKUP", "INDEX RANGE SCAN", "FULL SCAN",
                   "| id 범위 1,000건 | `id BETWEEN 150 AND 1149` | INDEX RANGE SCAN | 151 |",
                   "| age 단건 — 일치 없음 | `age = 151` | FULL SCAN | 0 |",
                   "| id 단건 — 없음 | `id = 301` | INDEX LOOKUP | 0 |"):
        assert needle in out, "출력에 %r 없음:\n%s" % (needle, out)


# ---------- Phase 2 / P2-05: 전체 회귀 — 무작위 문장 묶음을 Python 모델과 비교 ----------

@test
def p2_random_model(env):
    # 정해진 시드로 INSERT와 모든 조회 형태를 섞고, 실행을 세 번(재시작 두 번) 나눠
    # 결과를 단순 모델(전체 목록을 걸러내기)과 바이트 단위로 비교한다.
    rnd = random.Random(20261116)
    model = []  # (id, name, age)
    names = ["a", "민수", "O'Brien", "Alice", "지영"]

    def rows(pred):
        return b"".join(u("%d\t%s\t%d\n" % r) for r in model if pred(r))

    for _round in range(3):
        stmts, expected = [], b""
        for _ in range(200):
            k = rnd.random()
            if k < 0.35:
                rid = len(model) + 1
                name = rnd.choice(names) + "_" * rnd.randint(0, 40) + str(rid)
                age = rnd.randint(0, 150)
                stmts.append("INSERT INTO users (name, age) VALUES ('%s', %d);" % (name.replace("'", "''"), age))
                model.append((rid, name, age))
                expected += b"INSERT 1\n"
                continue
            n = len(model)
            a = rnd.randint(-2, n + 3)
            b = rnd.randint(-2, n + 3)
            kind = rnd.choice(["eq", "between", "lt", "le", "gt", "ge", "age", "none"])
            if kind == "eq":
                where, pred = "id = %d" % a, (lambda r, a=a: r[0] == a)
            elif kind == "between":
                where, pred = "id BETWEEN %d AND %d" % (a, b), (lambda r, a=a, b=b: a <= r[0] <= b)
            elif kind == "lt":
                where, pred = "id < %d" % a, (lambda r, a=a: r[0] < a)
            elif kind == "le":
                where, pred = "id <= %d" % a, (lambda r, a=a: r[0] <= a)
            elif kind == "gt":
                where, pred = "id > %d" % a, (lambda r, a=a: r[0] > a)
            elif kind == "ge":
                where, pred = "id >= %d" % a, (lambda r, a=a: r[0] >= a)
            elif kind == "age":
                age = rnd.choice([rnd.randint(0, 150), -1, 151])
                where, pred = "age = %d" % age, (lambda r, age=age: r[2] == age)
            else:
                where, pred = None, (lambda r: True)
            stmts.append("SELECT * FROM users%s;" % (" WHERE " + where if where else ""))
            expected += HEADER + rows(pred)
        expect_ok(env.run("\n".join(stmts)), expected)
    eq(env.data(), rows(lambda r: True), "최종 파일")


# ---------- Phase 4: UPDATE·DELETE ----------

def next_file(env):
    p = os.path.join(env.data_dir, "users.next")
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        return f.read()


def leftover_tmp(env):
    return [n for n in ("users.tsv.tmp", "users.next.tmp") if os.path.exists(os.path.join(env.data_dir, n))]


FIVE = "".join("INSERT INTO users (name, age) VALUES ('%s', %d);" % p
               for p in [("a", 10), ("b", 20), ("c", 20), ("d", 30), ("e", 40)])


@test
def p4_update_delete_basic(env):
    # D2: 고치고 지운 뒤 SELECT가 id 순서로 바뀐 결과를 보여 준다. 다시 실행해도 같다
    expect_ok(env.run(FIVE), b"INSERT 1\n" * 5)
    expect_ok(env.run("UPDATE users SET age = 21 WHERE age = 20;"
                      "UPDATE users SET name = 'O''Brien', age = 5 WHERE id = 1;"
                      "UPDATE users SET name = '민수' WHERE id BETWEEN 4 AND 9;"
                      "UPDATE users SET age = 21 WHERE id = 2;"  # 값이 같아도 센다
                      "DELETE FROM users WHERE id = 3;"
                      "DELETE FROM users WHERE id > 100;"
                      "SELECT * FROM users;"),
              b"UPDATE 2\nUPDATE 1\nUPDATE 2\nUPDATE 1\nDELETE 1\nDELETE 0\n" + HEADER +
              u("1\tO'Brien\t5\n2\tb\t21\n4\t민수\t30\n5\t민수\t40\n"))
    eq(env.data(), u("1\tO'Brien\t5\n2\tb\t21\n4\t민수\t30\n5\t민수\t40\n"), "저장 파일")
    eq(leftover_tmp(env), [], "임시 파일 없음")
    eq(next_file(env), None, "가장 큰 id를 지우지 않으면 users.next 없음")
    # 재시작: 인덱스 조회·범위 조회·선형 탐색 모두 새 파일 기준
    expect_ok(env.run("SELECT * FROM users WHERE id = 4;"
                      "SELECT * FROM users WHERE id = 3;"
                      "SELECT * FROM users WHERE id BETWEEN 2 AND 5;"
                      "SELECT * FROM users WHERE age = 21;"),
              HEADER + u("4\t민수\t30\n") + HEADER + HEADER + u("2\tb\t21\n4\t민수\t30\n5\t민수\t40\n") +
              HEADER + b"2\tb\t21\n")


@test
def p4_delete_max_id_keeps_next_id(env):
    # D3: 가장 큰 id를 지운 뒤 INSERT는 그보다 큰 id를 받는다. 다시 실행해도 같다
    expect_ok(env.run(TWO_INSERTS + "INSERT INTO users (name, age) VALUES ('c', 3);"
                      "DELETE FROM users WHERE id = 3;"
                      "INSERT INTO users (name, age) VALUES ('d', 4);"
                      "SELECT * FROM users WHERE id >= 3;"),
              b"INSERT 1\n" * 3 + b"DELETE 1\nINSERT 1\n" + HEADER + b"4\td\t4\n")
    eq(next_file(env), b"3\n", "users.next")
    expect_ok(env.run("DELETE FROM users WHERE id >= 1;"), b"DELETE 3\n")
    eq(env.data(), b"", "모두 지움")
    eq(next_file(env), b"4\n", "users.next")
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('e', 5); SELECT * FROM users;"),
              b"INSERT 1\n" + HEADER + b"5\te\t5\n")


@test
def p4_zero_rows_leave_file_untouched(env):
    # D06: 조건에 맞는 행이 없으면 0이고 파일을 다시 쓰지 않는다(같은 파일 그대로: inode·수정 시각)
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    before = os.stat(env.data_file)
    expect_ok(env.run("UPDATE users SET age = 1 WHERE id = 99;"
                      "DELETE FROM users WHERE age = 77;"
                      "DELETE FROM users WHERE id BETWEEN 5 AND 3;"),
              b"UPDATE 0\nDELETE 0\nDELETE 0\n")
    after = os.stat(env.data_file)
    eq((after.st_ino, after.st_mtime_ns, after.st_size), (before.st_ino, before.st_mtime_ns, before.st_size),
       "파일 그대로")
    eq(env.data(), TWO_ROWS, "저장 파일")


@test
def p4_value_errors_change_nothing(env):
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    cases = [
        ("UPDATE users SET age = 151 WHERE id = 1;", "age"),
        ("UPDATE users SET name = '' WHERE id = 1;", "빈 문자열"),
        ("UPDATE users SET name = '%s' WHERE id = 1;" % ("x" * 256), "255"),
        ("UPDATE users SET name = 'ok', age = 200 WHERE id = 1;", "age"),
    ]
    for sql, needle in cases:
        expect_fail(env.run(sql), "문장 1", needle)
        eq(env.data(), TWO_ROWS, "실패 후 저장 파일 (%s)" % sql[:40])
        eq(leftover_tmp(env), [], "임시 파일 없음")


@test
def p4_fail_fast_keeps_earlier_change(env):
    # 문장마다 바로 반영된다: 뒤 문장이 실패해도 앞 UPDATE·DELETE는 남는다(INSERT와 같은 fail-fast)
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    expect_fail(env.run("DELETE FROM users WHERE id = 1; UPDATE users SET age = 999 WHERE id = 2;"),
                "문장 2", stdout=b"DELETE 1\n")
    eq(env.data(), u("2\tO'Brien\t30\n"), "앞 DELETE는 반영")


@test
def p4_stale_tmp_removed_on_start(env):
    # 이전 실행이 다시 쓰기 도중 끝나 남긴 임시 파일은 시작할 때 지운다. 원래 파일은 그대로
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    for name, data in (("users.tsv.tmp", b"1\tpartial"), ("users.next.tmp", b"9\n")):
        with open(os.path.join(env.data_dir, name), "wb") as f:
            f.write(data)
    expect_ok(env.run("SELECT * FROM users;"), HEADER + TWO_ROWS)
    eq(leftover_tmp(env), [], "임시 파일 정리")
    expect_ok(env.run("INSERT INTO users (name, age) VALUES ('c', 3);"), b"INSERT 1\n")
    eq(env.data(), TWO_ROWS + b"3\tc\t3\n", "남은 users.next.tmp는 반영하지 않음")


@test
def p4_corrupt_next_file_refuses_start(env):
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    with open(os.path.join(env.data_dir, "users.next"), "wb") as f:
        f.write(b"abc\n")
    expect_fail(env.run("SELECT * FROM users;"), "다음 id 파일 손상")


@test
def p4_timing_lines(env):
    expect_ok(env.run(TWO_INSERTS), b"INSERT 1\nINSERT 1\n")
    code, out, err = env.run_args(["--timing", "--data-dir", env.data_dir,
                                   env.sql_file("UPDATE users SET age = 1 WHERE id >= 1;"
                                                "DELETE FROM users WHERE id = 9;")])
    eq((code, out), (0, b"UPDATE 2\nDELETE 0\n"), "--timing")
    lines = timing_lines(err)
    assert re.match(u(r"^\[측정\] 문장 1: [0-9.]+ ms, 수정 2행$"), lines[2]), lines[2]
    assert re.match(u(r"^\[측정\] 문장 2: [0-9.]+ ms, 삭제 0행$"), lines[3]), lines[3]


@test
def p4_random_model(env):
    # 무작위 UPDATE·DELETE를 파이썬 모델과 비교한다. 매번 다른 프로세스(재시작)로 확인한다
    rng = random.Random(4)
    people = {i: ("p%d" % i, rng.randint(0, 150)) for i in range(1, 41)}
    expect_ok(env.run("".join("INSERT INTO users (name, age) VALUES ('%s', %d);" % people[i] for i in range(1, 41))),
              b"INSERT 1\n" * 40)
    ages = [a for _, a in people.values()]
    for step in range(25):
        k = rng.randrange(4)
        a = rng.randint(1, 45)
        b = a + rng.randint(0, 10)
        v = rng.choice(ages)
        if k == 0:
            sql, hit = "DELETE FROM users WHERE age = %d;" % v, [i for i in people if people[i][1] == v]
        elif k == 1:
            sql, hit = "DELETE FROM users WHERE id BETWEEN %d AND %d;" % (a, b), [i for i in people if a <= i <= b]
        elif k == 2:
            sql, hit = "UPDATE users SET age = %d WHERE id <= %d;" % (v, a), [i for i in people if i <= a]
        else:
            sql, hit = "UPDATE users SET name = 's%d' WHERE id = %d;" % (step, a), [i for i in people if i == a]
        for i in hit:
            if k < 2:
                del people[i]
            elif k == 2:
                people[i] = (people[i][0], v)
            else:
                people[i] = ("s%d" % step, people[i][1])
        kind = b"DELETE" if k < 2 else b"UPDATE"
        rows = b"".join(u("%d\t%s\t%d\n" % (i, people[i][0], people[i][1])) for i in sorted(people))
        expect_ok(env.run(sql + "SELECT * FROM users;"), kind + b" %d\n" % len(hit) + HEADER + rows)
        eq(env.data(), rows, "저장 파일 (%s)" % sql)


@test
def p4_update_delete_syntax_errors(env):
    cases = [
        ("UPDATE users SET name = 'x';", "UPDATE에는 WHERE 조건이 필요"),
        ("DELETE FROM users;", "DELETE에는 WHERE 조건이 필요"),
        ("UPDATE users SET id = 5 WHERE id = 1;", "id는 바꿀 수 없습니다"),
        ("UPDATE users SET age = 1, age = 2 WHERE id = 1;", "중복"),
        ("EXPLAIN DELETE FROM users WHERE id = 1;", "EXPLAIN은 SELECT 문에만"),
    ]
    for sql, needle in cases:
        expect_fail(env.run(sql), "문장 1", needle)
        eq(env.data(), b"", "실패 후 저장 파일 (%s)" % sql[:40])


@test
def usage_errors(env):
    code, out, err = env.run_args([])
    eq(code, 2, "인자 없음 종료 코드")
    assert u("사용법") in err
    code, _, err = env.run_args(["--bogus", "x.sql"])
    eq(code, 2, "알 수 없는 옵션 종료 코드")
    code, _, err = env.run_args(["a.sql", "b.sql"])
    eq(code, 2, "SQL 파일 두 개 종료 코드")
    code, _, err = env.run_args(["x.sql", "--data-dir"])
    eq(code, 2, "--data-dir 값 없음 종료 코드")
    code, out, err = env.run_args(["--help"])
    eq(code, 0, "--help 종료 코드")
    assert u("사용법") in out


@test
def default_data_dir_is_cwd_data(env):
    path = env.sql_file("INSERT INTO users (name, age) VALUES ('a', 1);")
    expect_ok(env.run_args([path]), b"INSERT 1\n")
    eq(env.data(), b"1\ta\t1\n", "기본 data 디렉터리에 저장")


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    if BIN is None or not os.path.exists(BIN):
        print("사용법: cli_test.py <minidb 실행 파일>", file=sys.stderr)
        return 2
    failures = 0
    for fn in TESTS:
        with tempfile.TemporaryDirectory() as root:
            try:
                fn(Env(root))
                print("PASS", fn.__name__)
            except Exception:
                failures += 1
                print("FAIL", fn.__name__)
                traceback.print_exc()
    print("CLI 테스트: %d개 중 %d개 실패" % (len(TESTS), failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
