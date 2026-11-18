/*
 * 단위 테스트: Parser, Storage, Executor를 함수 단위로 검증한다.
 * 사용법: test_unit <임시 디렉터리>
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* clock_gettime (잠금 동작 테스트의 기한) */
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#ifdef _WIN32
#include <pthread_time.h> /* winpthreads의 clock_gettime */
#endif

#include "bptree.h"
#include "db.h"
#include "executor.h"
#include "json.h"
#include "parser.h"
#include "storage.h"
#include "workqueue.h"

#include <sched.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) mkdir((path), 0755)
#endif

static int g_checks;
static int g_failures;
static const char *g_base;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: 실패: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                        \
    } while (0)

/* ---------- 도우미 ---------- */

/* base/name 디렉터리를 비운 상태로 준비하고 경로를 돌려준다. */
static const char *fresh_dir(const char *name) {
    static char dir[512];
    char file[600];

    static const char *const files[] = {"users.tsv", "users.tsv.tmp", "users.next", "users.next.tmp"};
    size_t i;

    snprintf(dir, sizeof dir, "%s/%s", g_base, name);
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(file, sizeof file, "%s/%s", dir, files[i]);
        remove(file);
    }
    make_dir(dir);
    return dir;
}

static void write_file(const char *dir, const char *data, size_t len) {
    char file[600];
    FILE *fp;

    snprintf(file, sizeof file, "%s/users.tsv", dir);
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "테스트 파일 생성 실패: %s\n", file);
        exit(2);
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

/* 파일 전체를 buf에 읽는다. 없으면 길이 0. */
static size_t read_file(const char *dir, char *buf, size_t cap) {
    char file[600];
    FILE *fp;
    size_t n;

    snprintf(file, sizeof file, "%s/users.tsv", dir);
    fp = fopen(file, "rb");
    if (fp == NULL) {
        buf[0] = '\0';
        return 0;
    }
    n = fread(buf, 1, cap - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return n;
}

typedef struct {
    int count;
    int64_t ids[8];
    char names[8][300];
    int ages[8];
    int stop_after; /* 0이면 멈추지 않음 */
} Collected;

static int collect_row(const Row *row, void *ctx) {
    Collected *c = ctx;

    if (c->count < 8) {
        c->ids[c->count] = row->id;
        snprintf(c->names[c->count], sizeof c->names[0], "%s", row->name);
        c->ages[c->count] = row->age;
    }
    c->count++;
    return (c->stop_after != 0 && c->count >= c->stop_after) ? 1 : 0;
}

/* ---------- Parser ---------- */

static int parse_one(const char *sql, Statement *st, DbError *err) {
    size_t pos = 0;
    return parser_next(sql, &pos, st, err);
}

#define EXPECT_PARSE_ERROR(sql, needle)                                                   \
    do {                                                                                  \
        Statement st_;                                                                    \
        DbError e_ = {{0}, DBE_NONE};                                                               \
        int r_ = parse_one((sql), &st_, &e_);                                             \
        CHECK(r_ == -1);                                                                  \
        CHECK(strstr(e_.msg, (needle)) != NULL);                                          \
        if (r_ != -1 || strstr(e_.msg, (needle)) == NULL) {                               \
            fprintf(stderr, "    SQL: %s\n    메시지: %s\n    기대: %s\n", (sql), e_.msg, \
                    (needle));                                                            \
        }                                                                                 \
        CHECK(st_.name == NULL);                                                          \
        if (r_ == 1) {                                                                    \
            statement_free(&st_);                                                         \
        }                                                                                 \
    } while (0)

static void test_parse_insert_basic(void) {
    const char *sql = "INSERT INTO users (name, age) VALUES ('민수', 25);";
    Statement st;
    DbError err = {{0}, DBE_NONE};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    CHECK(st.name != NULL && strcmp(st.name, "민수") == 0);
    CHECK(st.age == 25);
    CHECK(pos == strlen(sql));
    statement_free(&st);
    CHECK(st.name == NULL);
    statement_free(&st); /* 두 번 호출해도 안전 */
}

static void test_parse_column_order_and_case(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("insert into USERS (Age, NAME) values (30, 'O''Brien');", &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    CHECK(st.name != NULL && strcmp(st.name, "O'Brien") == 0);
    CHECK(st.age == 30);
    statement_free(&st);
}

static void test_parse_string_special_and_multiline(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("INSERT\n  INTO users\n(name,\tage)\r\nVALUES\n('a;b ''c''',\n 0)\n;", &st,
                    &err) == 1);
    CHECK(st.name != NULL && strcmp(st.name, "a;b 'c'") == 0);
    CHECK(st.age == 0);
    statement_free(&st);

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('''', 150);", &st, &err) == 1);
    CHECK(st.name != NULL && strcmp(st.name, "'") == 0);
    CHECK(st.age == 150);
    statement_free(&st);
}

static void test_parse_select_and_sequence(void) {
    const char *sql = "SELECT * FROM users; insert into users (name, age) values ('x', 1);\n  \n";
    Statement st;
    DbError err = {{0}, DBE_NONE};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT);
    CHECK(st.name == NULL);
    statement_free(&st);

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    statement_free(&st);

    CHECK(parser_next(sql, &pos, &st, &err) == 0);
    CHECK(parser_next(sql, &pos, &st, &err) == 0); /* 끝에서 반복 호출해도 0 */
}

static void test_parse_empty_input(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("", &st, &err) == 0);
    CHECK(parse_one(" \n\t\r\n ", &st, &err) == 0);
}

static void test_parse_comment_only(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("-- 주석만 있음", &st, &err) == 0);           /* 줄 끝 LF 없이 입력 끝 */
    CHECK(parse_one("-- a\n-- b\n", &st, &err) == 0);
    CHECK(parse_one("/* 블록 */", &st, &err) == 0);
    CHECK(parse_one("/* 여러\n줄 ; 'x' */\n -- 끝\n /**/", &st, &err) == 0);
    CHECK(parse_one("/* /* 중첩 안 함 */", &st, &err) == 0);       /* 첫 닫는 기호에서 끝남 */
}

static void test_parse_comments_around_and_inside(void) {
    const char *sql =
        "-- 앞 주석; SELECT 아님\n"
        "/* 앞 블록; */ INSERT /* 중간 */ INTO users -- 줄 끝 주석\n"
        "(name, /* 컬럼 사이 */ age) VALUES ('a', 1);-- 바로 뒤\n"
        "SELECT*/**/FROM/*x*/users;/* 뒤 */ -- 마지막";
    Statement st;
    DbError err = {{0}, DBE_NONE};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT && st.name != NULL && strcmp(st.name, "a") == 0 && st.age == 1);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == 0);

    /* 음수는 여전히 정수 토큰, -- 만 주석이다. */
    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('a', -1);", &st, &err) == 1);
    CHECK(st.age == -1);
    statement_free(&st);
}

static void test_parse_comment_symbols_in_string(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('a--b /* c */ d', 1);", &st, &err) ==
          1);
    CHECK(st.name != NULL && strcmp(st.name, "a--b /* c */ d") == 0);
    statement_free(&st);

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('/* 열기만', 2); -- '닫기 */'", &st,
                    &err) == 1);
    CHECK(st.name != NULL && strcmp(st.name, "/* 열기만") == 0 && st.age == 2);
    statement_free(&st);
}

static void test_parse_comment_errors(void) {
    EXPECT_PARSE_ERROR("/* 닫히지 않음", "블록 주석");
    EXPECT_PARSE_ERROR("SELECT * FROM users /* 세미콜론이 주석 안; ", "블록 주석");
    /* 중첩 미지원: 안쪽 닫는 기호에서 주석이 끝나 남은 닫는 기호가 SQL로 읽힌다. */
    EXPECT_PARSE_ERROR("/* a /* b */ */ SELECT * FROM users;", "INSERT, SELECT, UPDATE 또는 DELETE");
    EXPECT_PARSE_ERROR("SELECT * FROM users -- 세미콜론이 주석 안;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users / 1;", "알 수 없는 문자 '/'");
}

static void expect_where(const char *sql, WhereKind kind, int64_t value) {
    Statement st;
    DbError err = {{0}, DBE_NONE};
    int r = parse_one(sql, &st, &err);

    CHECK(r == 1);
    CHECK(r == 1 && st.kind == STMT_SELECT && st.where == kind && st.where_value == value);
    if (r != 1 || st.where != kind || st.where_value != value) {
        fprintf(stderr, "    SQL: %s\n    메시지: %s\n", sql, err.msg);
    }
    if (r == 1) {
        statement_free(&st);
    }
}

static void test_parse_where(void) {
    expect_where("SELECT * FROM users;", WHERE_NONE, 0);
    expect_where("SELECT * FROM users WHERE id = 1;", WHERE_ID, 1);
    expect_where("select * from users where ID=42;", WHERE_ID, 42);
    expect_where("SELECT * FROM users WHERE age = 0;", WHERE_AGE, 0);
    expect_where("SELECT * FROM users\nWHERE\n  Age\n  =\n  150\n;", WHERE_AGE, 150);
    expect_where("SELECT * FROM users WHERE /* c */ id -- x\n = /* y */ 7;", WHERE_ID, 7);
    /* 범위 밖 값은 오류가 아니라 일치 없음으로 처리하도록 그대로 담는다. */
    expect_where("SELECT * FROM users WHERE id = -1;", WHERE_ID, -1);
    expect_where("SELECT * FROM users WHERE age = 151;", WHERE_AGE, 151);
    expect_where("SELECT * FROM users WHERE id = 9223372036854775807;", WHERE_ID, INT64_MAX);
    expect_where("SELECT * FROM users WHERE id = -9223372036854775808;", WHERE_ID, INT64_MIN);
}

static void test_parse_where_errors(void) {
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE;", "조건 컬럼 이름");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id =;", "정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id 1;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE 1 = id;", "조건 컬럼 이름");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE name = 'a';", "id 또는 age");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE email = 1;", "컬럼 'email'");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age > 1;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age >= 1;", ">=");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age <> 1;", "<>");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age != 1;", "!=");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age = '1';", "정수 값이 필요");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = NULL;", "정수 값이 필요");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1.5;", "알 수 없는 문자");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 AND age = 2;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 or id = 2;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 = 2;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 9223372036854775808;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = -9223372036854775809;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age = 123456789012345678901234567890123;",
                       "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 WHERE age = 2;", "세미콜론");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1) WHERE id = 1;", "세미콜론");
}

static void test_parse_next_statement_error_isolated(void) {
    /* 첫 문장은 성공해야 하고, 두 번째 문장의 잘못된 문자는 두 번째에서만 보고된다. */
    const char *sql = "SELECT * FROM users; SELECT # FROM users;";
    Statement st;
    DbError err = {{0}, DBE_NONE};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == -1);
    CHECK(strstr(err.msg, "알 수 없는 문자") != NULL);
}

static void test_parse_errors(void) {
    EXPECT_PARSE_ERROR("SELECT * FROM users", "세미콜론");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1)", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users SELECT * FROM users;", "세미콜론");
    EXPECT_PARSE_ERROR(";", "INSERT, SELECT, UPDATE 또는 DELETE");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 1;", "WHERE 조건이 필요");
    EXPECT_PARSE_ERROR("SELECT * FROM orders;", "테이블 'orders'");
    EXPECT_PARSE_ERROR("SELECT name FROM users;", "SELECT *");
    EXPECT_PARSE_ERROR("SELECT * users;", "FROM");
    EXPECT_PARSE_ERROR("INSERT users (name, age) VALUES ('a', 1);", "INTO");
    EXPECT_PARSE_ERROR("INSERT INTO users name, age VALUES ('a', 1);", "'('");
    EXPECT_PARSE_ERROR("INSERT INTO users () VALUES ();", "컬럼 이름");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, email) VALUES ('a', 'b');", "컬럼 'email'");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, NAME) VALUES ('a', 'b');", "중복");
    EXPECT_PARSE_ERROR("INSERT INTO users (name) VALUES ('a');", "age 컬럼");
    EXPECT_PARSE_ERROR("INSERT INTO users (age) VALUES (1);", "name 컬럼");
    EXPECT_PARSE_ERROR("INSERT INTO users (id, name, age) VALUES (1, 'a', 1);", "자동");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) ('a', 1);", "VALUES");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES (1, 1);", "문자열");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 'b');", "정수");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', NULL);", "정수");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a');", "적습니다");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1, 2);", "많습니다");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a' 1);", "',' 또는 ')'");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('abc, 1);", "닫히지");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 99999999999999999999);",
                       "범위");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 2147483648);", "범위");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1.5);", "알 수 없는 문자");
}

/* ---------- Phase 4 / P4-01: UPDATE·DELETE 문법 ---------- */

static void test_parse_update_delete(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};
    const char *multi = "update USERS set AGE = 3 where ID = 1; Delete From users Where id > 5;";
    size_t pos = 0;

    /* SET 하나 */
    CHECK(parse_one("UPDATE users SET age = 31 WHERE id = 3;", &st, &err) == 1);
    CHECK(st.kind == STMT_UPDATE && st.set_cols == SET_COL_AGE && st.age == 31 && st.name == NULL);
    CHECK(st.where == WHERE_ID && st.where_value == 3 && st.explain == 0);
    statement_free(&st);

    /* SET 둘(순서 무관), 문자열 이스케이프, age 조건 */
    CHECK(parse_one("UPDATE users SET age = 40, name = 'O''Brien' WHERE age = 30;", &st, &err) == 1);
    CHECK(st.kind == STMT_UPDATE && st.set_cols == (SET_COL_NAME | SET_COL_AGE));
    CHECK(st.name != NULL && strcmp(st.name, "O'Brien") == 0 && st.age == 40);
    CHECK(st.where == WHERE_AGE && st.where_value == 30);
    statement_free(&st);

    /* id 범위 조건, 주석·여러 줄 */
    CHECK(parse_one("UPDATE users -- 주석\n SET name = 'Kim'\n WHERE id BETWEEN 10 AND 20;", &st, &err) == 1);
    CHECK(st.set_cols == SET_COL_NAME && strcmp(st.name, "Kim") == 0);
    CHECK(st.where == WHERE_ID_RANGE && st.where_op == CMP_BETWEEN && st.where_value == 10 &&
          st.where_value2 == 20);
    statement_free(&st);

    /* DELETE: id =, 비교 연산자, age = */
    CHECK(parse_one("DELETE FROM users WHERE id = 7;", &st, &err) == 1);
    CHECK(st.kind == STMT_DELETE && st.where == WHERE_ID && st.where_value == 7 && st.set_cols == 0 &&
          st.name == NULL);
    statement_free(&st);
    CHECK(parse_one("DELETE FROM users WHERE id >= 1000;", &st, &err) == 1);
    CHECK(st.kind == STMT_DELETE && st.where == WHERE_ID_RANGE && st.where_op == CMP_GE && st.where_value == 1000);
    statement_free(&st);
    CHECK(parse_one("DELETE FROM users WHERE age = 0;", &st, &err) == 1);
    CHECK(st.kind == STMT_DELETE && st.where == WHERE_AGE && st.where_value == 0);
    statement_free(&st);

    /* 대소문자 무시, 한 입력에 여러 문장 */
    CHECK(parser_next(multi, &pos, &st, &err) == 1 && st.kind == STMT_UPDATE && st.age == 3 && st.where_value == 1);
    statement_free(&st);
    CHECK(parser_next(multi, &pos, &st, &err) == 1 && st.kind == STMT_DELETE && st.where_op == CMP_GT);
    statement_free(&st);
    CHECK(parser_next(multi, &pos, &st, &err) == 0);

    /* WHERE 필수 (DB-P4-D03) */
    EXPECT_PARSE_ERROR("DELETE FROM users;", "DELETE에는 WHERE 조건이 필요");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'x';", "UPDATE에는 WHERE 조건이 필요");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'x'", "UPDATE에는 WHERE 조건이 필요");
    EXPECT_PARSE_ERROR("DELETE FROM users LIMIT 1;", "WHERE");
    /* SET 오류: 문자열이 할당된 뒤 실패해도 name이 남지 않는다(EXPECT_PARSE_ERROR가 확인) */
    EXPECT_PARSE_ERROR("UPDATE users SET id = 5 WHERE id = 1;", "id는 바꿀 수 없습니다");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 1, age = 2 WHERE id = 1;", "중복");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'a', NAME = 'b' WHERE id = 1;", "중복");
    EXPECT_PARSE_ERROR("UPDATE users SET email = 'a' WHERE id = 1;", "컬럼 'email'");
    EXPECT_PARSE_ERROR("UPDATE users WHERE id = 1;", "SET");
    EXPECT_PARSE_ERROR("UPDATE users SET WHERE id = 1;", "컬럼 'WHERE'");
    EXPECT_PARSE_ERROR("UPDATE users SET age 3 WHERE id = 1;", "'='");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 'x' WHERE id = 1;", "정수");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 3 WHERE id = 1;", "문자열");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 99999999999 WHERE id = 1;", "범위");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'a', WHERE id = 1;", "컬럼 'WHERE'");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'a', ;", "컬럼 이름");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'a' WHERE name = 'b';", "id 또는 age");
    EXPECT_PARSE_ERROR("UPDATE users SET name = 'a' WHERE id = 1 AND age = 2;", "하나만");
    EXPECT_PARSE_ERROR("UPDATE orders SET age = 1 WHERE id = 1;", "테이블 'orders'");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 1 WHERE id = 1", "세미콜론");
    EXPECT_PARSE_ERROR("DELETE users WHERE id = 1;", "FROM");
    EXPECT_PARSE_ERROR("DELETE FROM users WHERE age > 3;", "'=' 비교만");
    /* EXPLAIN은 SELECT에만 */
    EXPECT_PARSE_ERROR("EXPLAIN UPDATE users SET age = 1 WHERE id = 1;", "EXPLAIN은 SELECT 문에만");
    EXPECT_PARSE_ERROR("EXPLAIN DELETE FROM users WHERE id = 1;", "EXPLAIN은 SELECT 문에만");
}

static void test_parse_explain(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    CHECK(parse_one("EXPLAIN SELECT * FROM users WHERE id = 3;", &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT && st.explain == 1 && st.where == WHERE_ID && st.where_value == 3);
    statement_free(&st);
    CHECK(parse_one("explain /* c */ select * from USERS where AGE = 25;", &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT && st.explain == 1 && st.where == WHERE_AGE && st.where_value == 25);
    statement_free(&st);
    CHECK(parse_one("Explain\nSELECT * FROM users;", &st, &err) == 1);
    CHECK(st.explain == 1 && st.where == WHERE_NONE);
    statement_free(&st);
    /* EXPLAIN이 없으면 0 */
    CHECK(parse_one("SELECT * FROM users WHERE id = 3;", &st, &err) == 1 && st.explain == 0);
    statement_free(&st);
    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('a', 1);", &st, &err) == 1);
    CHECK(st.explain == 0);
    statement_free(&st);
    /* 앞 문장의 EXPLAIN이 다음 문장으로 이어지지 않는다 */
    {
        const char *sql = "EXPLAIN SELECT * FROM users; SELECT * FROM users;";
        size_t pos = 0;
        CHECK(parser_next(sql, &pos, &st, &err) == 1 && st.explain == 1);
        statement_free(&st);
        CHECK(parser_next(sql, &pos, &st, &err) == 1 && st.explain == 0);
        statement_free(&st);
    }
}

static void test_parse_explain_errors(void) {
    EXPECT_PARSE_ERROR("EXPLAIN INSERT INTO users (name, age) VALUES ('a', 1);", "SELECT 문에만");
    EXPECT_PARSE_ERROR("EXPLAIN;", "EXPLAIN 뒤에 SELECT");
    EXPECT_PARSE_ERROR("EXPLAIN", "EXPLAIN 뒤에 SELECT");
    EXPECT_PARSE_ERROR("EXPLAIN EXPLAIN SELECT * FROM users;", "EXPLAIN 뒤에 SELECT");
    EXPECT_PARSE_ERROR("EXPLAIN UPDATE users SET age = 1;", "SELECT 문에만"); /* Phase 4: UPDATE도 EXPLAIN 불가 */
    EXPECT_PARSE_ERROR("SELECT EXPLAIN * FROM users;", "SELECT *");
    /* EXPLAIN 뒤 SELECT의 오류 규칙은 일반 SELECT와 같다 */
    EXPECT_PARSE_ERROR("EXPLAIN SELECT * FROM users", "세미콜론");
    EXPECT_PARSE_ERROR("EXPLAIN SELECT * FROM users WHERE id <> 1;", "BETWEEN만 지원");
    EXPECT_PARSE_ERROR("EXPLAIN SELECT * FROM orders;", "테이블 'orders'");
}

static void expect_range(const char *sql, CmpOp op, int64_t v1, int64_t v2) {
    Statement st;
    DbError err = {{0}, DBE_NONE};
    int r = parse_one(sql, &st, &err);

    CHECK(r == 1);
    CHECK(r == 1 && st.kind == STMT_SELECT && st.where == WHERE_ID_RANGE);
    CHECK(r == 1 && st.where_op == op && st.where_value == v1 && st.where_value2 == v2);
    if (r != 1 || st.where != WHERE_ID_RANGE || st.where_op != op || st.where_value != v1 ||
        st.where_value2 != v2) {
        fprintf(stderr, "    SQL: %s\n    메시지: %s\n", sql, err.msg);
    }
    if (r == 1) {
        statement_free(&st);
    }
}

static void test_parse_id_range(void) {
    Statement st;
    DbError err = {{0}, DBE_NONE};

    expect_range("SELECT * FROM users WHERE id BETWEEN 10 AND 20;", CMP_BETWEEN, 10, 20);
    expect_range("select * from users where ID between -5 and 3;", CMP_BETWEEN, -5, 3);
    expect_range("SELECT * FROM users WHERE id BETWEEN 20 AND 10;", CMP_BETWEEN, 20, 10); /* 뒤집힘도 파싱은 성공 */
    expect_range("SELECT * FROM users WHERE id\n BETWEEN /* c */ 1 -- x\n AND 1;", CMP_BETWEEN, 1, 1);
    expect_range("SELECT * FROM users WHERE id < 5;", CMP_LT, 5, 0);
    expect_range("SELECT * FROM users WHERE id<=5;", CMP_LE, 5, 0);
    expect_range("SELECT * FROM users WHERE id > 5;", CMP_GT, 5, 0);
    expect_range("SELECT * FROM users WHERE id >= 5;", CMP_GE, 5, 0);
    expect_range("SELECT * FROM users WHERE id < -9223372036854775808;", CMP_LT, INT64_MIN, 0);
    expect_range("SELECT * FROM users WHERE id > 9223372036854775807;", CMP_GT, INT64_MAX, 0);
    expect_range("SELECT * FROM users WHERE id BETWEEN -9223372036854775808 AND 9223372036854775807;",
                 CMP_BETWEEN, INT64_MIN, INT64_MAX);
    expect_range("EXPLAIN SELECT * FROM users WHERE id >= 7;", CMP_GE, 7, 0);
    /* 등호 조건은 여전히 WHERE_ID / WHERE_AGE + CMP_EQ */
    CHECK(parse_one("SELECT * FROM users WHERE id = 3;", &st, &err) == 1);
    CHECK(st.where == WHERE_ID && st.where_op == CMP_EQ && st.where_value2 == 0);
    statement_free(&st);
    CHECK(parse_one("SELECT * FROM users WHERE age = 3;", &st, &err) == 1);
    CHECK(st.where == WHERE_AGE && st.where_op == CMP_EQ);
    statement_free(&st);
}

static void test_parse_id_range_errors(void) {
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id <> 1;", "BETWEEN만 지원");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id != 1;", "BETWEEN만 지원");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id ! 1;", "BETWEEN만 지원");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id NOT BETWEEN 1 AND 2;", "BETWEEN만 지원");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age BETWEEN 1 AND 2;", "age 조건은 '='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age >= 1;", "age 조건은 '='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1;", "BETWEEN의 AND");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 OR 2;", "BETWEEN의 AND");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN AND 2;", "id 조건에는 정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 AND;", "정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 AND 'x';", "id 조건에는 정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 AND 2 AND age = 3;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id >= 1 OR id = 2;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id < ;", "정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id >= 9223372036854775808;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 AND 9223372036854775808;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id BETWEEN 1 AND 2", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id < 1 < 2;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id 1;", "'='");
}

/* ---------- Storage ---------- */

static void test_storage_empty_then_append(void) {
    const char *dir = fresh_dir("empty");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t id = 0;

    memset(&c, 0, sizeof c);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 0);
    CHECK(table_append(t, "kim", 1, &id, &err) == 0);
    CHECK(id == 1);
    CHECK(table_close(t, &err) == 0);
    CHECK(table_close(NULL, &err) == 0);
}

static void test_storage_persist_and_restore_id(void) {
    const char *dir = fresh_dir("persist");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t id = 0;
    char buf[256];

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "민수", 25, &id, &err) == 0 && id == 1);
    CHECK(table_append(t, "O'Brien", 30, &id, &err) == 0 && id == 2);
    CHECK(table_close(t, &err) == 0);

    /* 다시 열면 같은 레코드가 같은 순서로 보이고, 다음 ID는 3이다. */
    memset(&c, 0, sizeof c);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 2);
    CHECK(c.ids[0] == 1 && strcmp(c.names[0], "민수") == 0 && c.ages[0] == 25);
    CHECK(c.ids[1] == 2 && strcmp(c.names[1], "O'Brien") == 0 && c.ages[1] == 30);
    CHECK(table_append(t, "a;b", 0, &id, &err) == 0 && id == 3);

    /* 같은 핸들로 추가 직후 다시 조회해도 새 레코드가 보인다. */
    memset(&c, 0, sizeof c);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 3 && c.ids[2] == 3 && strcmp(c.names[2], "a;b") == 0);
    CHECK(table_close(t, &err) == 0);

    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\t민수\t25\n2\tO'Brien\t30\n3\ta;b\t0\n") == 0);
}

static void expect_corrupt(const char *data, size_t len, const char *needle) {
    const char *dir = fresh_dir("corrupt");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};

    write_file(dir, data, len);
    CHECK(table_open(dir, &t, &err) == -1);
    CHECK(t == NULL);
    CHECK(strstr(err.msg, needle) != NULL);
    if (strstr(err.msg, needle) == NULL) {
        fprintf(stderr, "    메시지: %s\n    기대: %s\n", err.msg, needle);
    }
    table_close(t, &err);
}

#define CORRUPT(lit, needle) expect_corrupt((lit), sizeof(lit) - 1, (needle))

static void test_storage_corrupt_files(void) {
    char long_line[400];

    CORRUPT("1\tkim\t25", "개행");
    CORRUPT("1\tkim\n", "필드");
    CORRUPT("1\tkim\t25\textra\n", "필드");
    CORRUPT("2\ta\t1\n1\tb\t2\n", "증가");
    CORRUPT("1\ta\t1\n1\tb\t2\n", "증가");
    CORRUPT("0\ta\t1\n", "양의 정수");
    CORRUPT("01\ta\t1\n", "양의 정수");
    CORRUPT("x\ta\t1\n", "양의 정수");
    CORRUPT("-1\ta\t1\n", "양의 정수");
    CORRUPT("99999999999999999999\ta\t1\n", "양의 정수");
    CORRUPT("9223372036854775808\ta\t1\n", "양의 정수");
    CORRUPT("1\t\t1\n", "name");
    CORRUPT("1\ta\r\t1\n", "name");
    CORRUPT("1\ta\t151\n", "age");
    CORRUPT("1\ta\t-1\n", "age");
    CORRUPT("1\ta\t1\r\n", "age");
    CORRUPT("1\ta\t\n", "age");
    CORRUPT("1\ta\t1\n\n", "필드");
    CORRUPT("1\ta\0b\t1\n", "NUL");
    CORRUPT("1\tok\t1\n2\tbad\t200\n", "2번째 줄");

    memset(long_line, 'a', sizeof long_line);
    memcpy(long_line, "1\t", 2);
    expect_corrupt(long_line, sizeof long_line, "너무 깁니다");
}

static void test_storage_max_id(void) {
    const char *dir = fresh_dir("maxid");
    const char data[] = "9223372036854775807\ta\t1\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    int64_t id = 0;

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "b", 1, &id, &err) == -1);
    CHECK(strstr(err.msg, "ID") != NULL);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_rejects_bad_record(void) {
    const char *dir = fresh_dir("badrec");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char buf[64];
    int64_t id = 0;

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "a\tb", 1, &id, &err) == -1);
    CHECK(table_append(t, "", 1, &id, &err) == -1);
    CHECK(table_append(t, "a", 151, &id, &err) == -1);
    CHECK(table_close(t, &err) == 0);
    CHECK(read_file(dir, buf, sizeof buf) == 0);
}

static void test_storage_callback_stop(void) {
    const char *dir = fresh_dir("stop");
    const char data[] = "1\ta\t1\n2\tb\t2\n3\tc\t3\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;

    write_file(dir, data, sizeof data - 1);
    memset(&c, 0, sizeof c);
    c.stop_after = 2;
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == -1);
    CHECK(c.count == 2);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_open_failure(void) {
    char path[512];
    FILE *fp;
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};

    /* 데이터 디렉터리 자리에 일반 파일이 있으면 열 수 없다. */
    snprintf(path, sizeof path, "%s/not_a_dir", g_base);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fclose(fp);
    }
    CHECK(table_open(path, &t, &err) == -1);
    CHECK(t == NULL);
    CHECK(strstr(err.msg, "열 수 없습니다") != NULL);
}

/* ---------- Executor ---------- */

static int count_insert(int64_t id, void *ctx) {
    int64_t *last = ctx;
    *last = id;
    return 0;
}

static void expect_exec_error(Table *t, const char *name, int age, const char *needle) {
    char buf[300];
    Statement st = {STMT_INSERT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    DbError err = {{0}, DBE_NONE};

    snprintf(buf, sizeof buf, "%s", name);
    st.kind = STMT_INSERT;
    st.name = buf;
    st.age = age;
    CHECK(executor_run(t, &st, NULL, &err) == -1);
    CHECK(strstr(err.msg, needle) != NULL);
    if (strstr(err.msg, needle) == NULL) {
        fprintf(stderr, "    메시지: %s\n    기대: %s\n", err.msg, needle);
    }
}

static void test_executor_validation(void) {
    const char *dir = fresh_dir("exec_valid");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char name255[256];
    char name256[257];
    char buf[64];

    memset(name255, 'a', 255);
    name255[255] = '\0';
    memset(name256, 'a', 256);
    name256[256] = '\0';

    CHECK(table_open(dir, &t, &err) == 0);
    expect_exec_error(t, "", 1, "빈 문자열");
    expect_exec_error(t, name256, 1, "255바이트");
    expect_exec_error(t, "a\tb", 1, "탭");
    expect_exec_error(t, "a\nb", 1, "줄바꿈");
    expect_exec_error(t, "a\rb", 1, "줄바꿈");
    expect_exec_error(t, "a", -1, "범위");
    expect_exec_error(t, "a", 151, "범위");
    CHECK(table_close(t, &err) == 0);
    /* 실패한 INSERT는 하나도 기록되지 않는다. */
    CHECK(read_file(dir, buf, sizeof buf) == 0);

    /* 경계값은 성공한다. */
    {
        Statement st = {STMT_INSERT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
        int64_t last = 0;
        ResultSink sink = {count_insert, NULL, &last, NULL, NULL};

        CHECK(table_open(dir, &t, &err) == 0);
        st.kind = STMT_INSERT;
        st.name = name255;
        st.age = 0;
        CHECK(executor_run(t, &st, &sink, &err) == 0 && last == 1);
        st.age = 150;
        CHECK(executor_run(t, &st, &sink, &err) == 0 && last == 2);
        CHECK(table_close(t, &err) == 0);
    }
}

static void test_executor_insert_select(void) {
    const char *dir = fresh_dir("exec_flow");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t last = 0;
    char kim[] = "kim";
    char lee[] = "lee";
    Statement ins = {STMT_INSERT, kim, 20, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    ResultSink sink;

    memset(&c, 0, sizeof c);
    sink.on_insert = count_insert;
    sink.on_row = collect_row;
    sink.ctx = &last;
    sink.on_plan = NULL;
    sink.on_change = NULL;

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(executor_run(t, &ins, &sink, &err) == 0 && last == 1);
    ins.name = lee;
    ins.age = 21;
    CHECK(executor_run(t, &ins, &sink, &err) == 0 && last == 2);

    sink.ctx = &c;
    CHECK(executor_run(t, &sel, &sink, &err) == 0);
    CHECK(c.count == 2);
    CHECK(c.ids[0] == 1 && strcmp(c.names[0], "kim") == 0 && c.ages[0] == 20);
    CHECK(c.ids[1] == 2 && strcmp(c.names[1], "lee") == 0 && c.ages[1] == 21);
    CHECK(executor_run(t, &sel, NULL, &err) == 0); /* sink 없이도 동작 */
    CHECK(table_close(t, &err) == 0);
}

/* WHERE 조건으로 조회한 id 목록을 ids에 담고 개수를 돌려준다. */
static int select_where(Table *t, WhereKind kind, int64_t value, int64_t *ids) {
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    ResultSink sink = {NULL, collect_row, NULL, NULL, NULL};
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int i;

    memset(&c, 0, sizeof c);
    sel.where = kind;
    sel.where_value = value;
    sink.ctx = &c;
    CHECK(executor_run(t, &sel, &sink, &err) == 0);
    for (i = 0; i < c.count && i < 8; i++) {
        ids[i] = c.ids[i];
    }
    return c.count;
}

static void test_executor_where(void) {
    const char *dir = fresh_dir("exec_where");
    const char data[] = "1\ta\t25\n2\tb\t30\n3\tc\t25\n5\td\t40\n6\te\t25\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    int64_t ids[8];

    write_file(dir, data, sizeof data - 1); /* id 4가 빠진 파일(증가 순서만 요구) */
    CHECK(table_open(dir, &t, &err) == 0);

    /* id 조건: 한 건, 없음(빈 id·범위 밖) */
    CHECK(select_where(t, WHERE_ID, 1, ids) == 1 && ids[0] == 1);
    CHECK(select_where(t, WHERE_ID, 6, ids) == 1 && ids[0] == 6);
    CHECK(select_where(t, WHERE_ID, 4, ids) == 0);
    CHECK(select_where(t, WHERE_ID, 0, ids) == 0);
    CHECK(select_where(t, WHERE_ID, -1, ids) == 0);
    CHECK(select_where(t, WHERE_ID, INT64_MAX, ids) == 0);

    /* age 조건: 여러 건은 저장 순서대로, 한 건, 없음 */
    CHECK(select_where(t, WHERE_AGE, 25, ids) == 3 && ids[0] == 1 && ids[1] == 3 && ids[2] == 6);
    CHECK(select_where(t, WHERE_AGE, 40, ids) == 1 && ids[0] == 5);
    CHECK(select_where(t, WHERE_AGE, 0, ids) == 0);
    CHECK(select_where(t, WHERE_AGE, 151, ids) == 0);
    /* int 범위를 넘는 값이 잘려서 25와 같아지지 않아야 한다 */
    CHECK(select_where(t, WHERE_AGE, 25 + ((int64_t)1 << 32), ids) == 0);

    /* 조건 없음은 전체 */
    CHECK(select_where(t, WHERE_NONE, 0, ids) == 5);
    CHECK(table_close(t, &err) == 0);
}

static void corrupt_line2_behind_back(const char *dir) {
    /* 열린 뒤 다른 경로로 2번째 줄이 손상된 상황 */
    const char bad[] = "1\ta\t25\n2\tb\n";
    char path[600];
    FILE *fp;

    snprintf(path, sizeof path, "%s/users.tsv", dir);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fwrite(bad, 1, sizeof bad - 1, fp);
        fclose(fp);
    }
}

static void test_executor_id_uses_index_age_scans(void) {
    /*
     * Phase 2: WHERE id는 인덱스로 그 줄 하나만 읽고, WHERE age는 끝까지 선형 탐색한다.
     * 열린 뒤 2번째 줄을 망가뜨리면 id = 1은 성공하고(2번째 줄을 읽지 않으므로)
     * age 조회는 2번째 줄에서 손상을 보고한다. 두 경로가 실제로 다르다는 증거다.
     */
    const char *dir = fresh_dir("exec_where_corrupt");
    const char good[] = "1\ta\t25\n2\tb\t30\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Statement by_id = {STMT_SELECT, NULL, 0, WHERE_ID, 1, 0, CMP_EQ, 0, 0};
    Statement by_id2 = {STMT_SELECT, NULL, 0, WHERE_ID, 2, 0, CMP_EQ, 0, 0};
    Statement by_age = {STMT_SELECT, NULL, 0, WHERE_AGE, 25, 0, CMP_EQ, 0, 0};
    Collected c;
    ResultSink sink = {NULL, collect_row, NULL, NULL, NULL};

    write_file(dir, good, sizeof good - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    corrupt_line2_behind_back(dir);

    memset(&c, 0, sizeof c);
    sink.ctx = &c;
    CHECK(executor_run(t, &by_id, &sink, &err) == 0);
    CHECK(c.count == 1 && c.ids[0] == 1 && strcmp(c.names[0], "a") == 0 && c.ages[0] == 25);

    /* 인덱스가 가리키는 줄이 바뀌었으면 엉뚱한 행을 돌려주지 않고 불일치로 보고한다 */
    memset(&c, 0, sizeof c);
    CHECK(executor_run(t, &by_id2, &sink, &err) == -1);
    CHECK(strstr(err.msg, "인덱스와 데이터 파일이 일치하지 않습니다") != NULL);
    CHECK(c.count == 0);

    memset(&c, 0, sizeof c);
    CHECK(executor_run(t, &by_age, &sink, &err) == -1);
    CHECK(strstr(err.msg, "2번째 줄") != NULL);
    CHECK(table_close(t, &err) == 0);
}

/* ---------- Executor: 실행 계획 (EXPLAIN) ---------- */

typedef struct {
    int calls;
    QueryPlan last;
    int fail; /* 1이면 on_plan이 실패를 돌려준다 */
} PlanCapture;

static int capture_plan(const QueryPlan *plan, void *ctx) {
    PlanCapture *pc = ctx;
    pc->calls++;
    pc->last = *plan;
    return pc->fail;
}

/* EXPLAIN 테스트에서 on_row가 불리면 실패로 표시한다. */
static int row_must_not_be_called(const Row *row, void *ctx) {
    (void)row;
    (void)ctx;
    CHECK(0);
    return 0;
}

static void test_executor_plan_paths_and_stats(void) {
    const char *dir = fresh_dir("exec_plan");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    Statement ins = {STMT_INSERT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    QueryPlan plan;
    char name[] = "a";
    int i;

    CHECK(table_open(dir, &t, &err) == 0);

    /* 빈 테이블: 리프 하나뿐이라 높이 1, 행 0 */
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.kind == PLAN_FULL_SCAN && plan.where == WHERE_NONE);
    CHECK(plan.stats.tree_height == 1 && plan.stats.total_rows == 0);

    /* 경로는 조건의 종류만으로 정해진다. 없는 id·범위 밖 값도 INDEX LOOKUP */
    sel.where = WHERE_ID;
    sel.where_value = 99;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.kind == PLAN_INDEX_LOOKUP && plan.where == WHERE_ID && plan.where_value == 99);
    sel.where_value = -1;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0 && plan.kind == PLAN_INDEX_LOOKUP);
    sel.where = WHERE_AGE;
    sel.where_value = 25;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.kind == PLAN_FULL_SCAN && plan.where == WHERE_AGE && plan.where_value == 25);

    /* 통계는 같은 실행 안의 INSERT를 바로 반영한다. 기본 차수 64: 키 64개에서 첫 분할 */
    ins.name = name;
    ins.age = 1;
    for (i = 0; i < 63; i++) {
        CHECK(executor_run(t, &ins, NULL, &err) == 0);
    }
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.stats.tree_height == 1 && plan.stats.total_rows == 63);
    CHECK(executor_run(t, &ins, NULL, &err) == 0);
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.stats.tree_height == 2 && plan.stats.total_rows == 64);
    CHECK(table_index_height(t) == 2 && table_row_count(t) == 64);
    CHECK(table_close(t, &err) == 0);

    /* 재시작 후: 파일에서 다시 만든 인덱스의 통계 */
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.stats.tree_height == 2 && plan.stats.total_rows == 64);

    /* INSERT에는 계획이 없다 */
    CHECK(executor_plan(t, &ins, &plan, &err) == -1);
    CHECK(strstr(err.msg, "SELECT 문에만") != NULL);
    CHECK(table_close(t, &err) == 0);
}

static void test_executor_explain_does_not_execute(void) {
    /*
     * EXPLAIN은 계획만 넘기고 조회를 실행하지 않는다.
     * 열린 뒤 2번째 줄을 망가뜨려도 EXPLAIN ... WHERE age는 성공한다(계획 생성은 파일을 읽지 않으므로).
     * 같은 조건의 실제 SELECT는 2번째 줄에서 손상을 보고한다.
     */
    const char *dir = fresh_dir("exec_explain");
    const char good[] = "1\ta\t25\n2\tb\t30\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Statement ex_age = {STMT_SELECT, NULL, 0, WHERE_AGE, 25, 1, CMP_EQ, 0, 0};
    Statement ex_id = {STMT_SELECT, NULL, 0, WHERE_ID, 2, 1, CMP_EQ, 0, 0};
    Statement sel_age = {STMT_SELECT, NULL, 0, WHERE_AGE, 25, 0, CMP_EQ, 0, 0};
    Collected c;
    PlanCapture pc;
    ResultSink sink = {NULL, row_must_not_be_called, NULL, capture_plan, NULL};
    ResultSink rows_only = {NULL, row_must_not_be_called, NULL, NULL, NULL};
    ResultSink collect = {NULL, collect_row, NULL, NULL, NULL};

    write_file(dir, good, sizeof good - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    corrupt_line2_behind_back(dir);

    memset(&pc, 0, sizeof pc);
    sink.ctx = &pc;
    CHECK(executor_run(t, &ex_age, &sink, &err) == 0);
    CHECK(pc.calls == 1 && pc.last.kind == PLAN_FULL_SCAN && pc.last.where == WHERE_AGE);
    CHECK(pc.last.where_value == 25 && pc.last.stats.total_rows == 2);
    CHECK(executor_run(t, &ex_id, &sink, &err) == 0); /* 가리키는 줄이 바뀌었어도 계획은 성공 */
    CHECK(pc.calls == 2 && pc.last.kind == PLAN_INDEX_LOOKUP && pc.last.where_value == 2);
    CHECK(pc.last.stats.tree_height == 1);

    /* on_plan이 없거나 sink가 없어도 성공하고, on_row는 부르지 않는다 */
    CHECK(executor_run(t, &ex_age, &rows_only, &err) == 0);
    CHECK(executor_run(t, &ex_age, NULL, &err) == 0);

    /* 출력 콜백 실패는 실패로 보고한다 */
    pc.fail = 1;
    CHECK(executor_run(t, &ex_age, &sink, &err) == -1);
    CHECK(strstr(err.msg, "출력") != NULL);

    /* 실제 SELECT는 같은 조건에서 파일을 읽어 손상을 보고한다 */
    memset(&c, 0, sizeof c);
    collect.ctx = &c;
    CHECK(executor_run(t, &sel_age, &collect, &err) == -1);
    CHECK(strstr(err.msg, "2번째 줄") != NULL);
    CHECK(table_close(t, &err) == 0);
}

/* ---------- Executor: id 범위 조회 (P2-03) ---------- */

/* 범위 조회 결과(id 목록)를 out에 담고 개수를 돌려준다. 실패하면 -1. */
static int select_range(Table *t, CmpOp op, int64_t v1, int64_t v2, int64_t *out, int cap) {
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 0, 0, CMP_EQ, 0, 0};
    ResultSink sink = {NULL, collect_row, NULL, NULL, NULL};
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int i;

    memset(&c, 0, sizeof c);
    sel.where_op = op;
    sel.where_value = v1;
    sel.where_value2 = v2;
    sink.ctx = &c;
    if (executor_run(t, &sel, &sink, &err) != 0) {
        return -1;
    }
    for (i = 0; i < c.count && i < 8 && i < cap; i++) {
        out[i] = c.ids[i];
    }
    return c.count;
}

static void test_executor_id_range(void) {
    const char *dir = fresh_dir("exec_range");
    const char data[] = "1\ta\t25\n2\tb\t30\n3\tc\t25\n5\td\t40\n6\te\t25\n9\tf\t1\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    int64_t ids[8];
    QueryPlan plan;
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 2, 0, CMP_BETWEEN, 5, 0};

    write_file(dir, data, sizeof data - 1); /* 4, 7, 8이 빠진 파일 */
    CHECK(table_open(dir, &t, &err) == 0);

    /* BETWEEN: 양 끝 포함, 빈 번호는 건너뜀 */
    CHECK(select_range(t, CMP_BETWEEN, 2, 5, ids, 8) == 3 && ids[0] == 2 && ids[1] == 3 && ids[2] == 5);
    CHECK(select_range(t, CMP_BETWEEN, 4, 4, ids, 8) == 0);
    CHECK(select_range(t, CMP_BETWEEN, 7, 8, ids, 8) == 0);
    CHECK(select_range(t, CMP_BETWEEN, 6, 6, ids, 8) == 1 && ids[0] == 6);
    CHECK(select_range(t, CMP_BETWEEN, 5, 2, ids, 8) == 0); /* 뒤집힘: 결과 없음 */
    CHECK(select_range(t, CMP_BETWEEN, INT64_MIN, INT64_MAX, ids, 8) == 6);
    CHECK(select_range(t, CMP_BETWEEN, -10, 1, ids, 8) == 1 && ids[0] == 1);
    CHECK(select_range(t, CMP_BETWEEN, 9, INT64_MAX, ids, 8) == 1 && ids[0] == 9);
    CHECK(select_range(t, CMP_BETWEEN, 10, INT64_MAX, ids, 8) == 0);

    /* < <= > >= 경계 */
    CHECK(select_range(t, CMP_LT, 3, 0, ids, 8) == 2 && ids[0] == 1 && ids[1] == 2);
    CHECK(select_range(t, CMP_LE, 3, 0, ids, 8) == 3 && ids[2] == 3);
    CHECK(select_range(t, CMP_GT, 5, 0, ids, 8) == 2 && ids[0] == 6 && ids[1] == 9);
    CHECK(select_range(t, CMP_GE, 5, 0, ids, 8) == 3 && ids[0] == 5);
    CHECK(select_range(t, CMP_LT, 1, 0, ids, 8) == 0);
    CHECK(select_range(t, CMP_GT, 9, 0, ids, 8) == 0);
    CHECK(select_range(t, CMP_GE, 4, 0, ids, 8) == 3 && ids[0] == 5); /* 없는 키에서 시작 */
    CHECK(select_range(t, CMP_LE, 4, 0, ids, 8) == 3 && ids[2] == 3);
    /* 64비트 끝: ±1이 넘치지 않고 빈 범위 / 전체가 된다 */
    CHECK(select_range(t, CMP_LT, INT64_MIN, 0, ids, 8) == 0);
    CHECK(select_range(t, CMP_GT, INT64_MAX, 0, ids, 8) == 0);
    CHECK(select_range(t, CMP_LE, INT64_MAX, 0, ids, 8) == 6);
    CHECK(select_range(t, CMP_GE, INT64_MIN, 0, ids, 8) == 6);
    CHECK(select_range(t, CMP_LT, INT64_MAX, 0, ids, 8) == 6);
    CHECK(select_range(t, CMP_GT, INT64_MIN, 0, ids, 8) == 6);

    /* 계획: INDEX RANGE SCAN과 정규화된 범위 */
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.kind == PLAN_INDEX_RANGE_SCAN && plan.where_op == CMP_BETWEEN);
    CHECK(plan.where_value == 2 && plan.where_value2 == 5);
    CHECK(plan.range.lo == 2 && plan.range.hi == 5 && !plan.range.empty);
    sel.where_op = CMP_LT;
    sel.where_value = 3;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.range.lo == INT64_MIN && plan.range.hi == 2 && !plan.range.empty);
    sel.where_op = CMP_GT;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0);
    CHECK(plan.range.lo == 4 && plan.range.hi == INT64_MAX && !plan.range.empty);
    sel.where_value = INT64_MAX;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0 && plan.range.empty);
    sel.where_op = CMP_BETWEEN;
    sel.where_value = 5;
    sel.where_value2 = 2;
    CHECK(executor_plan(t, &sel, &plan, &err) == 0 && plan.range.empty);
    CHECK(table_close(t, &err) == 0);
}

/* 같은 실행에서 INSERT한 레코드도 범위 조회에 바로 보이고, 결과가 선형 탐색과 같아야 한다. */
static void test_executor_range_matches_scan(void) {
    const char *dir = fresh_dir("exec_range_scan");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char name[64];
    int64_t id;
    int i;
    int lo;
    int ok = 1;

    CHECK(table_open(dir, &t, &err) == 0);
    for (i = 1; i <= 300; i++) {
        snprintf(name, sizeof name, "%.*s%d", 1 + i % 30, "abcdefghijklmnopqrstuvwxyz0123456789", i);
        if (table_append(t, name, i % 151, &id, &err) != 0) {
            ok = 0;
        }
    }
    CHECK(ok);
    /* 차수 64 기본 트리에서 리프 경계를 여러 번 넘는 범위들 */
    for (lo = -3; lo <= 305 && ok; lo += 17) {
        int hi;
        for (hi = lo - 2; hi <= lo + 140; hi += 23) {
            int expect = 0;
            int got;
            int64_t first = 0;
            int64_t out[8];
            int a = lo < 1 ? 1 : lo;
            int b = hi > 300 ? 300 : hi;
            if (a <= b) {
                expect = b - a + 1;
                first = a;
            }
            got = select_range(t, CMP_BETWEEN, lo, hi, out, 8);
            if (got != expect || (expect > 0 && out[0] != first)) {
                fprintf(stderr, "    BETWEEN %d AND %d: 기대 %d건, 실제 %d건\n", lo, hi, expect, got);
                ok = 0;
                break;
            }
        }
    }
    CHECK(ok);
    CHECK(table_close(t, &err) == 0);
}

static void test_executor_range_reads_only_inside(void) {
    /*
     * 범위 조회는 범위 안의 줄만 읽는다. 열린 뒤 2번째 줄을 망가뜨리면
     * BETWEEN 1 AND 1은 성공하고, 2를 포함하는 범위는 "인덱스 불일치"로 실패한다.
     */
    const char *dir = fresh_dir("exec_range_corrupt");
    const char good[] = "1\ta\t25\n2\tb\t30\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Statement r11 = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 1, 0, CMP_BETWEEN, 1, 0};
    Statement r12 = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 1, 0, CMP_BETWEEN, 2, 0};
    Statement empty = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 2, 0, CMP_BETWEEN, 1, 0};
    Collected c;
    ResultSink sink = {NULL, collect_row, NULL, NULL, NULL};

    write_file(dir, good, sizeof good - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    corrupt_line2_behind_back(dir);

    memset(&c, 0, sizeof c);
    sink.ctx = &c;
    CHECK(executor_run(t, &r11, &sink, &err) == 0);
    CHECK(c.count == 1 && c.ids[0] == 1 && strcmp(c.names[0], "a") == 0);

    memset(&c, 0, sizeof c);
    CHECK(executor_run(t, &r12, &sink, &err) == -1);
    CHECK(strstr(err.msg, "인덱스와 데이터 파일이 일치하지 않습니다") != NULL);
    CHECK(c.count == 1); /* 1번은 이미 넘긴 뒤 2번에서 멈춤 */

    memset(&c, 0, sizeof c);
    CHECK(executor_run(t, &empty, &sink, &err) == 0 && c.count == 0); /* 빈 범위는 아무것도 읽지 않음 */

    /* 형식은 멀쩡하지만 id가 다른 줄(같은 길이): 파싱은 통과하므로 id 재확인만이 잡는다 */
    write_file(dir, "1\ta\t25\n7\tb\t30\n", 14);
    memset(&c, 0, sizeof c);
    CHECK(executor_run(t, &r12, &sink, &err) == -1);
    CHECK(strstr(err.msg, "인덱스와 데이터 파일이 일치하지 않습니다") != NULL);
    CHECK(c.count == 1 && c.ids[0] == 1);
    CHECK(table_close(t, &err) == 0);
}

/* ---------- B+ 트리 ---------- */

#define EXPECT_VALID(t)                                                     \
    do {                                                                    \
        char why_[128] = "";                                                \
        int ok_ = bpt_validate((t), why_, sizeof why_) == 0;                \
        CHECK(ok_);                                                         \
        if (!ok_) {                                                         \
            fprintf(stderr, "    불변식 위반: %s\n", why_);                 \
        }                                                                   \
    } while (0)

static void test_bpt_create_order_bounds(void) {
    BpTree *t;

    CHECK(bpt_create(BPT_MIN_ORDER - 1) == NULL);
    CHECK(bpt_create(BPT_MAX_ORDER + 1) == NULL);
    CHECK(bpt_create(0) == NULL);
    CHECK(bpt_create(-5) == NULL);
    t = bpt_create(BPT_MIN_ORDER);
    CHECK(t != NULL && bpt_order(t) == BPT_MIN_ORDER);
    bpt_destroy(t);
    t = bpt_create(BPT_MAX_ORDER);
    CHECK(t != NULL && bpt_order(t) == BPT_MAX_ORDER);
    bpt_destroy(t);
    bpt_destroy(NULL); /* NULL 해제는 아무것도 하지 않는다 */
}

static void test_bpt_empty(void) {
    BpTree *t = bpt_create(4);
    int64_t v = 777;

    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(bpt_size(t) == 0);
    CHECK(bpt_height(t) == 1);
    CHECK(bpt_search(t, 1, &v) == 0);
    CHECK(v == 777); /* 못 찾으면 값을 건드리지 않는다 */
    CHECK(bpt_search(t, 0, NULL) == 0);
    EXPECT_VALID(t);
    bpt_destroy(t);
}

static void test_bpt_split_shapes(void) {
    /* 차수 3(키 최대 2개): 손으로 그린 모양과 높이가 일치하는지 */
    BpTree *t = bpt_create(3);
    int64_t v;
    int64_t k;

    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(bpt_insert(t, 1, 100) == BPT_OK);
    CHECK(bpt_insert(t, 2, 200) == BPT_OK);
    CHECK(bpt_height(t) == 1); /* [1 2]: 아직 리프 하나 */
    CHECK(bpt_insert(t, 3, 300) == BPT_OK);
    CHECK(bpt_height(t) == 2); /* [1 2 3] 넘침 → [1 2] [3], 루트 [3] */
    CHECK(bpt_insert(t, 4, 400) == BPT_OK);
    CHECK(bpt_insert(t, 5, 500) == BPT_OK);
    CHECK(bpt_height(t) == 2); /* 루트 [3 5]: 꽉 참 */
    CHECK(bpt_insert(t, 6, 600) == BPT_OK);
    CHECK(bpt_insert(t, 7, 700) == BPT_OK);
    CHECK(bpt_height(t) == 3); /* 리프·루트 연쇄 분할 → 새 루트 [5] */
    EXPECT_VALID(t);
    for (k = 1; k <= 7; k++) {
        CHECK(bpt_search(t, k, &v) == 1 && v == k * 100);
    }
    CHECK(bpt_search(t, 0, NULL) == 0);
    CHECK(bpt_search(t, 8, NULL) == 0);
    bpt_destroy(t);
}

static void test_bpt_sequential_many_orders(void) {
    /* 자동 ID처럼 1, 2, 3...을 차례로 넣는다. 작은 차수일수록 분할이 많이 일어난다. */
    static const int orders[] = {3, 4, 5, 8, 64};
    size_t oi;

    for (oi = 0; oi < sizeof orders / sizeof orders[0]; oi++) {
        BpTree *t = bpt_create(orders[oi]);
        int64_t k;
        int all_found = 1;
        int valid_each = 1;

        CHECK(t != NULL);
        if (t == NULL) {
            continue;
        }
        for (k = 1; k <= 2000; k++) {
            CHECK(bpt_insert(t, k, k * 10 + 1) == BPT_OK);
            if (k <= 200 && bpt_validate(t, NULL, 0) != 0) {
                valid_each = 0; /* 앞부분은 삽입할 때마다 전체 불변식을 확인한다 */
            }
        }
        CHECK(valid_each);
        EXPECT_VALID(t);
        CHECK(bpt_size(t) == 2000);
        for (k = 1; k <= 2000; k++) {
            int64_t v = 0;
            if (bpt_search(t, k, &v) != 1 || v != k * 10 + 1) {
                all_found = 0;
            }
        }
        CHECK(all_found);
        CHECK(bpt_search(t, 0, NULL) == 0);
        CHECK(bpt_search(t, -1, NULL) == 0);
        CHECK(bpt_search(t, 2001, NULL) == 0);
        CHECK(bpt_search(t, INT64_MAX, NULL) == 0);
        CHECK(bpt_search(t, INT64_MIN, NULL) == 0);
        bpt_destroy(t);
    }
}

/* 결정적인 의사 난수 (테스트 재현용) */
static uint64_t g_rng = 88172645463325252ULL;
static uint64_t next_rand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void test_bpt_random_order_and_duplicates(void) {
    /* 섞인 순서·음수·극단값 키, 중복 삽입 거부 */
    enum { N = 3000 };
    static int64_t keys[N];
    static const int orders[] = {3, 4, 7, 64};
    size_t oi;
    int i;

    for (i = 0; i < N; i++) {
        keys[i] = (int64_t)i * 7919 - 5000000; /* 서로 다른 값, 음수 포함 */
    }
    keys[0] = INT64_MIN;
    keys[1] = INT64_MAX;
    keys[2] = 0;
    for (oi = 0; oi < sizeof orders / sizeof orders[0]; oi++) {
        BpTree *t = bpt_create(orders[oi]);
        int all_found = 1;
        int64_t v;

        CHECK(t != NULL);
        if (t == NULL) {
            continue;
        }
        for (i = N - 1; i > 0; i--) { /* Fisher-Yates 섞기 */
            int j = (int)(next_rand() % (uint64_t)(i + 1));
            int64_t tmp = keys[i];
            keys[i] = keys[j];
            keys[j] = tmp;
        }
        for (i = 0; i < N; i++) {
            CHECK(bpt_insert(t, keys[i], keys[i] ^ 0x5a5a) == BPT_OK);
        }
        EXPECT_VALID(t);
        CHECK(bpt_size(t) == N);
        for (i = 0; i < N; i++) {
            if (bpt_search(t, keys[i], &v) != 1 || v != (keys[i] ^ 0x5a5a)) {
                all_found = 0;
            }
        }
        CHECK(all_found);
        /* 중복: 거부되고 크기·기존 값은 그대로 */
        CHECK(bpt_insert(t, keys[10], 12345) == BPT_DUPLICATE);
        CHECK(bpt_insert(t, INT64_MIN, 1) == BPT_DUPLICATE);
        CHECK(bpt_size(t) == N);
        CHECK(bpt_search(t, keys[10], &v) == 1 && v == (keys[10] ^ 0x5a5a));
        CHECK(bpt_search(t, 1, NULL) == 0); /* 7919의 배수 - 5000000 꼴이 아닌 값 */
        EXPECT_VALID(t);
        bpt_destroy(t);
    }
}

static void test_bpt_height_is_logarithmic(void) {
    /* 차수 64로 10만 개: 리프가 절반만 차도(순차 삽입) 높이는 4 이하여야 한다 */
    BpTree *t = bpt_create(64);
    int64_t k;

    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    for (k = 1; k <= 100000; k++) {
        if (bpt_insert(t, k, k) != BPT_OK) {
            CHECK(0);
            break;
        }
    }
    CHECK(bpt_height(t) >= 3 && bpt_height(t) <= 4);
    EXPECT_VALID(t);
    bpt_destroy(t);
}

static void test_bpt_nomem_leaves_tree_unchanged(void) {
    /*
     * 차수 3에 1..6을 넣으면 루트 [3 5]와 리프 [5 6]이 모두 꽉 찬다.
     * 여기에 7을 넣으면 리프 1개 + 내부 1개 + 새 루트 1개 = 노드 3개(할당 9번)가 필요하다.
     * 몇 번째 할당에서 실패하든 트리는 삽입 전과 같아야 한다.
     */
    long fail_at;

    for (fail_at = 0; fail_at <= 9; fail_at++) {
        BpTree *t = bpt_create(3);
        int64_t k;
        int r;
        int all_found = 1;

        CHECK(t != NULL);
        if (t == NULL) {
            continue;
        }
        for (k = 1; k <= 6; k++) {
            CHECK(bpt_insert(t, k, k) == BPT_OK);
        }
        CHECK(bpt_height(t) == 2);
        bpt_test_fail_alloc_after(fail_at);
        r = bpt_insert(t, 7, 7);
        bpt_test_fail_alloc_after(-1);
        if (fail_at < 9) {
            CHECK(r == BPT_NOMEM);
            CHECK(bpt_size(t) == 6);
            CHECK(bpt_height(t) == 2);
            CHECK(bpt_search(t, 7, NULL) == 0);
            CHECK(bpt_insert(t, 7, 7) == BPT_OK); /* 메모리가 돌아오면 다시 넣을 수 있다 */
        } else {
            CHECK(r == BPT_OK); /* 9번 모두 성공하면 삽입 완료 */
        }
        CHECK(bpt_height(t) == 3);
        for (k = 1; k <= 7; k++) {
            if (bpt_search(t, k, NULL) != 1) {
                all_found = 0;
            }
        }
        CHECK(all_found);
        EXPECT_VALID(t);
        bpt_destroy(t);
    }
    /* 생성 자체의 할당 실패 */
    bpt_test_fail_alloc_after(0);
    CHECK(bpt_create(4) == NULL);
    bpt_test_fail_alloc_after(1);
    CHECK(bpt_create(4) == NULL);
    bpt_test_fail_alloc_after(-1);
}

/* ---------- B+ 트리: 범위 순회 (P2-03) ---------- */

typedef struct {
    int count;
    int64_t keys[4096];
    int64_t values[4096];
    int stop_after; /* 0이면 멈추지 않음 */
} RangeCollect;

static int collect_range(int64_t key, int64_t value, void *ctx) {
    RangeCollect *rc = ctx;
    if (rc->count < 4096) {
        rc->keys[rc->count] = key;
        rc->values[rc->count] = value;
    }
    rc->count++;
    return (rc->stop_after != 0 && rc->count >= rc->stop_after) ? 7 : 0;
}

/* 키가 2, 4, 6, ..., 2n(짝수만)인 트리에서 [lo, hi] 결과가 직접 센 값과 같은지 */
static int range_ok(const BpTree *t, int n, int64_t lo, int64_t hi) {
    static RangeCollect rc;
    int64_t k;
    int i = 0;

    rc.count = 0;
    rc.stop_after = 0;
    if (bpt_range(t, lo, hi, collect_range, &rc) != 0) {
        return 0;
    }
    for (k = 2; k <= 2 * (int64_t)n; k += 2) {
        if (k < lo || k > hi) {
            continue;
        }
        if (i >= rc.count || rc.keys[i] != k || rc.values[i] != k * 10) {
            return 0;
        }
        i++;
    }
    return i == rc.count;
}

static void test_bpt_range(void) {
    static const int orders[] = {3, 4, 5, 64};
    static const int sizes[] = {0, 1, 2, 3, 7, 50, 1000};
    size_t oi;
    size_t si;

    for (oi = 0; oi < sizeof orders / sizeof orders[0]; oi++) {
        for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            int n = sizes[si];
            BpTree *t = bpt_create(orders[oi]);
            int ok = 1;
            int64_t lo;
            int i;

            CHECK(t != NULL);
            if (t == NULL) {
                continue;
            }
            for (i = 1; i <= n; i++) {
                ok = ok && bpt_insert(t, 2 * (int64_t)i, 20 * (int64_t)i) == BPT_OK;
            }
            CHECK(ok);
            /* 있는 키·없는 키(홀수)에서 시작/끝, 리프 경계를 넘는 범위, 뒤집힌 범위 */
            for (lo = -1; lo <= 2 * (int64_t)n + 2 && ok; lo += (n > 100 ? 37 : 1)) {
                int64_t hi;
                for (hi = lo - 2; hi <= lo + 2 * (int64_t)n + 3; hi += (n > 100 ? 211 : 1)) {
                    if (!range_ok(t, n, lo, hi)) {
                        fprintf(stderr, "    차수 %d, 키 %d개, [%lld, %lld] 불일치\n", orders[oi], n,
                                (long long)lo, (long long)hi);
                        ok = 0;
                        break;
                    }
                }
            }
            CHECK(ok);
            CHECK(range_ok(t, n, INT64_MIN, INT64_MAX));
            CHECK(range_ok(t, n, INT64_MAX, INT64_MAX));
            CHECK(range_ok(t, n, INT64_MIN, INT64_MIN));
            CHECK(range_ok(t, n, INT64_MAX, INT64_MIN));
            bpt_destroy(t);
        }
    }

    /* 콜백이 멈추라고 하면 바로 멈추고 그 값을 돌려준다 */
    {
        static RangeCollect rc;
        BpTree *t = bpt_create(3);
        int i;
        CHECK(t != NULL);
        if (t != NULL) {
            for (i = 1; i <= 20; i++) {
                bpt_insert(t, i, i);
            }
            memset(&rc, 0, sizeof rc);
            rc.stop_after = 5;
            CHECK(bpt_range(t, 3, 100, collect_range, &rc) == 7);
            CHECK(rc.count == 5 && rc.keys[0] == 3 && rc.keys[4] == 7);
            bpt_destroy(t);
        }
    }
}

/* ---------- Storage: id 인덱스 ---------- */

static void test_storage_index_lookup(void) {
    const char *dir = fresh_dir("index_lookup");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t id;

    CHECK(table_open(dir, &t, &err) == 0);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 1, collect_row, &c, &err) == 0 && c.count == 0); /* 빈 테이블 */
    CHECK(table_append(t, "민수", 25, &id, &err) == 0 && id == 1);
    CHECK(table_append(t, "O'Brien", 30, &id, &err) == 0 && id == 2);
    CHECK(table_append(t, "지영", 41, &id, &err) == 0 && id == 3);

    /* 같은 실행 안에서 방금 추가한 레코드도 인덱스로 찾는다 */
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 2, collect_row, &c, &err) == 0);
    CHECK(c.count == 1 && c.ids[0] == 2 && strcmp(c.names[0], "O'Brien") == 0 && c.ages[0] == 30);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 3, collect_row, &c, &err) == 0);
    CHECK(c.count == 1 && strcmp(c.names[0], "지영") == 0);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 4, collect_row, &c, &err) == 0 && c.count == 0);
    CHECK(table_find_by_id(t, 0, collect_row, &c, &err) == 0 && c.count == 0);
    CHECK(table_find_by_id(t, -1, collect_row, &c, &err) == 0 && c.count == 0);
    CHECK(table_find_by_id(t, 1, NULL, NULL, &err) == 0); /* 콜백 없음도 허용 */
    /* 콜백이 멈추라고 하면 실패로 보고한다 (선형 탐색과 같은 규칙) */
    memset(&c, 0, sizeof c);
    c.stop_after = 1;
    CHECK(table_find_by_id(t, 1, collect_row, &c, &err) == -1);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_index_rebuilt_on_open(void) {
    /* 인덱스는 메모리에만 있으므로, 다시 열면 파일에서 위치를 새로 계산해야 한다 */
    const char *dir = fresh_dir("index_rebuild");
    const char data[] = "1\t가\t1\n2\tbb\t22\n5\t다라마\t150\n9\tz\t0\n";
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t id;

    write_file(dir, data, sizeof data - 1); /* id 사이에 빈 번호가 있는 파일 */
    CHECK(table_open(dir, &t, &err) == 0);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 5, collect_row, &c, &err) == 0);
    CHECK(c.count == 1 && strcmp(c.names[0], "다라마") == 0 && c.ages[0] == 150);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 9, collect_row, &c, &err) == 0 && c.count == 1 && c.ages[0] == 0);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 3, collect_row, &c, &err) == 0 && c.count == 0);
    CHECK(table_append(t, "새", 7, &id, &err) == 0 && id == 10);
    CHECK(table_close(t, &err) == 0);

    CHECK(table_open(dir, &t, &err) == 0);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 10, collect_row, &c, &err) == 0);
    CHECK(c.count == 1 && strcmp(c.names[0], "새") == 0 && c.ages[0] == 7);
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 1, collect_row, &c, &err) == 0 && c.count == 1);
    CHECK(strcmp(c.names[0], "가") == 0);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_index_many_records(void) {
    /* 길이가 제각각인 레코드 수천 건: 모든 위치가 정확해야 하고, 재시작 후에도 같아야 한다 */
    enum { N = 5000 };
    const char *dir = fresh_dir("index_many");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char name[300];
    int pass;
    int i;

    CHECK(table_open(dir, &t, &err) == 0);
    for (i = 1; i <= N; i++) {
        int64_t id;
        int len = 1 + (i * 37) % 200;
        memset(name, 'a' + i % 26, (size_t)len);
        name[len] = '\0';
        if (i % 3 == 0) {
            memcpy(name, "한글", 6 < len ? 6 : 0); /* 여러 바이트 문자 섞기 */
        }
        if (table_append(t, name, i % 151, &id, &err) != 0 || id != i) {
            CHECK(0);
            break;
        }
    }
    for (pass = 0; pass < 2; pass++) {
        int ok = 1;
        if (pass == 1) { /* 다시 열어 인덱스를 파일에서 재구성 */
            CHECK(table_close(t, &err) == 0);
            CHECK(table_open(dir, &t, &err) == 0);
        }
        for (i = N; i >= 1; i -= 7) {
            Collected c;
            int len = 1 + (i * 37) % 200;
            memset(&c, 0, sizeof c);
            if (table_find_by_id(t, i, collect_row, &c, &err) != 0 || c.count != 1 || c.ids[0] != i ||
                c.ages[0] != i % 151 || strlen(c.names[0]) != (size_t)len) {
                ok = 0;
            }
        }
        CHECK(ok);
    }
    CHECK(table_close(t, &err) == 0);
}

/* ---------- B+ 트리·Storage: 메모리 실측 (P2-04) ---------- */

static void test_bpt_mem_stats(void) {
    BptMemStats m0;
    BptMemStats m3;
    BptMemStats m7;
    BpTree *t = bpt_create(3);
    int i;

    CHECK(t != NULL);
    if (t == NULL) {
        return;
    }
    bpt_mem_stats(t, &m0); /* 빈 트리: 빈 리프 하나 */
    CHECK(m0.leaves == 1 && m0.internals == 0 && m0.bytes > 0);
    for (i = 1; i <= 3; i++) {
        bpt_insert(t, i, i);
    }
    bpt_mem_stats(t, &m3); /* [3] / [1 2] [3] */
    CHECK(m3.leaves == 2 && m3.internals == 1 && m3.bytes > m0.bytes);
    for (i = 4; i <= 7; i++) {
        bpt_insert(t, i, i);
    }
    bpt_mem_stats(t, &m7); /* [5] / [3] [7] / [1 2] [3 4] [5 6] [7] */
    CHECK(m7.leaves == 4 && m7.internals == 3);
    /* 리프·내부 노드 하나의 크기는 일정하므로, 늘어난 바이트는 늘어난 노드 수로 설명된다 */
    {
        size_t leaf_plus_internal = m3.bytes - m0.bytes; /* 리프 1 + 내부 1 */
        CHECK(m7.bytes - m3.bytes == 2 * leaf_plus_internal);
    }
    bpt_destroy(t);
}

static void test_storage_index_stats(void) {
    const char *dir = fresh_dir("index_stats");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    IndexStats ix;
    int64_t id;
    int i;

    CHECK(table_open(dir, &t, &err) == 0);
    table_index_stats(t, &ix);
    CHECK(ix.height == 1 && ix.keys == 0 && ix.leaves == 1 && ix.internals == 0 && ix.bytes > 0);
    for (i = 0; i < 64; i++) { /* 기본 차수 64: 64번째 키에서 첫 분할 */
        CHECK(table_append(t, "a", 1, &id, &err) == 0);
    }
    table_index_stats(t, &ix);
    CHECK(ix.height == 2 && ix.keys == 64 && ix.leaves == 2 && ix.internals == 1);
    CHECK(ix.height == table_index_height(t) && ix.keys == table_row_count(t));
    CHECK(table_close(t, &err) == 0);
}


/* ---------- Phase 3 / P3-01: DB 진입점, 중단 상태, 동시 정확성 ---------- */

static int count_row(const Row *row, void *ctx) {
    (void)row;
    (*(int *)ctx)++;
    return 0;
}

static int remember_id(int64_t id, void *ctx) {
    *(int64_t *)ctx = id;
    return 0;
}

static int db_insert(Db *db, const char *name, int age, int64_t *out_id, int64_t *stored, DbError *err) {
    Statement st = {STMT_INSERT, (char *)name, age, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    ResultSink sink = {remember_id, NULL, out_id, NULL, NULL};
    return db_exec(db, &st, &sink, stored, err);
}

static int db_count(Db *db, WhereKind where, int64_t v, DbError *err) {
    Statement st = {STMT_SELECT, NULL, 0, where, v, 0, CMP_EQ, 0, 0};
    int n = 0;
    ResultSink sink = {NULL, count_row, &n, NULL, NULL};
    return db_exec(db, &st, &sink, NULL, err) == 0 ? n : -1;
}

/* CLI와 같은 결과: db_exec는 executor_run을 잠금 안에서 부를 뿐이다. 오류 종류도 구분된다. */
static void test_db_exec_basic_and_kinds(void) {
    const char *dir = fresh_dir("p3_db_basic");
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int64_t id = 0, stored = -1;

    CHECK(db_open(dir, &db, &err) == 0);
    CHECK(db_state(db) == DB_OK);
    CHECK(db_insert(db, "a", 1, &id, &stored, &err) == 0 && id == 1 && stored == 0);
    CHECK(db_insert(db, "b", 2, &id, &stored, &err) == 0 && id == 2);
    CHECK(db_count(db, WHERE_ID, 2, &err) == 1);
    CHECK(db_count(db, WHERE_NONE, 0, &err) == 2);

    /* 값 오류: DBE_SQL, 상태는 그대로, 파일 변화 없음 */
    CHECK(db_insert(db, "c", 151, &id, &stored, &err) == -1);
    CHECK(err.kind == DBE_SQL && stored == 0);
    CHECK(db_state(db) == DB_OK);
    /* INSERT에 EXPLAIN 계획 요청 같은 엔진 쪽 문장 오류도 DBE_SQL */
    {
        Statement st = {STMT_INSERT, (char *)"x", 1, WHERE_NONE, 0, 1, CMP_EQ, 0, 0};
        QueryPlan plan;
        CHECK(executor_plan(NULL, &st, &plan, &err) == -1 && err.kind == DBE_SQL);
    }
    /* 결과 콜백 실패: DBE_SINK */
    {
        Statement st = {STMT_SELECT, NULL, 0, WHERE_ID, 1, 0, CMP_EQ, 0, 0};
        Collected c;
        ResultSink sink = {NULL, collect_row, &c, NULL, NULL};
        c.count = 0;
        c.stop_after = 1;
        CHECK(db_exec(db, &st, &sink, NULL, &err) == -1 && err.kind == DBE_SINK);
        CHECK(db_state(db) == DB_OK);
    }
    /* 선형 탐색이 last_id를 바꾸지 않음: 조회 뒤에도 다음 id는 3 */
    CHECK(db_count(db, WHERE_AGE, 2, &err) == 1);
    CHECK(db_insert(db, "c", 3, &id, &stored, &err) == 0 && id == 3);
    CHECK(db_close(db, &err) == 0);
    CHECK(db_close(NULL, &err) == 0);
}

/* 파일 기록 완료 뒤 인덱스 등록 실패 → stored_id, DB_HALTED_INDEX, 이후 실행 거부, 재시작으로 복구 */
static void test_db_halt_after_index_failure(void) {
    const char *dir = fresh_dir("p3_db_halt_index");
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int64_t id = 0, stored = -1;
    char name[16];
    int i, n = 0;
    Collected c;

    CHECK(db_open(dir, &db, &err) == 0);
    /* 차수 64: 리프에 63개가 차면 64번째 삽입이 분할(메모리 할당)을 한다 */
    for (i = 1; i <= 63; i++) {
        snprintf(name, sizeof name, "u%d", i);
        CHECK(db_insert(db, name, i % 151, &id, &stored, &err) == 0);
    }
    bpt_test_fail_alloc_after(0);
    CHECK(db_insert(db, "lost-index", 30, &id, &stored, &err) == -1);
    bpt_test_fail_alloc_after(-1);
    CHECK(err.kind == DBE_INDEX_FAILED);
    CHECK(stored == 64);
    CHECK(strstr(err.msg, "레코드는 저장됨") != NULL);
    CHECK(db_state(db) == DB_HALTED_INDEX);

    /* 중단 상태: 조회도 INSERT도 실행하지 않는다 */
    {
        Statement st = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
        ResultSink sink = {NULL, count_row, &n, NULL, NULL};
        CHECK(db_exec(db, &st, &sink, &stored, &err) == -1);
        CHECK(err.kind == DBE_HALTED && stored == 0 && n == 0);
        CHECK(strstr(err.msg, "인덱스를 다시 만듭니다") != NULL);
    }
    CHECK(db_insert(db, "after", 1, &id, &stored, &err) == -1 && err.kind == DBE_HALTED);
    CHECK(db_close(db, &err) == 0);

    /* 파일은 온전하므로 재시작하면 64번이 인덱스로 보인다. "after"는 없다 */
    CHECK(db_open(dir, &db, &err) == 0);
    {
        Statement st = {STMT_SELECT, NULL, 0, WHERE_ID, 64, 0, CMP_EQ, 0, 0};
        ResultSink sink = {NULL, collect_row, &c, NULL, NULL};
        c.count = 0;
        c.stop_after = 0;
        CHECK(db_exec(db, &st, &sink, NULL, &err) == 0);
        CHECK(c.count == 1 && c.ids[0] == 64 && strcmp(c.names[0], "lost-index") == 0);
    }
    CHECK(db_count(db, WHERE_NONE, 0, &err) == 64);
    CHECK(db_state(db) == DB_OK);
    CHECK(db_close(db, &err) == 0);
}

/* 쓰기 도중 실패 → stored_id 없음, DB_HALTED_WRITE. 줄 일부가 남으면 재시작이 손상으로 거부된다 */
static void test_db_halt_after_write_failure(void) {
    const char *dir = fresh_dir("p3_db_halt_write");
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int64_t id = 0, stored = -1;
    char buf[256];

    CHECK(db_open(dir, &db, &err) == 0);
    CHECK(db_insert(db, "a", 1, &id, &stored, &err) == 0);
    table_test_fail_write(1);
    CHECK(db_insert(db, "b", 2, &id, &stored, &err) == -1);
    CHECK(err.kind == DBE_WRITE_FAILED);
    CHECK(stored == 0); /* 기록 여부를 모르므로 알려 주지 않는다 */
    CHECK(db_state(db) == DB_HALTED_WRITE);
    CHECK(db_count(db, WHERE_ID, 1, &err) == -1 && err.kind == DBE_HALTED);
    CHECK(strstr(err.msg, "쓰기 실패 이후") != NULL);
    CHECK(db_close(db, &err) == 0);

    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\ta\t1\n2\t") == 0);
    CHECK(db_open(dir, &db, &err) == -1);
    CHECK(strstr(err.msg, "데이터 파일 손상") != NULL);
}

/*
 * 동시 정확성 (C1~C4). 스레드 여러 개가 db_exec로 INSERT와 여러 조회를 섞어 실행한다.
 * 각 스레드는 자기가 확실히 아는 사실만 검사한다(삭제가 없으므로 id는 1..최대까지 빈틈없음).
 * 검사 결과는 스레드별 카운터에 담고, CHECK는 모든 스레드가 끝난 뒤 메인 스레드에서 한다.
 */
#define CT_THREADS 8
#define CT_OPS 400
#define CT_MAX_INS CT_OPS

typedef struct {
    Db *db;
    int tid;
    uint32_t seed;
    int n_ins;
    int64_t ids[CT_MAX_INS];
    char names[CT_MAX_INS][32];
    int bad_insert;    /* INSERT 실패 */
    int bad_readback;  /* 방금 넣은 행을 id로 못 찾음 (C3) */
    int bad_lookup;    /* 이미 있다고 아는 id를 못 찾거나 엉뚱한 행 */
    int bad_range;     /* 범위 결과가 a부터 빈틈없이 이어지지 않음, 아는 최대보다 짧음 */
    int bad_scan;      /* 전체 결과가 1부터 빈틈없지 않음, 아는 최대보다 짧음 */
    int bad_plan;      /* EXPLAIN 행 수가 아는 최대보다 작음 */
    int errors;        /* 그 밖의 엔진 오류 */
    char first_error[256];
} CtState;

typedef struct {
    int64_t expect_next; /* 다음에 와야 하는 id */
    int64_t count;
    int broken;
    const char *want_name; /* NULL이 아니면 첫 행의 이름도 확인 */
} SeqCheck;

static int seq_row(const Row *row, void *ctx) {
    SeqCheck *s = ctx;
    if (row->id != s->expect_next) {
        s->broken = 1;
    }
    if (s->want_name != NULL && s->count == 0 && strcmp(row->name, s->want_name) != 0) {
        s->broken = 1;
    }
    s->expect_next = row->id + 1;
    s->count++;
    return 0;
}

typedef struct {
    int64_t total_rows;
} PlanSeen;

static int plan_seen(const QueryPlan *plan, void *ctx) {
    ((PlanSeen *)ctx)->total_rows = plan->stats.total_rows;
    return 0;
}

static uint32_t ct_rand(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void ct_error(CtState *cs, const DbError *err) {
    if (cs->errors++ == 0) {
        snprintf(cs->first_error, sizeof cs->first_error, "%s", err->msg);
    }
}

static void *ct_worker(void *arg) {
    CtState *cs = arg;
    int64_t known_max = 0; /* 이 스레드가 "이미 있다"고 확신하는 가장 큰 id */
    int op;

    for (op = 0; op < CT_OPS; op++) {
        uint32_t k = ct_rand(&cs->seed) % 100;
        DbError err = {{0}, DBE_NONE};

        if (k < 40) {
            char name[32];
            int64_t id = 0;
            SeqCheck sc = {0, 0, 0, NULL};
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, 0, 0, CMP_EQ, 0, 0};
            ResultSink rs = {NULL, seq_row, &sc, NULL, NULL};

            snprintf(name, sizeof name, "t%d_%d", cs->tid, op);
            if (db_insert(cs->db, name, (cs->tid * 7 + op) % 151, &id, NULL, &err) != 0) {
                cs->bad_insert++;
                ct_error(cs, &err);
                continue;
            }
            snprintf(cs->names[cs->n_ins], sizeof cs->names[0], "%s", name);
            cs->ids[cs->n_ins++] = id;
            if (id > known_max) {
                known_max = id;
            }
            /* C3: 성공을 받은 INSERT는 바로 다음 조회에 보인다 */
            sel.where_value = id;
            sc.expect_next = id;
            sc.want_name = name;
            if (db_exec(cs->db, &sel, &rs, NULL, &err) != 0) {
                ct_error(cs, &err);
            } else if (sc.count != 1 || sc.broken) {
                cs->bad_readback++;
            }
        } else if (k < 60) {
            int64_t x;
            SeqCheck sc = {0, 0, 0, NULL};
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, 0, 0, CMP_EQ, 0, 0};
            ResultSink rs = {NULL, seq_row, &sc, NULL, NULL};

            if (known_max == 0) {
                continue;
            }
            x = 1 + (int64_t)(ct_rand(&cs->seed) % (uint32_t)known_max);
            sel.where_value = x;
            sc.expect_next = x;
            if (db_exec(cs->db, &sel, &rs, NULL, &err) != 0) {
                ct_error(cs, &err);
            } else if (sc.count != 1 || sc.broken) {
                cs->bad_lookup++;
            }
        } else if (k < 80) {
            int64_t a, b, want;
            SeqCheck sc = {0, 0, 0, NULL};
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID_RANGE, 0, 0, CMP_BETWEEN, 0, 0};
            ResultSink rs = {NULL, seq_row, &sc, NULL, NULL};

            if (known_max == 0) {
                continue;
            }
            a = 1 + (int64_t)(ct_rand(&cs->seed) % (uint32_t)known_max);
            b = a + 20;
            sel.where_value = a;
            sel.where_value2 = b;
            sc.expect_next = a;
            want = (b < known_max ? b : known_max) - a + 1; /* 적어도 이만큼은 있어야 한다 */
            if (db_exec(cs->db, &sel, &rs, NULL, &err) != 0) {
                ct_error(cs, &err);
            } else if (sc.broken || sc.count < want || sc.count > 21) {
                cs->bad_range++;
            }
        } else if (k < 90) {
            SeqCheck sc = {1, 0, 0, NULL};
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
            ResultSink rs = {NULL, seq_row, &sc, NULL, NULL};

            if (db_exec(cs->db, &sel, &rs, NULL, &err) != 0) {
                ct_error(cs, &err);
            } else if (sc.broken || sc.count < known_max) {
                cs->bad_scan++;
            } else {
                known_max = sc.count; /* 전체를 봤으므로 그만큼은 확실히 있다 */
            }
        } else {
            PlanSeen ps = {-1};
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_AGE, 30, 1, CMP_EQ, 0, 0};
            ResultSink rs = {NULL, NULL, &ps, plan_seen, NULL};

            if (db_exec(cs->db, &sel, &rs, NULL, &err) != 0) {
                ct_error(cs, &err);
            } else if (ps.total_rows < known_max) {
                cs->bad_plan++;
            }
        }
    }
    return NULL;
}

typedef struct {
    int64_t count;
    int64_t next;
    int broken;
    int64_t total;
    const char *const *names; /* names[1..total] */
} FinalCheck;

static int final_row(const Row *row, void *ctx) {
    FinalCheck *f = ctx;
    if (row->id != f->next || row->id > f->total || f->names[row->id] == NULL ||
        strcmp(row->name, f->names[row->id]) != 0) {
        f->broken = 1;
    }
    f->next++;
    f->count++;
    return 0;
}

static void test_db_concurrent_correctness(void) {
    const char *dir = fresh_dir("p3_db_concurrent");
    static CtState st[CT_THREADS];
    static const char *names[CT_THREADS * CT_MAX_INS + 1];
    static unsigned char seen[CT_THREADS * CT_MAX_INS + 1];
    pthread_t th[CT_THREADS];
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int64_t total = 0, dup = 0, out_of_range = 0;
    char why[200] = "";
    int i, j;

    CHECK(db_open(dir, &db, &err) == 0);
    for (i = 0; i < CT_THREADS; i++) {
        memset(&st[i], 0, sizeof st[i]);
        st[i].db = db;
        st[i].tid = i;
        st[i].seed = 0x9E3779B9u * (uint32_t)(i + 1);
        CHECK(pthread_create(&th[i], NULL, ct_worker, &st[i]) == 0);
    }
    for (i = 0; i < CT_THREADS; i++) {
        pthread_join(th[i], NULL);
    }

    memset(seen, 0, sizeof seen);
    memset(names, 0, sizeof names);
    for (i = 0; i < CT_THREADS; i++) {
        CtState *cs = &st[i];
        if (cs->errors != 0) {
            fprintf(stderr, "    스레드 %d 첫 오류: %s\n", i, cs->first_error);
        }
        CHECK(cs->errors == 0);
        CHECK(cs->bad_insert == 0);
        CHECK(cs->bad_readback == 0);
        CHECK(cs->bad_lookup == 0);
        CHECK(cs->bad_range == 0);
        CHECK(cs->bad_scan == 0);
        CHECK(cs->bad_plan == 0);
        total += cs->n_ins;
    }
    /* C1: 받은 id를 모두 모으면 1..total이 정확히 한 번씩 */
    for (i = 0; i < CT_THREADS; i++) {
        for (j = 0; j < st[i].n_ins; j++) {
            int64_t id = st[i].ids[j];
            if (id < 1 || id > total) {
                out_of_range++;
                continue;
            }
            if (seen[id]++) {
                dup++;
            }
            names[id] = st[i].names[j];
        }
    }
    CHECK(total > 0);
    CHECK(dup == 0);
    CHECK(out_of_range == 0);

    /* 각 id가 그 id를 받은 스레드가 넣은 이름을 가리킨다 (인덱스 → 위치 → 줄) */
    for (i = 0; i < CT_THREADS; i++) {
        for (j = 0; j < st[i].n_ins; j++) {
            Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, st[i].ids[j], 0, CMP_EQ, 0, 0};
            Collected c;
            ResultSink rs = {NULL, collect_row, &c, NULL, NULL};

            c.count = 0;
            c.stop_after = 0;
            CHECK(db_exec(db, &sel, &rs, NULL, &err) == 0);
            CHECK(c.count == 1 && strcmp(c.names[0], st[i].names[j]) == 0);
        }
    }
    /* C4: 트리 불변식, 행 수, 재시작 뒤 같은 내용 */
    CHECK(db_test_validate(db, why, sizeof why) == 0);
    if (why[0] != '\0') {
        fprintf(stderr, "    트리 검사: %s\n", why);
    }
    CHECK(db_count(db, WHERE_NONE, 0, &err) == total);
    CHECK(db_state(db) == DB_OK);
    CHECK(db_close(db, &err) == 0);

    CHECK(db_open(dir, &db, &err) == 0);
    {
        FinalCheck f = {0, 1, 0, total, names};
        Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
        ResultSink rs = {NULL, final_row, &f, NULL, NULL};
        CHECK(db_exec(db, &sel, &rs, NULL, &err) == 0);
        CHECK(f.count == total && !f.broken);
    }
    CHECK(db_close(db, &err) == 0);
}

/* ---------- Phase 3 / P3-02: JSON 응답 버퍼 ---------- */

static int jb_is(const JsonBuf *b, const char *want) {
    return b->len == strlen(want) && (b->len == 0 || memcmp(b->data, want, b->len) == 0);
}

static void test_json_escape_and_utf8(void) {
    JsonBuf b;

    jb_init(&b, 1024);
    CHECK(jb_str(&b, "a\"b\\c") == 0 && jb_is(&b, "\"a\\\"b\\\\c\""));
    jb_truncate(&b, 0);
    CHECK(jb_strn(&b, "\x01\t\n\x1f", 4) == 0 && jb_is(&b, "\"\\u0001\\u0009\\u000a\\u001f\""));
    jb_truncate(&b, 0);
    CHECK(jb_str(&b, "민수 😀 é") == 0 && jb_is(&b, "\"민수 😀 é\"")); /* 올바른 UTF-8은 그대로 */
    jb_truncate(&b, 0);
    /* 잘못된 바이트·너무 긴 표현·서로게이트·잘린 글자 → 대체 문자 */
    CHECK(jb_str(&b, "a\xff" "b") == 0 && jb_is(&b, "\"a\\ufffdb\""));
    jb_truncate(&b, 0);
    CHECK(jb_str(&b, "\xc0\x80") == 0 && jb_is(&b, "\"\\ufffd\\ufffd\""));
    jb_truncate(&b, 0);
    CHECK(jb_str(&b, "\xed\xa0\x80") == 0 && jb_is(&b, "\"\\ufffd\\ufffd\\ufffd\""));
    jb_truncate(&b, 0);
    CHECK(jb_str(&b, "x\xea\xb0") == 0 && jb_is(&b, "\"x\\ufffd\\ufffd\""));
    jb_truncate(&b, 0);
    CHECK(jb_str(&b, "\xf4\x90\x80\x80") == 0 && jb_is(&b, "\"\\ufffd\\ufffd\\ufffd\\ufffd\""));
    jb_truncate(&b, 0);
    CHECK(jb_i64(&b, INT64_MIN) == 0 && jb_is(&b, "-9223372036854775808"));
    CHECK(!b.over && !b.oom);
    jb_free(&b);
}

static void test_json_budget(void) {
    JsonBuf b;

    jb_init(&b, 10);
    CHECK(jb_lit(&b, "12345") == 0);
    CHECK(jb_lit(&b, "678901") == -1 && b.over && jb_is(&b, "12345")); /* 넘치는 쓰기는 통째로 거절 */
    b.over = 0;
    CHECK(jb_lit(&b, "67890") == 0 && jb_is(&b, "1234567890") && !b.over); /* 딱 상한까지는 됨 */
    CHECK(jb_reserve(&b, 1) == -1 && b.over);
    CHECK(b.cap <= 10);
    jb_truncate(&b, 3);
    b.over = 0;
    CHECK(jb_reserve(&b, 7) == 0 && jb_reserve(&b, 8) == -1);
    /* 문자열이 중간에 넘치면 앞부분도 남기지 않는다 */
    b.over = 0;
    CHECK(jb_str(&b, "abcdefgh") == -1 && b.over && jb_is(&b, "123"));
    jb_free(&b);
    jb_init(&b, 0);
    CHECK(jb_lit(&b, "") == 0 && jb_lit(&b, "x") == -1);
    jb_free(&b);
}

/* ---------- Phase 3 / P3-04: 읽기 병렬화 ---------- */

#define PR_ROWS 3000
#define PR_THREADS 8
#define PR_OPS 300

typedef struct {
    Table *t;
    uint32_t seed;
    int bad;    /* 엉뚱한 행, 빠진 행 */
    int errors; /* 엔진 오류 */
    char first_error[256];
} PrState;

static void pr_name(int64_t id, char *buf, size_t cap) {
    snprintf(buf, cap, "row%" PRId64, id);
}

typedef struct {
    int64_t next;
    int64_t count;
    int broken;
} PrSeq;

static int pr_row(const Row *row, void *ctx) {
    PrSeq *s = ctx;
    char want[32];

    pr_name(row->id, want, sizeof want);
    if (row->id != s->next || strcmp(row->name, want) != 0 || row->age != (int)(row->id % 151)) {
        s->broken = 1;
    }
    s->next = row->id + 1;
    s->count++;
    return 0;
}

static void *pr_worker(void *arg) {
    PrState *ps = arg;
    int op;

    for (op = 0; op < PR_OPS; op++) {
        uint32_t k = ct_rand(&ps->seed) % 100;
        int64_t a = 1 + (int64_t)(ct_rand(&ps->seed) % PR_ROWS);
        PrSeq s = {a, 0, 0};
        DbError err = {{0}, DBE_NONE};
        int rc, want;

        if (k < 60) {
            rc = table_find_by_id(ps->t, a, pr_row, &s, &err);
            want = 1;
        } else if (k < 95) {
            int64_t b = a + 40 > PR_ROWS ? PR_ROWS : a + 40;
            rc = table_find_id_range(ps->t, a, b, pr_row, &s, &err);
            want = (int)(b - a + 1);
        } else {
            s.next = 1;
            rc = table_scan(ps->t, pr_row, &s, &err);
            want = PR_ROWS;
        }
        if (rc != 0) {
            if (ps->errors++ == 0) {
                snprintf(ps->first_error, sizeof ps->first_error, "%s", err.msg);
            }
        } else if (s.broken || s.count != want) {
            ps->bad++;
        }
    }
    return NULL;
}

/* 조회 경로는 공유 상태를 쓰지 않으므로 잠금 없이 여러 스레드가 같은 Table을 읽어도 결과가 맞다.
   (P3-01까지는 공유 FILE*의 위치가 섞여 "인덱스와 데이터 파일이 일치하지 않습니다"가 났다: tasks.md S1x)
   TSan 빌드에서는 조회 경로의 데이터 경쟁도 함께 검사한다. */
static void test_storage_parallel_reads_without_lock(void) {
    const char *dir = fresh_dir("p3_parallel_reads");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    PrState st[PR_THREADS];
    pthread_t th[PR_THREADS];
    int64_t i, id;
    int j;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    for (i = 1; i <= PR_ROWS; i++) {
        char name[32];
        pr_name(i, name, sizeof name);
        CHECK(table_append(t, name, (int)(i % 151), &id, &err) == 0 && id == i);
    }
    for (j = 0; j < PR_THREADS; j++) {
        memset(&st[j], 0, sizeof st[j]);
        st[j].t = t;
        st[j].seed = 0x51ED270Bu * (uint32_t)(j + 1);
        CHECK(pthread_create(&th[j], NULL, pr_worker, &st[j]) == 0);
    }
    for (j = 0; j < PR_THREADS; j++) {
        pthread_join(th[j], NULL);
        if (st[j].errors != 0) {
            fprintf(stderr, "    스레드 %d 첫 오류: %s\n", j, st[j].first_error);
        }
        CHECK(st[j].errors == 0);
        CHECK(st[j].bad == 0);
    }
    CHECK(table_close(t, &err) == 0);
}

/* 조회는 시작 시점의 file_size까지만 읽는다. 그 뒤에 붙은(아직 끝나지 않은) 바이트는 보지 않는다.
   서버에서는 INSERT가 배타 잠금 안에서 줄을 다 쓴 뒤에야 file_size를 늘리므로, 이 장치는 잠금과 별도로
   "반쯤 쓰인 줄은 조회에 보이지 않는다"(C2)를 코드에 드러낸다. 여기서는 파일 끝에 직접 덧붙여 확인한다. */
static void test_storage_scan_stops_at_file_size(void) {
    const char *dir = fresh_dir("p3_file_size");
    char file[600];
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    Collected c;
    int64_t id;
    FILE *fp;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    CHECK(table_append(t, "a", 1, &id, &err) == 0 && table_append(t, "b", 2, &id, &err) == 0);
    snprintf(file, sizeof file, "%s/users.tsv", dir);
    fp = fopen(file, "ab");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fputs("3\tpartial", fp); /* 개행이 없는 줄 앞부분: 끝까지 읽으면 "손상"이다 */
        fclose(fp);
    }
    c.count = 0;
    c.stop_after = 0;
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 2 && c.ids[1] == 2);
    c.count = 0;
    CHECK(table_find_id_range(t, 1, 9, collect_row, &c, &err) == 0 && c.count == 2);
    CHECK(table_close(t, &err) == 0);
    /* 다시 열면 실제 파일 끝까지 검증하므로 손상으로 거부된다(시작 시 검증은 그대로) */
    CHECK(table_open(dir, &t, &err) == -1 && strstr(err.msg, "개행") != NULL);
}

/* 잠금 동작을 직접 확인하는 도우미: 콜백 안(=엔진 잠금 안)에서 서로를 기다린다. 기다림에는 기한이 있어서
   잠금이 기대와 다르면 멈추지 않고 실패한다. */
typedef struct {
    Db *db;
    pthread_mutex_t m;
    pthread_cond_t c;
    int inside;       /* 지금 콜백 안에 있는 읽기 수 */
    int met;          /* 한 번이라도 둘이 동시에 콜백 안에 있었다 */
    int saw_both;     /* 콜백 안에서 다른 읽기도 들어온 것을 본 읽기 수 */
    int insert_done;  /* INSERT의 db_exec가 끝났다 */
    int insert_early; /* 읽기가 잠금 안에 있는 동안 INSERT가 끝났다(배타 잠금 위반) */
    int mode;         /* 0: 둘이 만나기, 1: 안에서 잠깐 버티기 */
} LockProbe;

static void deadline_ms(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int probe_row(const Row *row, void *ctx) {
    LockProbe *p = ctx;
    struct timespec ts;

    (void)row;
    pthread_mutex_lock(&p->m);
    p->inside++;
    if (p->inside >= 2) {
        p->met = 1; /* 먼저 나간 쪽이 inside를 줄여도 만났다는 사실은 남는다 */
    }
    pthread_cond_broadcast(&p->c);
    if (p->mode == 0) {
        deadline_ms(&ts, 3000);
        while (!p->met && pthread_cond_timedwait(&p->c, &p->m, &ts) == 0) {
        }
        if (p->met) {
            p->saw_both++;
        }
    } else {
        deadline_ms(&ts, 300); /* 300 ms 버티는 동안 INSERT가 끝나면 안 된다 */
        while (pthread_cond_timedwait(&p->c, &p->m, &ts) == 0) {
        }
        if (p->insert_done) {
            p->insert_early = 1;
        }
    }
    p->inside--;
    pthread_mutex_unlock(&p->m);
    return 0;
}

static void *probe_reader(void *arg) {
    LockProbe *p = arg;
    Statement st = {STMT_SELECT, NULL, 0, WHERE_ID, 1, 0, CMP_EQ, 0, 0};
    ResultSink rs = {NULL, probe_row, p, NULL, NULL};
    DbError err = {{0}, DBE_NONE};
    db_exec(p->db, &st, &rs, NULL, &err);
    return NULL;
}

static void *probe_writer(void *arg) {
    LockProbe *p = arg;
    DbError err = {{0}, DBE_NONE};
    int64_t id = 0;

    db_insert(p->db, "w", 1, &id, NULL, &err);
    pthread_mutex_lock(&p->m);
    p->insert_done = 1;
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
    return NULL;
}

/* SELECT 두 개는 동시에 잠금 안에 들어간다(공유 잠금). 전역 잠금이면 둘째가 들어오지 못해 3초 뒤 실패한다. */
static void test_db_readers_run_together(void) {
    const char *dir = fresh_dir("p3_readers_together");
    LockProbe p;
    pthread_t a, b;
    DbError err = {{0}, DBE_NONE};
    int64_t id;

    memset(&p, 0, sizeof p);
    CHECK(db_open(dir, &p.db, &err) == 0);
    CHECK(db_insert(p.db, "r", 1, &id, NULL, &err) == 0);
    pthread_mutex_init(&p.m, NULL);
    pthread_cond_init(&p.c, NULL);
    p.mode = 0;
    CHECK(pthread_create(&a, NULL, probe_reader, &p) == 0);
    CHECK(pthread_create(&b, NULL, probe_reader, &p) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK(p.saw_both == 2);
    pthread_cond_destroy(&p.c);
    pthread_mutex_destroy(&p.m);
    CHECK(db_close(p.db, &err) == 0);
}

/* SELECT가 잠금 안에 있는 동안 INSERT는 끝나지 않는다(배타 잠금). 읽기가 나간 뒤에는 끝난다. */
static void test_db_writer_waits_for_reader(void) {
    const char *dir = fresh_dir("p3_writer_waits");
    LockProbe p;
    pthread_t r, w;
    DbError err = {{0}, DBE_NONE};
    struct timespec ts;
    int64_t id;

    memset(&p, 0, sizeof p);
    CHECK(db_open(dir, &p.db, &err) == 0);
    CHECK(db_insert(p.db, "r", 1, &id, NULL, &err) == 0);
    pthread_mutex_init(&p.m, NULL);
    pthread_cond_init(&p.c, NULL);
    p.mode = 1;
    CHECK(pthread_create(&r, NULL, probe_reader, &p) == 0);
    pthread_mutex_lock(&p.m); /* 읽기가 콜백 안(잠금 안)에 들어올 때까지 기다린 뒤 INSERT를 시작한다 */
    deadline_ms(&ts, 3000);
    while (p.inside == 0 && pthread_cond_timedwait(&p.c, &p.m, &ts) == 0) {
    }
    pthread_mutex_unlock(&p.m);
    CHECK(pthread_create(&w, NULL, probe_writer, &p) == 0);
    pthread_join(r, NULL);
    pthread_join(w, NULL);
    CHECK(p.insert_early == 0);
    CHECK(p.insert_done == 1);
    CHECK(db_count(p.db, WHERE_NONE, 0, &err) == 2);
    pthread_cond_destroy(&p.c);
    pthread_mutex_destroy(&p.m);
    CHECK(db_close(p.db, &err) == 0);
}

#ifdef __GLIBC__
/* 쓰기 우선(glibc): 읽기 R1이 잠금을 잡은 동안 INSERT W가 기다리기 시작하면, 그 뒤에 온 읽기 R2는 W보다
   먼저 들어가지 못한다. 읽기 우선(기본값)이면 R2가 바로 들어가고, 선형 탐색이 겹쳐 계속 들어오면
   INSERT가 끝없이 기다린다(P3-04 측정에서 20초 넘게 응답 없음). */
typedef struct {
    Db *db;
    pthread_mutex_t m;
    pthread_cond_t c;
    int r1_inside, release, seq, w_seq, r2_seq;
} WriterFirst;

static void wait_ms(WriterFirst *p, long ms) { /* p->m을 잡은 채로 부른다 */
    struct timespec ts;
    deadline_ms(&ts, ms);
    while (pthread_cond_timedwait(&p->c, &p->m, &ts) == 0) {
    }
}

static int wf_r1_row(const Row *row, void *ctx) {
    WriterFirst *p = ctx;
    struct timespec ts;
    (void)row;
    pthread_mutex_lock(&p->m);
    p->r1_inside = 1;
    pthread_cond_broadcast(&p->c);
    deadline_ms(&ts, 5000);
    while (!p->release && pthread_cond_timedwait(&p->c, &p->m, &ts) == 0) {
    }
    pthread_mutex_unlock(&p->m);
    return 0;
}

static int wf_r2_row(const Row *row, void *ctx) {
    WriterFirst *p = ctx;
    (void)row;
    pthread_mutex_lock(&p->m);
    p->r2_seq = ++p->seq;
    pthread_mutex_unlock(&p->m);
    return 0;
}

static void *wf_reader(void *arg, RowFn fn) {
    WriterFirst *p = arg;
    Statement st = {STMT_SELECT, NULL, 0, WHERE_ID, 1, 0, CMP_EQ, 0, 0};
    ResultSink rs = {NULL, fn, p, NULL, NULL};
    DbError err = {{0}, DBE_NONE};
    db_exec(p->db, &st, &rs, NULL, &err);
    return NULL;
}

static void *wf_r1(void *arg) { return wf_reader(arg, wf_r1_row); }
static void *wf_r2(void *arg) { return wf_reader(arg, wf_r2_row); }

static int wf_on_insert(int64_t id, void *ctx) { /* 엔진 잠금 안에서 불린다: 순서를 잠금 안에서 기록 */
    WriterFirst *p = ctx;
    (void)id;
    pthread_mutex_lock(&p->m);
    p->w_seq = ++p->seq;
    pthread_mutex_unlock(&p->m);
    return 0;
}

static void *wf_w(void *arg) {
    WriterFirst *p = arg;
    Statement st = {STMT_INSERT, "w", 1, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
    ResultSink rs = {wf_on_insert, NULL, p, NULL, NULL};
    DbError err = {{0}, DBE_NONE};
    db_exec(p->db, &st, &rs, NULL, &err);
    return NULL;
}

static void test_db_writer_not_starved(void) {
    const char *dir = fresh_dir("p3_writer_first");
    WriterFirst p;
    pthread_t r1, r2, w;
    DbError err = {{0}, DBE_NONE};
    int64_t id;
    int r2_early;
    struct timespec ts;

    memset(&p, 0, sizeof p);
    CHECK(db_open(dir, &p.db, &err) == 0);
    CHECK(db_insert(p.db, "r", 1, &id, NULL, &err) == 0);
    pthread_mutex_init(&p.m, NULL);
    pthread_cond_init(&p.c, NULL);
    CHECK(pthread_create(&r1, NULL, wf_r1, &p) == 0);
    pthread_mutex_lock(&p.m);
    deadline_ms(&ts, 3000);
    while (!p.r1_inside && pthread_cond_timedwait(&p.c, &p.m, &ts) == 0) {
    }
    pthread_mutex_unlock(&p.m);
    CHECK(pthread_create(&w, NULL, wf_w, &p) == 0); /* 배타 잠금을 기다리기 시작한다 */
    pthread_mutex_lock(&p.m);
    wait_ms(&p, 200);
    pthread_mutex_unlock(&p.m);
    CHECK(pthread_create(&r2, NULL, wf_r2, &p) == 0);
    pthread_mutex_lock(&p.m);
    wait_ms(&p, 200);
    r2_early = p.r2_seq != 0; /* R1이 아직 잠금 안인데 R2가 들어왔다 = 읽기 우선 */
    p.release = 1;
    pthread_cond_broadcast(&p.c);
    pthread_mutex_unlock(&p.m);
    pthread_join(r1, NULL);
    pthread_join(w, NULL);
    pthread_join(r2, NULL);
    CHECK(!r2_early);
    CHECK(p.w_seq > 0 && p.r2_seq > p.w_seq); /* INSERT가 먼저, 그 뒤에 R2 */
    pthread_cond_destroy(&p.c);
    pthread_mutex_destroy(&p.m);
    CHECK(db_close(p.db, &err) == 0);
}
#endif

/* ---------- 작업 큐 (P3-03) ---------- */

static void test_workqueue_basic(void) {
    WorkQueue q;
    int fd = -1, rest[3], i;

    CHECK(wq_init(&q, 0) == -1);
    CHECK(wq_init(&q, 3) == 0);
    /* 넣은 순서대로 나온다(고리 버퍼가 한 바퀴 돌아도) */
    for (i = 0; i < 3; i++) {
        CHECK(wq_try_push(&q, 10 + i) == 0);
    }
    CHECK(wq_try_push(&q, 99) == 1); /* 가득 참: 기다리지 않고 바로 실패 */
    CHECK(wq_pop(&q, &fd) == 0 && fd == 10);
    CHECK(wq_try_push(&q, 13) == 0); /* 빈자리 하나 → 다시 들어감 (끝을 넘어 처음 칸에) */
    CHECK(wq_pop(&q, &fd) == 0 && fd == 11);
    /* 닫으면 아무도 꺼내지 않은 것만 순서대로 돌려준다 */
    CHECK(wq_close(&q, rest) == 2 && rest[0] == 12 && rest[1] == 13);
    CHECK(wq_close(&q, rest) == 0);
    CHECK(wq_pop(&q, &fd) == -1);       /* 닫힌 뒤에는 기다리지 않고 -1 */
    CHECK(wq_try_push(&q, 20) == 2);    /* 닫힌 뒤에는 넣을 수 없다 */
    wq_destroy(&q);
}

#define WQ_PRODUCERS 4
#define WQ_CONSUMERS 4
#define WQ_PER_PRODUCER 5000

typedef struct {
    WorkQueue *q;
    int base;
    int full; /* 가득 차서 다시 시도한 횟수 */
} WqProducer;

typedef struct {
    WorkQueue *q;
    unsigned char *seen; /* 소비자끼리 겹치지 않는 값만 쓰므로 잠금이 필요 없다 */
    int got;
    int dup;
} WqConsumer;

static void *wq_producer_main(void *p) {
    WqProducer *w = p;
    int i, r;

    for (i = 0; i < WQ_PER_PRODUCER; i++) {
        while ((r = wq_try_push(w->q, w->base + i)) == 1) {
            w->full++;
            sched_yield();
        }
    }
    return NULL;
}

static void *wq_consumer_main(void *p) {
    WqConsumer *c = p;
    int v;

    while (wq_pop(c->q, &v) == 0) {
        if (v < 0 || v >= WQ_PRODUCERS * WQ_PER_PRODUCER || c->seen[v]) {
            c->dup++;
        } else {
            c->seen[v] = 1;
        }
        c->got++;
    }
    return NULL;
}

/* 여러 생산자·소비자가 동시에 써도 값이 빠지거나 두 번 나오지 않는다. 닫으면 잠든 소비자가 모두 깨어난다. */
static void test_workqueue_threads(void) {
    WorkQueue q;
    pthread_t pt[WQ_PRODUCERS], ct[WQ_CONSUMERS];
    WqProducer pr[WQ_PRODUCERS];
    WqConsumer co[WQ_CONSUMERS];
    unsigned char *seen = calloc(WQ_PRODUCERS * WQ_PER_PRODUCER, 1);
    int rest[8], i, total = 0, dup = 0, missing = 0, created = 1;
    size_t nrest;

    CHECK(seen != NULL && wq_init(&q, 8) == 0);
    if (seen == NULL) {
        return;
    }
    for (i = 0; i < WQ_CONSUMERS; i++) {
        co[i].q = &q;
        co[i].seen = seen;
        co[i].got = co[i].dup = 0;
        created &= pthread_create(&ct[i], NULL, wq_consumer_main, &co[i]) == 0;
    }
    for (i = 0; i < WQ_PRODUCERS; i++) {
        pr[i].q = &q;
        pr[i].base = i * WQ_PER_PRODUCER;
        pr[i].full = 0;
        created &= pthread_create(&pt[i], NULL, wq_producer_main, &pr[i]) == 0;
    }
    CHECK(created);
    for (i = 0; i < WQ_PRODUCERS; i++) {
        pthread_join(pt[i], NULL);
    }
    /* 생산이 끝난 뒤 소비자가 큐를 비울 때까지 기다린 다음 닫는다 */
    for (;;) {
        size_t len;
        pthread_mutex_lock(&q.mu);
        len = q.len;
        pthread_mutex_unlock(&q.mu);
        if (len == 0) {
            break;
        }
        sched_yield();
    }
    nrest = wq_close(&q, rest);
    for (i = 0; i < WQ_CONSUMERS; i++) {
        pthread_join(ct[i], NULL); /* 닫지 않으면 여기서 영영 멈춘다 */
        total += co[i].got;
        dup += co[i].dup;
    }
    for (i = 0; i < WQ_PRODUCERS * WQ_PER_PRODUCER; i++) {
        missing += !seen[i];
    }
    CHECK(nrest == 0);
    CHECK(total == WQ_PRODUCERS * WQ_PER_PRODUCER);
    CHECK(dup == 0);
    CHECK(missing == 0);
    wq_destroy(&q);
    free(seen);
}

/* ---------- Phase 4: UPDATE·DELETE ---------- */

typedef struct {
    int calls;
    StmtKind kind;
    int64_t count;
} ChangeSeen;

static int seen_change(StmtKind kind, int64_t count, void *ctx) {
    ChangeSeen *c = ctx;
    c->calls++;
    c->kind = kind;
    c->count = count;
    return 0;
}

/* 문장 하나를 파싱해 실행한다. 반환: on_change로 받은 행 수, -2 파싱·실행 실패(err), -3 on_change 이상 */
static int64_t run_change_sql(Table *t, Db *db, const char *sql, DbError *err) {
    Statement st;
    ChangeSeen c = {0, STMT_SELECT, -1};
    ResultSink sink = {NULL, NULL, &c, NULL, seen_change};
    StmtKind kind;
    int rc;

    if (parse_one(sql, &st, err) != 1) {
        return -2;
    }
    kind = st.kind;
    rc = t != NULL ? executor_run(t, &st, &sink, err) : db_exec(db, &st, &sink, NULL, err);
    statement_free(&st);
    if (rc != 0) {
        return c.calls == 0 ? -2 : -3;
    }
    return c.calls == 1 && c.kind == kind ? c.count : -3;
}

#define t_change(t, sql, err) run_change_sql((t), NULL, (sql), (err))
#define d_change(db, sql, err) run_change_sql(NULL, (db), (sql), (err))

static int file_exists(const char *dir, const char *name) {
    char path[600];
    FILE *fp;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static void write_named_n(const char *dir, const char *name, const char *data, size_t len) {
    char path[600];
    FILE *fp;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "테스트 파일 생성 실패: %s\n", path);
        exit(2);
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

static void write_named(const char *dir, const char *name, const char *data) {
    write_named_n(dir, name, data, strlen(data));
}

/* 파일 바이트 전체를 buf에 읽는다(NUL 포함). 읽은 길이, 없으면 0 */
static size_t read_named_n(const char *dir, const char *name, char *buf, size_t cap) {
    char path[600];
    FILE *fp;
    size_t n = 0;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (fp != NULL) {
        n = fread(buf, 1, cap, fp);
        fclose(fp);
    }
    return n;
}

static void read_named(const char *dir, const char *name, char *buf, size_t cap) {
    char path[600];
    FILE *fp;
    size_t n = 0;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (fp != NULL) {
        n = fread(buf, 1, cap - 1, fp);
        fclose(fp);
    }
    buf[n] = '\0';
}

typedef struct {
    char buf[8192];
    size_t len;
} Dump;

static int dump_row(const Row *row, void *ctx) {
    Dump *d = ctx;
    int n = snprintf(d->buf + d->len, sizeof d->buf - d->len, "%" PRId64 "\t%s\t%d\n", row->id, row->name, row->age);

    if (n < 0 || (size_t)n >= sizeof d->buf - d->len) {
        return 1;
    }
    d->len += (size_t)n;
    return 0;
}

/* D2: 고치고 지운 결과가 파일·인덱스·범위 조회에 그대로 보이고, 다시 열어도 같다 */
static void test_p4_update_delete_basic(void) {
    const char *dir = fresh_dir("p4_basic");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char buf[512];
    char why[200] = "";
    int64_t id;
    Collected c;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    CHECK(table_append(t, "a", 10, &id, &err) == 0);
    CHECK(table_append(t, "b", 20, &id, &err) == 0);
    CHECK(table_append(t, "c", 20, &id, &err) == 0);
    CHECK(table_append(t, "d", 30, &id, &err) == 0);
    CHECK(table_append(t, "e", 40, &id, &err) == 0);

    CHECK(t_change(t, "UPDATE users SET age = 21 WHERE age = 20;", &err) == 2);          /* 선형 탐색 */
    CHECK(t_change(t, "UPDATE users SET name = 'zz', age = 5 WHERE id = 1;", &err) == 1); /* 인덱스 조회 */
    CHECK(t_change(t, "UPDATE users SET name = 'r' WHERE id BETWEEN 4 AND 9;", &err) == 2); /* 범위 조회 */
    CHECK(t_change(t, "UPDATE users SET age = 21 WHERE id = 2;", &err) == 1); /* 값이 같아도 센다(D06) */
    CHECK(t_change(t, "DELETE FROM users WHERE id = 3;", &err) == 1);
    CHECK(t_change(t, "DELETE FROM users WHERE id > 100;", &err) == 0);
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\tzz\t5\n2\tb\t21\n4\tr\t30\n5\tr\t40\n") == 0);

    /* 새 인덱스가 새 파일의 위치를 가리킨다 */
    memset(&c, 0, sizeof c);
    CHECK(table_find_by_id(t, 4, collect_row, &c, &err) == 0 && c.count == 1 && strcmp(c.names[0], "r") == 0 &&
          c.ages[0] == 30);
    c.count = 0;
    CHECK(table_find_by_id(t, 3, collect_row, &c, &err) == 0 && c.count == 0);
    c.count = 0;
    CHECK(table_find_id_range(t, 2, 5, collect_row, &c, &err) == 0 && c.count == 3 && c.ids[0] == 2 &&
          c.ids[1] == 4 && c.ids[2] == 5);
    CHECK(table_row_count(t) == 4);
    CHECK(table_test_validate_index(t, why, sizeof why) == 0);
    CHECK(!file_exists(dir, "users.tsv.tmp"));
    CHECK(!file_exists(dir, "users.next")); /* 가장 큰 id(5)는 지우지 않았다 */

    /* 이어지는 INSERT는 새 파일에 덧붙고 인덱스에 들어간다 */
    CHECK(table_append(t, "f", 50, &id, &err) == 0 && id == 6);
    c.count = 0;
    CHECK(table_find_by_id(t, 6, collect_row, &c, &err) == 0 && c.count == 1 && strcmp(c.names[0], "f") == 0);
    CHECK(table_close(t, &err) == 0);

    t = NULL;
    CHECK(table_open(dir, &t, &err) == 0);
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\tzz\t5\n2\tb\t21\n4\tr\t30\n5\tr\t40\n6\tf\t50\n") == 0);
    CHECK(t != NULL && table_last_id(t) == 6 && table_row_count(t) == 5);
    CHECK(table_close(t, &err) == 0);
}

/* D06: 조건에 맞는 행이 없으면 0이고 파일을 다시 쓰지 않는다.
   다시 쓰면 실패하도록 걸어 두고 실행해서, 다시 쓰기가 일어나지 않았음을 확인한다 */
static void test_p4_zero_rows_untouched(void) {
    const char *dir = fresh_dir("p4_zero");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char before[256], after[256];
    int64_t id;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    CHECK(t_change(t, "DELETE FROM users WHERE id = 1;", &err) == 0); /* 빈 테이블 */
    CHECK(table_append(t, "a", 1, &id, &err) == 0);
    CHECK(table_append(t, "b", 2, &id, &err) == 0);
    read_file(dir, before, sizeof before);

    table_test_fail_rewrite(REWRITE_FAIL_WRITE);
    CHECK(t_change(t, "UPDATE users SET age = 9 WHERE id = 99;", &err) == 0);
    CHECK(t_change(t, "DELETE FROM users WHERE age = 77;", &err) == 0);
    CHECK(t_change(t, "DELETE FROM users WHERE id BETWEEN 5 AND 3;", &err) == 0);
    CHECK(t_change(t, "UPDATE users SET age = 9 WHERE id > 2;", &err) == 0);
    table_test_fail_rewrite(REWRITE_FAIL_NONE);

    read_file(dir, after, sizeof after);
    CHECK(strcmp(before, after) == 0);
    CHECK(!file_exists(dir, "users.tsv.tmp"));
    CHECK(table_close(t, &err) == 0);
}

/* SET 값 검증은 INSERT와 같은 규칙이다. 실패하면 아무것도 바뀌지 않는다 */
static void test_p4_update_validation(void) {
    const char *dir = fresh_dir("p4_validate");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char big[300];
    char tabbed[] = "a\tb";
    char empty[] = "";
    char okname[] = "ok";
    char buf[256];
    int64_t id, n = -1;
    ChangeSeen c = {0, STMT_SELECT, -1};
    ResultSink sink = {NULL, NULL, &c, NULL, seen_change};
    Statement up = {STMT_UPDATE, NULL, 151, WHERE_ID, 1, 0, CMP_EQ, 0, SET_COL_AGE};
    RowUpdate bad = {1, "x\ny", 0, 0};

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    CHECK(table_append(t, "a", 1, &id, &err) == 0);

    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL && strstr(err.msg, "age") != NULL);
    up.age = -1;
    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL);
    memset(big, 'x', 256);
    big[256] = '\0';
    up.set_cols = SET_COL_NAME;
    up.age = 0;
    up.name = big;
    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL && strstr(err.msg, "255") != NULL);
    up.name = tabbed;
    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL);
    up.name = empty;
    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL);
    up.set_cols = SET_COL_NAME | SET_COL_AGE;
    up.name = okname;
    up.age = 200;
    CHECK(executor_run(t, &up, &sink, &err) == -1 && err.kind == DBE_SQL); /* 둘 중 하나라도 틀리면 거부 */
    CHECK(c.calls == 0);
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\ta\t1\n") == 0);

    /* SET에 없는 컬럼은 검사하지 않는다: age 칸에 범위 밖 값이 있어도 name만 바꾼다 */
    up.set_cols = SET_COL_NAME;
    CHECK(executor_run(t, &up, &sink, &err) == 0 && c.calls == 1 && c.count == 1);
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\tok\t1\n") == 0);

    /* Storage도 파일 형식을 깨는 값을 따로 막는다 */
    err.kind = DBE_NONE;
    CHECK(table_rewrite(t, NULL, NULL, &bad, &n, &err) == -1 && err.kind == DBE_STORAGE && n == 0);
    CHECK(!file_exists(dir, "users.tsv.tmp"));
    CHECK(table_close(t, &err) == 0);
}

/* D3: 가장 큰 id를 지워도 다음 id는 줄지 않는다. 재시작해도 같다(users.next, DB-P4-D04) */
static void test_p4_next_id(void) {
    /* 길이를 함께 적는다: 내장 NUL("1\0" + "00\n")은 C 문자열로 표현할 수 없다 (Codex 재현 31 00 30 30 0a) */
    static const struct {
        const char *bytes;
        size_t len;
    } bad_next[] = {
        {"abc\n", 4}, {"3", 1}, {"0\n", 2}, {"", 0}, {"03\n", 3}, {"-1\n", 3}, {"3\n4\n", 4},
        {"99999999999999999999\n", 21}, {"1\0" "00\n", 5}, {"1\0\n", 3}, {"\0" "1\n", 3},
        {"1 \n", 3}, {"+1\n", 3}, {"1\r\n", 3},
    };
    const char *dir = fresh_dir("p4_next");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char buf[64];
    int64_t id = 0;
    size_t i;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    CHECK(table_append(t, "a", 1, &id, &err) == 0);
    CHECK(table_append(t, "b", 2, &id, &err) == 0);
    CHECK(table_append(t, "c", 3, &id, &err) == 0);
    CHECK(t_change(t, "DELETE FROM users WHERE id = 1;", &err) == 1);
    CHECK(!file_exists(dir, "users.next")); /* 가장 큰 id가 아니면 쓰지 않는다 */
    CHECK(t_change(t, "DELETE FROM users WHERE id = 3;", &err) == 1);
    read_named(dir, "users.next", buf, sizeof buf);
    CHECK(strcmp(buf, "3\n") == 0);
    CHECK(!file_exists(dir, "users.next.tmp"));
    CHECK(table_append(t, "d", 4, &id, &err) == 0 && id == 4);
    CHECK(table_close(t, &err) == 0);

    t = NULL;
    CHECK(table_open(dir, &t, &err) == 0 && t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(table_last_id(t) == 4);
    CHECK(t_change(t, "DELETE FROM users WHERE id >= 2;", &err) == 2); /* 전부 지움 */
    read_named(dir, "users.next", buf, sizeof buf);
    CHECK(strcmp(buf, "4\n") == 0);
    read_file(dir, buf, sizeof buf);
    CHECK(buf[0] == '\0');
    CHECK(table_close(t, &err) == 0);

    t = NULL;
    CHECK(table_open(dir, &t, &err) == 0 && t != NULL);
    if (t == NULL) {
        return;
    }
    CHECK(table_last_id(t) == 4 && table_row_count(t) == 0);
    CHECK(table_append(t, "e", 5, &id, &err) == 0 && id == 5);
    CHECK(table_close(t, &err) == 0);

    /* users.next가 파일의 최대 id보다 작으면 파일 쪽이 이긴다 */
    dir = fresh_dir("p4_next_small");
    write_file(dir, "1\ta\t1\n7\tb\t2\n", 12);
    write_named(dir, "users.next", "3\n");
    t = NULL;
    CHECK(table_open(dir, &t, &err) == 0 && t != NULL && table_last_id(t) == 7);
    CHECK(table_close(t, &err) == 0);

    /* 손상된 users.next는 시작을 거부한다(조용히 0으로 보면 지운 id를 다시 줄 수 있다) */
    for (i = 0; i < sizeof bad_next / sizeof bad_next[0]; i++) {
        char after[64];
        size_t n;

        dir = fresh_dir("p4_next_bad");
        write_file(dir, "1\ta\t1\n", 6);
        write_named_n(dir, "users.next", bad_next[i].bytes, bad_next[i].len);
        t = NULL;
        err.kind = DBE_NONE;
        CHECK(table_open(dir, &t, &err) == -1 && t == NULL && err.kind == DBE_STORAGE &&
              strstr(err.msg, "다음 id 파일 손상") != NULL);
        if (t != NULL) {
            fprintf(stderr, "    손상 %u번이 받아들여짐 (다음 id %" PRId64 ")\n", (unsigned)i, table_last_id(t));
            table_close(t, &err);
        }
        /* 거부할 때 두 파일 모두 바꾸지 않는다 */
        n = read_named_n(dir, "users.next", after, sizeof after);
        CHECK(n == bad_next[i].len && memcmp(after, bad_next[i].bytes, n) == 0);
        read_file(dir, buf, sizeof buf);
        CHECK(strcmp(buf, "1\ta\t1\n") == 0);
    }
}

/* D4: 다시 쓰기의 어느 단계에서 실패해도 원래 파일·조회 결과가 그대로이고 임시 파일이 남지 않는다 */
static void test_p4_rewrite_failure_keeps_original(void) {
    static const int stages[] = {REWRITE_FAIL_WRITE, REWRITE_FAIL_NEXT, REWRITE_FAIL_RENAME};
    size_t i;

    for (i = 0; i < sizeof stages / sizeof stages[0]; i++) {
        const char *dir = fresh_dir("p4_fail");
        DbError err = {{0}, DBE_NONE};
        Db *db = NULL;
        char before[256], after[256];
        char why[200] = "";
        int64_t id = 0;

        CHECK(db_open(dir, &db, &err) == 0);
        if (db == NULL) {
            return;
        }
        CHECK(db_insert(db, "a", 1, &id, NULL, &err) == 0);
        CHECK(db_insert(db, "b", 2, &id, NULL, &err) == 0);
        CHECK(db_insert(db, "c", 3, &id, NULL, &err) == 0);
        read_file(dir, before, sizeof before);

        table_test_fail_rewrite(stages[i]);
        err.kind = DBE_NONE;
        CHECK(d_change(db, "DELETE FROM users WHERE id = 3;", &err) == -2); /* 가장 큰 id: users.next 단계까지 간다 */
        CHECK(err.kind == DBE_STORAGE);
        if (err.kind != DBE_STORAGE) {
            fprintf(stderr, "    단계 %d: %s\n", stages[i], err.msg);
        }
        CHECK(db_state(db) == DB_OK); /* 원래 상태가 남으므로 중단하지 않는다 */
        read_file(dir, after, sizeof after);
        CHECK(strcmp(before, after) == 0);
        CHECK(!file_exists(dir, "users.tsv.tmp"));
        CHECK(!file_exists(dir, "users.next.tmp"));
        if (stages[i] != REWRITE_FAIL_RENAME) {
            CHECK(!file_exists(dir, "users.next")); /* 바꿔치기 실패는 users.next 뒤라 남아도 된다 */
        }
        CHECK(db_count(db, WHERE_NONE, 0, &err) == 3);
        CHECK(db_count(db, WHERE_ID, 3, &err) == 1);
        CHECK(db_test_validate(db, why, sizeof why) == 0);

        /* UPDATE도 같다 */
        table_test_fail_rewrite(stages[i] == REWRITE_FAIL_NEXT ? REWRITE_FAIL_WRITE : stages[i]);
        CHECK(d_change(db, "UPDATE users SET name = 'zz' WHERE id = 1;", &err) == -2 && err.kind == DBE_STORAGE);
        read_file(dir, after, sizeof after);
        CHECK(strcmp(before, after) == 0);
        CHECK(!file_exists(dir, "users.tsv.tmp"));

        /* 같은 문장을 다시 실행하면 성공하고, 다음 id는 지운 id 다음이다 */
        CHECK(d_change(db, "DELETE FROM users WHERE id = 3;", &err) == 1);
        CHECK(db_count(db, WHERE_NONE, 0, &err) == 2);
        CHECK(db_insert(db, "d", 4, &id, NULL, &err) == 0 && id == 4);
        CHECK(db_close(db, &err) == 0);
    }
    table_test_fail_rewrite(REWRITE_FAIL_NONE);
}

#ifdef _WIN32
/* Windows: 바꿔치기 뒤 다시 열기에 실패하면 엔진을 중단 상태로 둔다. 파일은 온전하고 재시작하면 복원된다 */
static void test_p4_windows_reopen_failure_halts(void) {
    const char *dir = fresh_dir("p4_reopen");
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    char buf[128];
    int64_t id = 0;

    CHECK(db_open(dir, &db, &err) == 0);
    if (db == NULL) {
        return;
    }
    CHECK(db_insert(db, "a", 1, &id, NULL, &err) == 0);
    CHECK(db_insert(db, "b", 2, &id, NULL, &err) == 0);
    table_test_fail_rewrite(REWRITE_FAIL_REOPEN);
    CHECK(d_change(db, "DELETE FROM users WHERE id = 1;", &err) == -2 && err.kind == DBE_REOPEN_FAILED);
    CHECK(db_state(db) == DB_HALTED_REOPEN);
    err.kind = DBE_NONE;
    CHECK(db_count(db, WHERE_NONE, 0, &err) == -1 && err.kind == DBE_HALTED);
    CHECK(db_close(db, &err) == 0);
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "2\tb\t2\n") == 0);
    db = NULL;
    CHECK(db_open(dir, &db, &err) == 0 && db != NULL);
    CHECK(db_count(db, WHERE_NONE, 0, &err) == 1);
    CHECK(db_close(db, &err) == 0);
}
#endif

/* 이전 실행이 다시 쓰기 도중 끝나 남긴 임시 파일은 시작할 때 지운다 */
static void test_p4_startup_removes_tmp(void) {
    const char *dir = fresh_dir("p4_tmp_cleanup");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    char buf[64];

    write_file(dir, "1\ta\t1\n", 6);
    write_named(dir, "users.tsv.tmp", "1\tpartial");
    write_named(dir, "users.next.tmp", "9\n");
    CHECK(table_open(dir, &t, &err) == 0 && t != NULL);
    CHECK(!file_exists(dir, "users.tsv.tmp") && !file_exists(dir, "users.next.tmp"));
    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\ta\t1\n") == 0);
    CHECK(t != NULL && table_last_id(t) == 1); /* 남은 users.next.tmp는 반영하지 않는다 */
    CHECK(table_close(t, &err) == 0);
}

/* 무작위 UPDATE·DELETE를 모델(배열)과 비교한다: 행 수, 파일 전체, 인덱스 조회, 트리 불변식, 재시작 */
#define PM_ROWS 200
static void test_p4_model_random(void) {
    static struct {
        int alive;
        char name[16];
        int age;
    } m[PM_ROWS + 1];
    static Dump got, want;
    const char *dir = fresh_dir("p4_model");
    Table *t = NULL;
    DbError err = {{0}, DBE_NONE};
    uint32_t seed = 12345u;
    char why[200] = "";
    int64_t id;
    int i, op;

    CHECK(table_open(dir, &t, &err) == 0);
    if (t == NULL) {
        return;
    }
    for (i = 1; i <= PM_ROWS; i++) {
        m[i].alive = 1;
        snprintf(m[i].name, sizeof m[i].name, "n%d", i);
        m[i].age = (i * 7) % 60;
        CHECK(table_append(t, m[i].name, m[i].age, &id, &err) == 0);
    }
    for (op = 0; op < 60; op++) {
        uint32_t k = ct_rand(&seed) % 5;
        int a = 1 + (int)(ct_rand(&seed) % PM_ROWS);
        int b = a + (int)(ct_rand(&seed) % 30);
        int v = (int)(ct_rand(&seed) % 60);
        int64_t expect = 0, n;
        int alive = 0;
        char sql[128], newname[16];

        snprintf(newname, sizeof newname, "u%d", op);
        switch (k) {
        case 0: snprintf(sql, sizeof sql, "DELETE FROM users WHERE age = %d;", v); break;
        case 1: snprintf(sql, sizeof sql, "DELETE FROM users WHERE id BETWEEN %d AND %d;", a, b); break;
        case 2: snprintf(sql, sizeof sql, "UPDATE users SET age = %d WHERE id = %d;", v, a); break;
        case 3: snprintf(sql, sizeof sql, "UPDATE users SET name = '%s', age = %d WHERE id >= %d;", newname, v, b); break;
        default: snprintf(sql, sizeof sql, "DELETE FROM users WHERE id = %d;", a); break;
        }
        for (i = 1; i <= PM_ROWS; i++) {
            int hit;
            if (!m[i].alive) {
                continue;
            }
            switch (k) {
            case 0: hit = m[i].age == v; break;
            case 1: hit = i >= a && i <= b; break;
            case 2: hit = i == a; break;
            case 3: hit = i >= b; break;
            default: hit = i == a; break;
            }
            if (!hit) {
                continue;
            }
            expect++;
            if (k == 2 || k == 3) {
                m[i].age = v;
                if (k == 3) {
                    snprintf(m[i].name, sizeof m[i].name, "%s", newname);
                }
            } else {
                m[i].alive = 0;
            }
        }
        n = t_change(t, sql, &err);
        CHECK(n == expect);
        if (n != expect) {
            fprintf(stderr, "    %s → %" PRId64 " (기대 %" PRId64 ") %s\n", sql, n, expect, err.msg);
        }
        want.len = 0;
        want.buf[0] = '\0';
        for (i = 1; i <= PM_ROWS; i++) {
            if (m[i].alive) {
                Row r;
                r.id = i;
                r.name = m[i].name;
                r.age = m[i].age;
                dump_row(&r, &want);
                alive++;
            }
        }
        got.len = 0;
        got.buf[0] = '\0';
        CHECK(table_scan(t, dump_row, &got, &err) == 0);
        CHECK(got.len == want.len && strcmp(got.buf, want.buf) == 0);
        CHECK(table_row_count(t) == alive);
        CHECK(table_test_validate_index(t, why, sizeof why) == 0);
        for (i = a - 1; i <= a + 1; i++) {
            int cnt = 0;
            if (i < 1 || i > PM_ROWS) {
                continue;
            }
            CHECK(table_find_by_id(t, i, count_row, &cnt, &err) == 0 && cnt == m[i].alive);
        }
    }
    CHECK(table_last_id(t) == PM_ROWS);
    CHECK(table_close(t, &err) == 0);

    t = NULL;
    CHECK(table_open(dir, &t, &err) == 0 && t != NULL);
    if (t == NULL) {
        return;
    }
    got.len = 0;
    got.buf[0] = '\0';
    CHECK(table_scan(t, dump_row, &got, &err) == 0 && strcmp(got.buf, want.buf) == 0);
    CHECK(table_append(t, "z", 1, &id, &err) == 0 && id == PM_ROWS + 1);
    CHECK(table_close(t, &err) == 0);
}

/* D5: 여러 스레드가 INSERT·UPDATE·DELETE·SELECT를 섞는다. 각 스레드는 자기가 넣은 행만 고치고 지우므로
   결과를 정확히 예상할 수 있다. 끝나면 각 스레드의 기록과 실제 내용, 트리 불변식, 재시작 뒤 내용을 비교한다 */
#define MX_THREADS 4
#define MX_OPS 150

typedef struct {
    Db *db;
    int tid;
    uint32_t seed;
    int n;
    int64_t ids[MX_OPS];
    int ages[MX_OPS];
    int alive[MX_OPS];
    char names[MX_OPS][24];
    int errors;
    char first_error[256];
    int bad;
} MxState;

typedef struct {
    int64_t prev;
    int broken;
    int64_t count;
} IncCheck;

static int inc_row(const Row *row, void *ctx) {
    IncCheck *c = ctx;
    if (row->id <= c->prev) {
        c->broken = 1;
    }
    c->prev = row->id;
    c->count++;
    return 0;
}

static void mx_error(MxState *s, const DbError *err) {
    if (s->errors++ == 0) {
        snprintf(s->first_error, sizeof s->first_error, "%s", err->msg);
    }
}

static void *mx_worker(void *arg) {
    MxState *s = arg;
    int op;

    for (op = 0; op < MX_OPS; op++) {
        uint32_t k = ct_rand(&s->seed) % 100;
        DbError err = {{0}, DBE_NONE};
        char sql[128];

        if (k < 35 || s->n == 0) {
            int64_t id = 0;
            int age = (int)(ct_rand(&s->seed) % 151);
            snprintf(s->names[s->n], sizeof s->names[0], "m%d_%d", s->tid, op);
            if (db_insert(s->db, s->names[s->n], age, &id, NULL, &err) != 0) {
                mx_error(s, &err);
                continue;
            }
            s->ids[s->n] = id;
            s->ages[s->n] = age;
            s->alive[s->n] = 1;
            s->n++;
        } else {
            int j = (int)(ct_rand(&s->seed) % (uint32_t)s->n);
            if (k < 55) {
                int age = (int)(ct_rand(&s->seed) % 151);
                int64_t got;
                snprintf(sql, sizeof sql, "UPDATE users SET age = %d WHERE id = %" PRId64 ";", age, s->ids[j]);
                got = d_change(s->db, sql, &err);
                if (got < 0) {
                    mx_error(s, &err);
                } else if (got != s->alive[j]) {
                    s->bad++;
                } else if (got == 1) {
                    s->ages[j] = age;
                }
            } else if (k < 70) {
                int64_t got;
                snprintf(sql, sizeof sql, "DELETE FROM users WHERE id = %" PRId64 ";", s->ids[j]);
                got = d_change(s->db, sql, &err);
                if (got < 0) {
                    mx_error(s, &err);
                } else if (got != s->alive[j]) {
                    s->bad++;
                } else {
                    s->alive[j] = 0;
                }
            } else if (k < 90) {
                Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, s->ids[j], 0, CMP_EQ, 0, 0};
                Collected c;
                ResultSink rs = {NULL, collect_row, &c, NULL, NULL};
                c.count = 0;
                c.stop_after = 0;
                if (db_exec(s->db, &sel, &rs, NULL, &err) != 0) {
                    mx_error(s, &err);
                } else if (c.count != s->alive[j] ||
                           (c.count == 1 && (c.ages[0] != s->ages[j] || strcmp(c.names[0], s->names[j]) != 0))) {
                    s->bad++;
                }
            } else {
                IncCheck ic = {0, 0, 0};
                Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0, 0, CMP_EQ, 0, 0};
                ResultSink rs = {NULL, inc_row, &ic, NULL, NULL};
                if (db_exec(s->db, &sel, &rs, NULL, &err) != 0) {
                    mx_error(s, &err);
                } else if (ic.broken) {
                    s->bad++;
                }
            }
        }
    }
    return NULL;
}

static void test_p4_concurrent_mixed(void) {
    const char *dir = fresh_dir("p4_concurrent");
    static MxState st[MX_THREADS];
    static unsigned char seen[MX_THREADS * MX_OPS + 1];
    pthread_t th[MX_THREADS];
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int64_t inserted = 0, alive = 0, dup = 0, out_of_range = 0, id = 0;
    char why[200] = "";
    int i, j, pass;

    CHECK(db_open(dir, &db, &err) == 0);
    if (db == NULL) {
        return;
    }
    for (i = 0; i < MX_THREADS; i++) {
        memset(&st[i], 0, sizeof st[i]);
        st[i].db = db;
        st[i].tid = i;
        st[i].seed = 0x85EBCA6Bu * (uint32_t)(i + 1);
        CHECK(pthread_create(&th[i], NULL, mx_worker, &st[i]) == 0);
    }
    for (i = 0; i < MX_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    memset(seen, 0, sizeof seen);
    for (i = 0; i < MX_THREADS; i++) {
        if (st[i].errors != 0) {
            fprintf(stderr, "    스레드 %d 첫 오류: %s\n", i, st[i].first_error);
        }
        CHECK(st[i].errors == 0);
        CHECK(st[i].bad == 0);
        inserted += st[i].n;
    }
    /* 받은 id는 1..inserted가 정확히 한 번씩: 지운 id를 다시 주지 않는다 */
    for (i = 0; i < MX_THREADS; i++) {
        for (j = 0; j < st[i].n; j++) {
            int64_t x = st[i].ids[j];
            if (x < 1 || x > inserted) {
                out_of_range++;
            } else if (seen[x]++) {
                dup++;
            }
            alive += st[i].alive[j];
        }
    }
    CHECK(inserted > 0 && dup == 0 && out_of_range == 0);

    /* 실행 중 상태와 재시작 뒤 상태가 모두 각 스레드의 기록과 같다 */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < MX_THREADS; i++) {
            for (j = 0; j < st[i].n; j++) {
                Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, st[i].ids[j], 0, CMP_EQ, 0, 0};
                Collected c;
                ResultSink rs = {NULL, collect_row, &c, NULL, NULL};
                c.count = 0;
                c.stop_after = 0;
                CHECK(db_exec(db, &sel, &rs, NULL, &err) == 0);
                CHECK(c.count == st[i].alive[j]);
                if (c.count == 1) {
                    CHECK(c.ages[0] == st[i].ages[j] && strcmp(c.names[0], st[i].names[j]) == 0);
                }
            }
        }
        CHECK(db_count(db, WHERE_NONE, 0, &err) == alive);
        CHECK(db_test_validate(db, why, sizeof why) == 0);
        CHECK(db_state(db) == DB_OK);
        CHECK(db_close(db, &err) == 0);
        db = NULL;
        CHECK(db_open(dir, &db, &err) == 0);
        if (db == NULL) {
            return;
        }
    }
    CHECK(db_insert(db, "next", 1, &id, NULL, &err) == 0 && id == inserted + 1);
    CHECK(db_close(db, &err) == 0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "사용법: test_unit <임시 디렉터리>\n");
        return 2;
    }
    g_base = argv[1];
    make_dir(g_base);

    test_parse_insert_basic();
    test_parse_column_order_and_case();
    test_parse_string_special_and_multiline();
    test_parse_select_and_sequence();
    test_parse_empty_input();
    test_parse_comment_only();
    test_parse_comments_around_and_inside();
    test_parse_comment_symbols_in_string();
    test_parse_comment_errors();
    test_parse_where();
    test_parse_where_errors();
    test_parse_next_statement_error_isolated();
    test_parse_errors();
    test_parse_update_delete();
    test_parse_explain();
    test_parse_explain_errors();
    test_parse_id_range();
    test_parse_id_range_errors();

    test_storage_empty_then_append();
    test_storage_persist_and_restore_id();
    test_storage_corrupt_files();
    test_storage_max_id();
    test_storage_rejects_bad_record();
    test_storage_callback_stop();
    test_storage_open_failure();

    test_executor_validation();
    test_executor_insert_select();
    test_executor_where();
    test_executor_id_uses_index_age_scans();
    test_executor_plan_paths_and_stats();
    test_executor_explain_does_not_execute();
    test_executor_id_range();
    test_executor_range_matches_scan();
    test_executor_range_reads_only_inside();

    test_bpt_create_order_bounds();
    test_bpt_empty();
    test_bpt_split_shapes();
    test_bpt_sequential_many_orders();
    test_bpt_random_order_and_duplicates();
    test_bpt_height_is_logarithmic();
    test_bpt_nomem_leaves_tree_unchanged();
    test_bpt_range();

    test_storage_index_lookup();
    test_storage_index_rebuilt_on_open();
    test_storage_index_many_records();
    test_bpt_mem_stats();
    test_storage_index_stats();

    test_db_exec_basic_and_kinds();
    test_db_halt_after_index_failure();
    test_db_halt_after_write_failure();
    test_db_concurrent_correctness();

    test_json_escape_and_utf8();
    test_json_budget();

    test_storage_parallel_reads_without_lock();
    test_storage_scan_stops_at_file_size();
    test_db_readers_run_together();
    test_db_writer_waits_for_reader();
#ifdef __GLIBC__
    test_db_writer_not_starved();
#endif

    test_workqueue_basic();
    test_workqueue_threads();

    test_p4_update_delete_basic();
    test_p4_zero_rows_untouched();
    test_p4_update_validation();
    test_p4_next_id();
    test_p4_rewrite_failure_keeps_original();
#ifdef _WIN32
    test_p4_windows_reopen_failure_halts();
#endif
    test_p4_startup_removes_tmp();
    test_p4_model_random();
    test_p4_concurrent_mixed();

    printf("단위 테스트: %d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
