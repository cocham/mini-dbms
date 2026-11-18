/*
 * CLI: 인자와 SQL 파일을 읽고, 문장을 하나씩 Parser → DB 진입점(db_exec → Executor)으로 넘기며
 * 결과(stdout)·오류(stderr)·종료 코드를 책임진다.
 *
 * 종료 코드: 0 성공, 1 SQL·데이터·파일 오류, 2 잘못된 사용법
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* clock_gettime(CLOCK_MONOTONIC) */
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "executor.h"
#include "parser.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#define EXIT_FAIL 1
#define EXIT_USAGE 2
#define DEFAULT_DATA_DIR "data"

#ifdef _WIN32
static UINT g_old_console_cp;
static void restore_console(void) { SetConsoleOutputCP(g_old_console_cp); }

/* 출력 바이트를 그대로 내보내고(\n → \r\n 변환 없음), 콘솔에서 한글이 보이게 한다. */
static void setup_windows_output(void) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    g_old_console_cp = GetConsoleOutputCP();
    if (g_old_console_cp != 0 && SetConsoleOutputCP(CP_UTF8)) {
        atexit(restore_console);
    }
}
#endif

static void print_usage(FILE *out) {
    fprintf(out, "사용법: minidb [--data-dir 디렉터리] [--timing] SQL파일\n"
                 "  --data-dir  데이터 파일(users.tsv) 위치. 기본값: " DEFAULT_DATA_DIR "\n"
                 "  --timing    시작 비용과 문장별 실행 시간을 stderr에 출력 (stdout은 그대로)\n");
}

/* ---------- 실행 측정 (--timing, P2-04) ---------- */

/*
 * 실제 실행 측정값. EXPLAIN의 PlanStats(계획 시점 통계)와 섞지 않는 별도 타입이다.
 * 시간은 단조 증가 시계로 잰다(시스템 시각 변경에 영향받지 않음).
 */
typedef struct {
    double elapsed_ms;     /* executor_run 한 번에 걸린 시간 (결과 출력 포함) */
    int64_t rows_returned; /* SELECT가 출력한 행 수 */
} ExecStats;

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/* 측정 줄은 stderr로. 앞선 stdout 결과가 먼저 보이도록 flush한다(측정 구간 밖에서). */
static void print_timing_start(double ms, Db *db) {
    IndexStats ix;

    db_stats(db, &ix);
    fflush(stdout);
    fprintf(stderr, "[측정] 시작: %.3f ms (파일 전체 읽기 + 인덱스 구축, %" PRId64 "행)\n", ms, ix.keys);
    fprintf(stderr, "[측정] 인덱스: 높이 %d, 리프 %zu개, 내부 노드 %zu개, 약 %.1f MB\n", ix.height,
            ix.leaves, ix.internals, (double)ix.bytes / (1024.0 * 1024.0));
}

static void print_timing_stmt(long stmt_no, const Statement *st, const ExecStats *es) {
    fflush(stdout);
    if (st->kind == STMT_INSERT) {
        fprintf(stderr, "[측정] 문장 %ld: %.4f ms, 추가 1행\n", stmt_no, es->elapsed_ms);
    } else if (st->kind == STMT_UPDATE || st->kind == STMT_DELETE) {
        fprintf(stderr, "[측정] 문장 %ld: %.4f ms, %s %" PRId64 "행\n", stmt_no, es->elapsed_ms,
                st->kind == STMT_UPDATE ? "수정" : "삭제", es->rows_returned);
    } else if (st->explain) {
        fprintf(stderr, "[측정] 문장 %ld: %.4f ms, 계획만\n", stmt_no, es->elapsed_ms);
    } else {
        fprintf(stderr, "[측정] 문장 %ld: %.4f ms, 결과 %" PRId64 "행\n", stmt_no, es->elapsed_ms,
                es->rows_returned);
    }
}

/* 파일 전체를 NUL로 끝나는 버퍼로 읽는다. 호출자가 free한다. */
static char *read_sql_file(const char *path, size_t *out_len, DbError *err) {
    FILE *fp = fopen(path, "rb");
    size_t cap = 4096;
    size_t len = 0;
    char *buf;

    if (fp == NULL) {
        db_error_set(err, "SQL 파일을 열 수 없습니다: %s", path);
        return NULL;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(fp);
        db_error_set(err, "메모리가 부족합니다");
        return NULL;
    }
    for (;;) {
        size_t n;
        if (len + 1 >= cap) {
            char *grown;
            if (cap > SIZE_MAX / 2) {
                break;
            }
            grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                fclose(fp);
                db_error_set(err, "메모리가 부족합니다");
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
        n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) {
            break;
        }
    }
    if (ferror(fp) || !feof(fp)) {
        free(buf);
        fclose(fp);
        db_error_set(err, "SQL 파일을 읽을 수 없습니다: %s", path);
        return NULL;
    }
    fclose(fp);
    buf[len] = '\0';
    if (memchr(buf, '\0', len) != NULL) {
        free(buf);
        db_error_set(err, "SQL 파일에 NUL 바이트가 있습니다: %s", path);
        return NULL;
    }
    *out_len = len;
    return buf;
}

static int print_insert(int64_t id, void *ctx) {
    (void)id;
    (void)ctx;
    return printf("INSERT 1\n") < 0 ? -1 : 0;
}

/* UPDATE n / DELETE n: 조건에 맞은 행 수 (DB-P4-D06) */
static int print_change(StmtKind kind, int64_t count, void *ctx) {
    ExecStats *es = ctx;
    es->rows_returned = count;
    return printf("%s %" PRId64 "\n", kind == STMT_UPDATE ? "UPDATE" : "DELETE", count) < 0 ? -1 : 0;
}

static int print_row(const Row *row, void *ctx) {
    ExecStats *es = ctx;
    es->rows_returned++;
    return printf("%" PRId64 "\t%s\t%d\n", row->id, row->name, row->age) < 0 ? -1 : 0;
}

/*
 * EXPLAIN 한 줄. 괄호 안의 숫자는 계획 시점 통계(트리 높이, 전체 행 수)이며 실행 측정값이 아니다.
 *   INDEX LOOKUP (id = 3, B+ 트리 높이 2)
 *   FULL SCAN (age = 25, 전체 행 수 5)
 *   FULL SCAN (조건 없음, 전체 행 수 5)
 *   INDEX RANGE SCAN (id BETWEEN 10 AND 20, B+ 트리 높이 2)    조건은 사용자가 쓴 형태 그대로
 */
static int print_plan(const QueryPlan *plan, void *ctx) {
    char cond[96];
    int n;

    (void)ctx;
    executor_plan_condition(plan, cond, sizeof cond);
    if (plan->kind == PLAN_FULL_SCAN) {
        n = printf("%s (%s, 전체 행 수 %" PRId64 ")\n", executor_plan_name(plan->kind), cond,
                   plan->stats.total_rows);
    } else {
        n = printf("%s (%s, B+ 트리 높이 %d)\n", executor_plan_name(plan->kind), cond, plan->stats.tree_height);
    }
    return n < 0 ? -1 : 0;
}

static void report(long stmt_no, const DbError *err) {
    fflush(stdout); /* 앞선 결과가 오류 메시지보다 먼저 보이게 */
    if (stmt_no > 0) {
        fprintf(stderr, "오류: 문장 %ld: %s\n", stmt_no, err->msg);
    } else {
        fprintf(stderr, "오류: %s\n", err->msg);
    }
}

int main(int argc, char **argv) {
    const char *data_dir = DEFAULT_DATA_DIR;
    const char *sql_path = NULL;
    DbError err;
    char *sql;
    size_t sql_len = 0;
    size_t pos = 0;
    Db *db = NULL;
    ExecStats es = {0.0, 0};
    ResultSink sink = {print_insert, print_row, NULL, print_plan, print_change};
    long stmt_no = 0;
    int status = 0;
    int timing = 0;
    double t0;
    int i;

#ifdef _WIN32
    setup_windows_output();
#endif
    memset(&err, 0, sizeof err);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data-dir") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "오류: --data-dir 뒤에 디렉터리가 필요합니다\n");
                print_usage(stderr);
                return EXIT_USAGE;
            }
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--timing") == 0) {
            timing = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "오류: 알 수 없는 옵션: %s\n", argv[i]);
            print_usage(stderr);
            return EXIT_USAGE;
        } else if (sql_path != NULL) {
            fprintf(stderr, "오류: SQL 파일은 하나만 지정할 수 있습니다\n");
            print_usage(stderr);
            return EXIT_USAGE;
        } else {
            sql_path = argv[i];
        }
    }
    if (sql_path == NULL) {
        print_usage(stderr);
        return EXIT_USAGE;
    }

    sql = read_sql_file(sql_path, &sql_len, &err);
    if (sql == NULL) {
        report(0, &err);
        return EXIT_FAIL;
    }
    if (sql_len >= 3 && memcmp(sql, "\xEF\xBB\xBF", 3) == 0) {
        pos = 3; /* UTF-8 BOM 건너뛰기 */
    }

    t0 = now_ms();
    if (db_open(data_dir, &db, &err) != 0) {
        report(0, &err);
        free(sql);
        return EXIT_FAIL;
    }
    if (timing) {
        print_timing_start(now_ms() - t0, db);
    }
    sink.ctx = &es;

    /* 문장을 순서대로 실행하고, 하나라도 실패하면 즉시 멈춘다(fail-fast).
     * 앞서 성공한 INSERT·UPDATE·DELETE는 이미 파일에 있으므로 유지된다. */
    for (;;) {
        Statement st;
        int r;
        int rc = 0;

        stmt_no++;
        r = parser_next(sql, &pos, &st, &err);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            report(stmt_no, &err);
            status = EXIT_FAIL;
            break;
        }
        /* EXPLAIN은 행을 출력하지 않으므로 헤더도 없다. */
        if (st.kind == STMT_SELECT && !st.explain && printf("id\tname\tage\n") < 0) {
            db_error_set(&err, "결과 출력에 실패했습니다");
            rc = -1;
        }
        if (rc == 0) {
            es.rows_returned = 0;
            t0 = now_ms();
            rc = db_exec(db, &st, &sink, NULL, &err);
            es.elapsed_ms = now_ms() - t0;
            if (rc == 0 && timing) {
                print_timing_stmt(stmt_no, &st, &es);
            }
        }
        statement_free(&st);
        if (rc != 0) {
            report(stmt_no, &err);
            status = EXIT_FAIL;
            break;
        }
    }

    if (db_close(db, &err) != 0 && status == 0) {
        report(0, &err);
        status = EXIT_FAIL;
    }
    free(sql);
    if (fflush(stdout) != 0 && status == 0) {
        fprintf(stderr, "오류: 결과 출력에 실패했습니다\n");
        status = EXIT_FAIL;
    }
    return status;
}
