/*
 * 단위 테스트: Parser, Storage, Executor를 함수 단위로 검증한다.
 * 사용법: test_unit <임시 디렉터리>
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bptree.h"
#include "executor.h"
#include "parser.h"
#include "storage.h"

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

    snprintf(dir, sizeof dir, "%s/%s", g_base, name);
    snprintf(file, sizeof file, "%s/users.tsv", dir);
    remove(file);
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
        DbError e_ = {{0}};                                                               \
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
    DbError err = {{0}};
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
    DbError err = {{0}};

    CHECK(parse_one("insert into USERS (Age, NAME) values (30, 'O''Brien');", &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    CHECK(st.name != NULL && strcmp(st.name, "O'Brien") == 0);
    CHECK(st.age == 30);
    statement_free(&st);
}

static void test_parse_string_special_and_multiline(void) {
    Statement st;
    DbError err = {{0}};

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
    DbError err = {{0}};
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
    DbError err = {{0}};

    CHECK(parse_one("", &st, &err) == 0);
    CHECK(parse_one(" \n\t\r\n ", &st, &err) == 0);
}

static void test_parse_comment_only(void) {
    Statement st;
    DbError err = {{0}};

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
    DbError err = {{0}};
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
    DbError err = {{0}};

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
    EXPECT_PARSE_ERROR("/* a /* b */ */ SELECT * FROM users;", "INSERT 또는 SELECT");
    EXPECT_PARSE_ERROR("SELECT * FROM users -- 세미콜론이 주석 안;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users / 1;", "알 수 없는 문자 '/'");
}

static void expect_where(const char *sql, WhereKind kind, int64_t value) {
    Statement st;
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    EXPECT_PARSE_ERROR(";", "INSERT 또는 SELECT");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 1;", "INSERT 또는 SELECT");
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

/* ---------- Storage ---------- */

static void test_storage_empty_then_append(void) {
    const char *dir = fresh_dir("empty");
    Table *t = NULL;
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    DbError err = {{0}};

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
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    DbError err = {{0}};

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
    Statement st = {STMT_INSERT, NULL, 0, WHERE_NONE, 0};
    DbError err = {{0}};

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
    DbError err = {{0}};
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
        Statement st = {STMT_INSERT, NULL, 0, WHERE_NONE, 0};
        int64_t last = 0;
        ResultSink sink = {count_insert, NULL, &last};

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
    DbError err = {{0}};
    Collected c;
    int64_t last = 0;
    char kim[] = "kim";
    char lee[] = "lee";
    Statement ins = {STMT_INSERT, kim, 20, WHERE_NONE, 0};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0};
    ResultSink sink;

    memset(&c, 0, sizeof c);
    sink.on_insert = count_insert;
    sink.on_row = collect_row;
    sink.ctx = &last;

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
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0};
    ResultSink sink = {NULL, collect_row, NULL};
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    DbError err = {{0}};
    Statement by_id = {STMT_SELECT, NULL, 0, WHERE_ID, 1};
    Statement by_id2 = {STMT_SELECT, NULL, 0, WHERE_ID, 2};
    Statement by_age = {STMT_SELECT, NULL, 0, WHERE_AGE, 25};
    Collected c;
    ResultSink sink = {NULL, collect_row, NULL};

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

/* ---------- Storage: id 인덱스 ---------- */

static void test_storage_index_lookup(void) {
    const char *dir = fresh_dir("index_lookup");
    Table *t = NULL;
    DbError err = {{0}};
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
    DbError err = {{0}};
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
    DbError err = {{0}};
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

    test_bpt_create_order_bounds();
    test_bpt_empty();
    test_bpt_split_shapes();
    test_bpt_sequential_many_orders();
    test_bpt_random_order_and_duplicates();
    test_bpt_height_is_logarithmic();
    test_bpt_nomem_leaves_tree_unchanged();

    test_storage_index_lookup();
    test_storage_index_rebuilt_on_open();
    test_storage_index_many_records();

    printf("단위 테스트: %d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
