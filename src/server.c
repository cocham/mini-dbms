/*
 * API 서버 (Phase 3, POSIX 전용). P3-03: accept 스레드 하나 + 작업 큐 + 워커 N개.
 *
 *   main(accept 스레드): poll → accept → wq_try_push
 *                        큐가 가득 차면 503 BUSY를 비차단으로 한 번 보내고 닫는다(기다리지 않음)
 *   워커: wq_pop → handle_connection → http_close   (워커마다 연결 하나씩, 동시에)
 *
 * 요청 하나의 흐름(워커 안):
 *   http_read_request(http.c) → 경로 확인
 *   → POST /query: 본문의 문장을 모두 parser_next로 파싱(잠금 밖)
 *                  → 문장마다 db_exec(db.c, 잠금 안) → 결과를 JSON 버퍼에(json.c)
 *   → http_send_response(잠금 밖) → http_close
 * 엔진은 아직 전역 잠금이라 문장 실행은 한 번에 하나다. 동시에 일어나는 것은 네트워크 입출력과 파싱이다.
 *
 * 종료(SIGINT·SIGTERM, spec "종료"):
 *   새 연결 받지 않음 → 큐에서 기다리던 연결에 503 SHUTTING_DOWN → 처리 중인 요청은 계속
 *   → 유예 시간(10초)이 지나면 남은 연결을 shutdown, 워커는 다음 문장 경계에서 멈춤
 *     (실행 중인 문장은 끊지 않으므로 10초는 종료 완료의 절대 상한이 아니다) → join → db_close
 *
 * 종료 코드: 0 정상 종료(SIGINT·SIGTERM), 1 시작 실패, 2 잘못된 사용법
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "executor.h"
#include "http.h"
#include "json.h"
#include "parser.h"
#include "workqueue.h"

#ifdef MINIDB_TESTING
#include "bptree.h"
#endif

#define EXIT_FAIL 1
#define EXIT_USAGE 2

/* 응답 예산 (spec "결과 크기 제한"). 오류 객체와 닫는 괄호를 쓸 자리는 예산에서 미리 뗀다. */
#define DEFAULT_MAX_ROWS 100000
#define DEFAULT_MAX_RESPONSE (8 * 1024 * 1024)
#define MIN_MAX_RESPONSE 4096
#define RESPONSE_RESERVE 2048
#define INSERT_RESULT_MAX 48 /* {"type":"insert","id":-9223372036854775808} + 쉼표 */
#define CHANGE_RESULT_MAX 48 /* {"type":"update","count":-9223372036854775808} + 쉼표 = 47 */

/* GET /users 커서 페이지 조회 (Phase 5, DB-P5-D05·D08) */
#define USERS_LIMIT_MAX 1000
#define USERS_LIMIT_DEFAULT 100
/* 한 행의 최대 JSON: "[" + id 19자리 + "," + 이름(따옴표 2 + 255바이트 × \u00XX 6바이트 = 1532)
   + "," + age 3자리 + "]" = 1558, 행 사이 쉼표 1을 모든 행에 붙여 셈 */
#define USERS_ROW_JSON_MAX 1559
/* 고정 부분 최대: {"ok":true,"columns":["id","name","age"],"rows":[ (49)
   + ],"has_more":false,"next_cursor":9223372036854775807} (53) */
#define USERS_HEAD "{\"ok\":true,\"columns\":[\"id\",\"name\",\"age\"],\"rows\":["
#define USERS_FIXED_JSON_MAX 102

/* 스레드 풀 (P3-03, DB-P3-D06) */
#define DEFAULT_THREADS 4
#define MAX_THREADS 256
#define DEFAULT_QUEUE 64
#define MAX_QUEUE 4096
#define SHUTDOWN_GRACE_MS 10000

typedef struct {
    Db *db;
    int64_t max_rows;
    size_t max_response;
    int64_t page_max;     /* GET /users의 limit 상한 (시작 때 계산, DB-P5-D08) */
    int64_t page_default; /* limit을 생략했을 때 = min(100, page_max) */
    PagePath age_path;    /* age 조건 페이지의 탐색 경로 (--age-path, DB-P6-D17) */
    int threads;
    size_t queue_cap;
    long grace_ms;
    long test_stmt_delay_ms; /* 테스트 빌드에서만 0이 아닐 수 있다 */

    WorkQueue queue;
    /* 워커마다 지금 처리 중인 연결(fd, 없으면 -1). 유예 시간이 지나면 main이 이 fd들을 shutdown한다.
       워커는 close하기 전에 칸을 -1로 비우므로, main이 이미 닫혀 번호가 재사용된 fd를 건드리지 않는다. */
    pthread_mutex_t act_mu;
    int *active;
    int live; /* 아직 끝나지 않은 워커 수 (act_mu) */
} Server;

static volatile sig_atomic_t g_stop;
static int g_wake[2] = {-1, -1};
/* 유예 시간이 지났다: 워커는 다음 문장 경계에서 멈춘다. main이 쓰고 워커가 읽는다. */
static atomic_int g_force;

static void on_signal(int sig) {
    int saved = errno;
    (void)sig;
    g_stop = 1;
    if (g_wake[1] >= 0) {
        (void)!write(g_wake[1], "x", 1); /* poll을 깨운다 */
    }
    errno = saved;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* ---------- 응답 보내기 ---------- */

static int send_body(int fd, int status, const char *extra, const char *a, size_t alen, const char *b,
                     size_t blen, const char *c, size_t clen) {
    const char *parts[3] = {a, b, c};
    size_t lens[3] = {alen, blen, clen};
    return http_send_response(fd, status, extra, parts, lens, 3);
}

/* 메모리가 없을 때도 보낼 수 있는 고정 본문 (500 INTERNAL, 결과 불명) */
static const char k_internal_body[] =
    "{\"ok\":false,\"error\":{\"code\":\"INTERNAL\",\"message\":\"응답을 만들 메모리가 부족합니다. "
    "이 요청에서 실행된 문장의 결과는 알 수 없습니다\"}}";

static void send_internal(int fd) {
    send_body(fd, 500, NULL, k_internal_body, sizeof k_internal_body - 1, "", 0, "", 0);
}

/* 문장과 상관없는 오류: {"ok":false,"error":{"code":..,"message":..}} (/query면 "results":[]도 붙인다) */
static void send_simple_error(int fd, int status, const char *code, const char *msg, int with_results,
                              const char *extra) {
    JsonBuf b;

    jb_init(&b, RESPONSE_RESERVE);
    if (jb_lit(&b, "{\"ok\":false,\"error\":{\"code\":\"") != 0 || jb_lit(&b, code) != 0 ||
        jb_lit(&b, "\",\"message\":") != 0 || jb_str(&b, msg) != 0 ||
        jb_lit(&b, with_results ? "},\"results\":[]}" : "}}") != 0) {
        jb_free(&b);
        send_internal(fd);
        return;
    }
    send_body(fd, status, extra, b.data, b.len, "", 0, "", 0);
    jb_free(&b);
}

/*
 * accept 스레드가 보내는 503 (BUSY, 큐에서 기다리던 연결의 SHUTTING_DOWN). 절대 기다리지 않는다.
 * 요청은 읽지 않는다. 다만 이미 도착해 있는 바이트는 비차단으로 버린다: 읽지 않은 데이터가 남은 채
 * 닫으면 운영체제가 연결 재설정(RST)을 보내 클라이언트가 503을 못 볼 수 있어서다. 그래도 재설정이
 * 보일 수 있다(최선 노력, spec "느린 클라이언트와 기한"). 응답은 200바이트 미만이라 send 한 번이면 된다.
 */
static void send_fast_503(int fd, const char *code, const char *msg) {
    char body[128], resp[320], sink[4096];
    int bn, rn, fl, i;

    bn = snprintf(body, sizeof body, "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, msg);
    rn = snprintf(resp, sizeof resp,
                  "HTTP/1.1 503 Service Unavailable\r\n"
                  "Content-Type: application/json; charset=utf-8\r\n"
                  "Content-Length: %d\r\n"
                  "Connection: close\r\n\r\n%s",
                  bn, body);
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0) {
        for (i = 0; i < 16 && recv(fd, sink, sizeof sink, 0) > 0; i++) {
        }
        (void)!send(fd, resp, (size_t)rn, 0);
        shutdown(fd, SHUT_WR);
    }
    close(fd);
}

static const char k_busy_msg[] = "서버가 바쁩니다";
static const char k_shutdown_msg[] = "서버 종료 중";

/* ---------- /query ---------- */

typedef struct {
    JsonBuf *res;
    int64_t rows_total; /* 요청 전체의 SELECT 행 합계 */
    int64_t max_rows;
    int over_rows;
    int first_row; /* 이번 SELECT의 첫 행이면 1 */
} QueryCtx;

static int json_on_insert(int64_t id, void *vctx) {
    QueryCtx *q = vctx;
    /* 자리는 실행 전에 jb_reserve로 확보했으므로 여기서는 실패하지 않는다 */
    return (jb_lit(q->res, "{\"type\":\"insert\",\"id\":") != 0 || jb_i64(q->res, id) != 0 ||
            jb_lit(q->res, "}") != 0)
               ? 1
               : 0;
}

/* {"type":"update","count":n} / {"type":"delete","count":n} (DB-P4-D06) */
static int json_on_change(StmtKind kind, int64_t count, void *vctx) {
    QueryCtx *q = vctx;
    const char *head = kind == STMT_UPDATE ? "{\"type\":\"update\",\"count\":" : "{\"type\":\"delete\",\"count\":";
    /* INSERT처럼 자리를 실행 전에 확보했으므로 여기서는 실패하지 않는다: 반영된 변경이 응답에서 빠지지 않게 */
    return (jb_lit(q->res, head) != 0 || jb_i64(q->res, count) != 0 || jb_lit(q->res, "}") != 0) ? 1 : 0;
}

/* 행 하나 [id,"name",age] (첫 행이 아니면 앞에 쉼표). /query의 select와 /users가 같이 쓴다. 0 성공 */
static int json_row(JsonBuf *b, const Row *row, int first) {
    return ((!first && jb_lit(b, ",") != 0) || jb_lit(b, "[") != 0 || jb_i64(b, row->id) != 0 ||
            jb_lit(b, ",") != 0 || jb_str(b, row->name) != 0 || jb_lit(b, ",") != 0 ||
            jb_i64(b, row->age) != 0 || jb_lit(b, "]") != 0)
               ? -1
               : 0;
}

static int json_on_row(const Row *row, void *vctx) {
    QueryCtx *q = vctx;

    if (++q->rows_total > q->max_rows) {
        q->over_rows = 1;
        return 1;
    }
    if (json_row(q->res, row, q->first_row) != 0) {
        return 1;
    }
    q->first_row = 0;
    return 0;
}

static int json_on_plan(const QueryPlan *plan, void *vctx) {
    QueryCtx *q = vctx;
    char cond[96];

    executor_plan_condition(plan, cond, sizeof cond);
    return (jb_lit(q->res, "{\"type\":\"explain\",\"plan\":") != 0 ||
            jb_str(q->res, executor_plan_name(plan->kind)) != 0 || jb_lit(q->res, ",\"condition\":") != 0 ||
            jb_str(q->res, cond) != 0 || jb_lit(q->res, ",\"tree_height\":") != 0 ||
            jb_i64(q->res, plan->stats.tree_height) != 0 || jb_lit(q->res, ",\"total_rows\":") != 0 ||
            jb_i64(q->res, plan->stats.total_rows) != 0 || jb_lit(q->res, "}") != 0)
               ? 1
               : 0;
}

typedef struct {
    int status;
    const char *code;
    long statement;   /* 0이면 넣지 않음 */
    char message[256];
    int64_t stored_id; /* 0이면 넣지 않음 */
    const char *reason; /* NULL이면 넣지 않음 */
} QueryError;

static const char *halt_reason_code(DbState s) {
    switch (s) {
    case DB_HALTED_INDEX: return "index_failed";
    case DB_HALTED_REOPEN: return "reopen_failed"; /* Windows 전용이라 서버(POSIX)에서는 나오지 않는다 */
    default: return "write_failed";
    }
}

/* 실행 단계 실패를 오류 코드로 (spec 오류 코드 표) */
static void classify(QueryError *qe, const DbError *err, const JsonBuf *res, const QueryCtx *q, Server *s,
                     int64_t stored, size_t max_response) {
    snprintf(qe->message, sizeof qe->message, "%s", err->msg);
    switch (err->kind) {
    case DBE_SINK:
        if (res->oom) {
            qe->status = 500;
            qe->code = "INTERNAL";
        } else if (q->over_rows) {
            qe->status = 400;
            qe->code = "RESULT_TOO_LARGE";
            snprintf(qe->message, sizeof qe->message, "결과가 너무 많습니다 (요청당 최대 %" PRId64 "행)",
                     q->max_rows);
        } else if (res->over) {
            qe->status = 400;
            qe->code = "RESULT_TOO_LARGE";
            snprintf(qe->message, sizeof qe->message, "응답이 너무 큽니다 (요청당 최대 %zu바이트)", max_response);
        } else {
            qe->status = 500;
            qe->code = "INTERNAL";
        }
        break;
    case DBE_STORAGE:
    case DBE_REOPEN_FAILED: /* Windows 전용. 서버에서는 나오지 않는다 */
        qe->status = 500;
        qe->code = "STORAGE";
        break;
    case DBE_INDEX_FAILED:
        qe->status = 500;
        qe->code = "INDEX_FAILED";
        qe->stored_id = stored;
        break;
    case DBE_WRITE_FAILED:
        qe->status = 500;
        qe->code = "WRITE_FAILED";
        break;
    case DBE_HALTED:
        qe->status = 503;
        qe->code = "HALTED";
        qe->reason = halt_reason_code(db_state(s->db));
        break;
    default: /* DBE_SQL, DBE_NONE */
        qe->status = 400;
        qe->code = "SQL_ERROR";
        break;
    }
}

static void free_statements(Statement *sts, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        statement_free(&sts[i]);
    }
    free(sts);
}

/* 응답을 보내고 상태 코드를 돌려준다(로그용). nstmt에는 문장 수를 담는다. */
static int handle_query(Server *s, int fd, const HttpRequest *req, size_t *nstmt, const char **logcode) {
    Statement *sts = NULL;
    size_t n = 0, cap = 0, pos = 0, k;
    JsonBuf res, head;
    QueryCtx q;
    QueryError qe;
    int failed = 0;
    size_t nresults = 0;

    *nstmt = 0;
    *logcode = "";
    memset(&qe, 0, sizeof qe);

    if (db_state(s->db) != DB_OK) {
        char msg[200];
        snprintf(msg, sizeof msg, "엔진이 중단 상태입니다 (%s). 서버를 재시작해야 합니다",
                 halt_reason_code(db_state(s->db)));
        *logcode = "HALTED";
        send_simple_error(fd, 503, "HALTED", msg, 1, NULL);
        return 503;
    }

    /* 1) 문장 전체를 먼저 파싱한다. 구문 오류가 있으면 아무것도 실행하지 않는다 (DB-P3-D08) */
    if (memchr(req->body, '\0', req->body_len) != NULL) {
        *logcode = "SQL_SYNTAX";
        send_simple_error(fd, 400, "SQL_SYNTAX", "본문에 NUL 바이트가 있습니다", 1, NULL);
        return 400;
    }
    if (req->body_len >= 3 && memcmp(req->body, "\xEF\xBB\xBF", 3) == 0) {
        pos = 3;
    }
    for (;;) {
        Statement st;
        DbError err = {{0}, DBE_NONE};
        int r = parser_next(req->body, &pos, &st, &err);

        if (r == 0) {
            break;
        }
        if (r < 0) {
            JsonBuf b;
            free_statements(sts, n);
            jb_init(&b, RESPONSE_RESERVE);
            *logcode = "SQL_SYNTAX";
            if (jb_lit(&b, "{\"ok\":false,\"error\":{\"statement\":") != 0 || jb_i64(&b, (int64_t)(n + 1)) != 0 ||
                jb_lit(&b, ",\"code\":\"SQL_SYNTAX\",\"message\":") != 0 || jb_str(&b, err.msg) != 0 ||
                jb_lit(&b, "},\"results\":[]}") != 0) {
                jb_free(&b);
                send_internal(fd);
                return 500;
            }
            send_body(fd, 400, NULL, b.data, b.len, "", 0, "", 0);
            jb_free(&b);
            return 400;
        }
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            Statement *grown = realloc(sts, ncap * sizeof *sts);
            if (grown == NULL) {
                statement_free(&st);
                free_statements(sts, n);
                *logcode = "INTERNAL";
                send_internal(fd);
                return 500;
            }
            sts = grown;
            cap = ncap;
        }
        sts[n++] = st;
    }
    *nstmt = n;

    /* 2) 문장마다 실행. 결과는 res에, 예산은 요청 전체 기준 */
    jb_init(&res, s->max_response - RESPONSE_RESERVE);
    q.res = &res;
    q.rows_total = 0;
    q.max_rows = s->max_rows;
    q.over_rows = 0;
    q.first_row = 1;

    for (k = 0; k < n && !failed; k++) {
        const Statement *st = &sts[k];
        ResultSink sink = {json_on_insert, json_on_row, &q, json_on_plan, json_on_change};
        DbError err = {{0}, DBE_NONE};
        size_t mark = res.len;
        const char *sep = nresults > 0 ? "," : "";
        int64_t stored = 0;
        int prep;

#ifdef MINIDB_TESTING
        if (s->test_stmt_delay_ms > 0) {
            sleep_ms(s->test_stmt_delay_ms); /* 종료 절차 테스트용: 문장 사이를 일부러 늘린다 */
        }
#endif
        /* 문장 경계: 종료 유예 시간이 지났으면 남은 문장은 실행하지 않는다 (spec "종료" 4) */
        if (atomic_load(&g_force)) {
            qe.status = 503;
            qe.code = "SHUTTING_DOWN";
            snprintf(qe.message, sizeof qe.message, "서버가 종료 중이라 이 문장부터 실행하지 않았습니다");
            qe.statement = (long)(k + 1);
            failed = 1;
            break;
        }
        res.over = res.oom = 0;
        q.first_row = 1;
        if (st->kind == STMT_INSERT) {
            /* 결과를 쓸 자리가 없으면 실행하지 않는다: 저장된 INSERT가 응답에서 빠지지 않게 */
            prep = jb_reserve(&res, strlen(sep) + INSERT_RESULT_MAX) == 0 ? jb_lit(&res, sep) : -1;
        } else if (st->kind == STMT_UPDATE || st->kind == STMT_DELETE) {
            /* INSERT와 같은 이유로 결과 자리를 먼저 확보한다: 자리가 없으면 파일을 다시 쓰지 않는다 */
            prep = jb_reserve(&res, strlen(sep) + CHANGE_RESULT_MAX) == 0 ? jb_lit(&res, sep) : -1;
        } else if (!st->explain) {
            prep = (jb_lit(&res, sep) == 0 &&
                    jb_lit(&res, "{\"type\":\"select\",\"columns\":[\"id\",\"name\",\"age\"],\"rows\":[") == 0)
                       ? 0
                       : -1;
        } else {
            prep = jb_lit(&res, sep);
        }
        if (prep != 0) {
            jb_truncate(&res, mark);
            err.kind = DBE_SINK;
            classify(&qe, &err, &res, &q, s, 0, s->max_response);
            qe.statement = (long)(k + 1);
            failed = 1;
            break;
        }
        if (db_exec(s->db, st, &sink, &stored, &err) != 0 ||
            (st->kind == STMT_SELECT && !st->explain && jb_lit(&res, "]}") != 0)) {
            if (err.kind == DBE_NONE && (res.over || res.oom)) {
                err.kind = DBE_SINK; /* 닫는 괄호를 못 씀 */
            }
            jb_truncate(&res, mark);
            classify(&qe, &err, &res, &q, s, stored, s->max_response);
            qe.statement = (long)(k + 1);
            failed = 1;
            break;
        }
        nresults++;
    }
    free_statements(sts, n);

    if (failed && strcmp(qe.code, "INTERNAL") == 0) {
        jb_free(&res);
        *logcode = "INTERNAL";
        send_internal(fd);
        return 500;
    }

    /* 3) 머리: {"ok":true,"results":[  또는  {"ok":false,"error":{...},"results":[ */
    jb_init(&head, RESPONSE_RESERVE - 2);
    if (!failed) {
        jb_lit(&head, "{\"ok\":true,\"results\":[");
    } else {
        *logcode = qe.code;
        jb_lit(&head, "{\"ok\":false,\"error\":{\"statement\":");
        jb_i64(&head, qe.statement);
        jb_lit(&head, ",\"code\":\"");
        jb_lit(&head, qe.code);
        jb_lit(&head, "\",\"message\":");
        jb_str(&head, qe.message);
        if (qe.stored_id > 0) {
            jb_lit(&head, ",\"stored_id\":");
            jb_i64(&head, qe.stored_id);
        }
        if (qe.reason != NULL) {
            jb_lit(&head, ",\"reason\":\"");
            jb_lit(&head, qe.reason);
            jb_lit(&head, "\"");
        }
        jb_lit(&head, "},\"results\":[");
    }
    if (head.over || head.oom) {
        jb_free(&head);
        jb_free(&res);
        *logcode = "INTERNAL";
        send_internal(fd);
        return 500;
    }
    send_body(fd, failed ? qe.status : 200, NULL, head.data, head.len, res.data != NULL ? res.data : "", res.len,
              "]}", 2);
    jb_free(&head);
    jb_free(&res);
    return failed ? qe.status : 200;
}

/* ---------- /health ---------- */

static int handle_health(Server *s, int fd) {
    DbState st = db_state(s->db);
    char body[160];
    int n;

    if (st != DB_OK) {
        n = snprintf(body, sizeof body, "{\"ok\":false,\"state\":\"halted\",\"reason\":\"%s\"}", halt_reason_code(st));
        send_body(fd, 503, NULL, body, (size_t)n, "", 0, "", 0);
        return 503;
    } else {
        IndexStats ix;
        db_stats(s->db, &ix);
        n = snprintf(body, sizeof body, "{\"ok\":true,\"state\":\"ok\",\"rows\":%" PRId64 "}", ix.keys);
        send_body(fd, 200, NULL, body, (size_t)n, "", 0, "", 0);
        return 200;
    }
}

/* ---------- GET /users (Phase 5) ---------- */

/* 정규형 10진수(앞자리 0 없음, "0"은 허용, 최대 19자리, INT64_MAX 이하)면 0 */
static int parse_param_num(const char *s, size_t n, int64_t *out) {
    int64_t v = 0;
    size_t i;

    if (n == 0 || n > 19 || (s[0] == '0' && n > 1)) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int d;
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        d = s[i] - '0';
        if (v > (INT64_MAX - d) / 10) {
            return -1;
        }
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/*
 * 쿼리 문자열 → after_id, limit, age (Phase 5 spec "입력 검증" DB-P5-D06, Phase 6 DB-P6-D03·D04).
 * 0 성공, -1이면 msg에 이유. 항목은 '&'로 나눈다. 이름은 after_id·limit·age만,
 * 같은 이름 두 번·'=' 없음·빈 항목·%XX(숫자가 아님)는 거부. age는 0~150(범위 밖은 맞는 행이 있을 수 없어 거부).
 */
static int parse_users_query(const Server *s, const HttpRequest *req, int64_t *after, int64_t *limit,
                             PageFilter *filter, char *msg, size_t cap) {
    const char *p = req->query;
    int seen_after = 0, seen_limit = 0, seen_age = 0;

    *after = 0;
    *limit = s->page_default;
    filter->has_age = 0;
    filter->age = 0;
    filter->path = s->age_path;
    if (req->query_over) {
        snprintf(msg, cap, "쿼리 문자열이 너무 깁니다 (최대 %d바이트)", HTTP_MAX_QUERY);
        return -1;
    }
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t ilen = amp != NULL ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', ilen);
        size_t klen, vlen;
        const char *v;
        int64_t num;

        if (ilen == 0) {
            snprintf(msg, cap, "쿼리 문자열에 빈 항목이 있습니다 ('&'가 연속되거나 끝에 있음)");
            return -1;
        }
        if (eq == NULL) {
            snprintf(msg, cap, "파라미터 '%.*s'에 값이 없습니다 (이름=값)", (int)(ilen > 40 ? 40 : ilen), p);
            return -1;
        }
        klen = (size_t)(eq - p);
        v = eq + 1;
        vlen = ilen - klen - 1;
        if (klen == 8 && memcmp(p, "after_id", 8) == 0) {
            if (seen_after++) {
                snprintf(msg, cap, "after_id가 두 번 있습니다");
                return -1;
            }
            if (parse_param_num(v, vlen, &num) != 0) {
                snprintf(msg, cap, "after_id는 0 이상 %" PRId64 " 이하의 10진 정수여야 합니다 (앞자리 0·부호 불가, 현재: '%.*s')",
                         (int64_t)INT64_MAX, (int)(vlen > 40 ? 40 : vlen), v);
                return -1;
            }
            *after = num;
        } else if (klen == 5 && memcmp(p, "limit", 5) == 0) {
            if (seen_limit++) {
                snprintf(msg, cap, "limit이 두 번 있습니다");
                return -1;
            }
            if (parse_param_num(v, vlen, &num) != 0 || num < 1 || num > s->page_max) {
                snprintf(msg, cap, "limit은 1 이상 %" PRId64 " 이하의 10진 정수여야 합니다 (앞자리 0·부호 불가, 현재: '%.*s')",
                         s->page_max, (int)(vlen > 40 ? 40 : vlen), v);
                return -1;
            }
            *limit = num;
        } else if (klen == 3 && memcmp(p, "age", 3) == 0) {
            if (seen_age++) {
                snprintf(msg, cap, "age가 두 번 있습니다");
                return -1;
            }
            if (parse_param_num(v, vlen, &num) != 0 || num < AGE_MIN || num > AGE_MAX) {
                snprintf(msg, cap, "age는 %d 이상 %d 이하의 10진 정수여야 합니다 (앞자리 0·부호 불가, 현재: '%.*s')", AGE_MIN,
                         AGE_MAX, (int)(vlen > 40 ? 40 : vlen), v);
                return -1;
            }
            filter->has_age = 1;
            filter->age = (int)num;
        } else {
            snprintf(msg, cap, "모르는 파라미터입니다: '%.*s' (after_id, limit, age만 받습니다)",
                     (int)(klen > 40 ? 40 : klen), p);
            return -1;
        }
        p += ilen;
        if (*p == '&') {
            p++;
            if (*p == '\0') {
                snprintf(msg, cap, "쿼리 문자열에 빈 항목이 있습니다 ('&'가 연속되거나 끝에 있음)");
                return -1;
            }
        }
    }
    return 0;
}

typedef struct {
    JsonBuf *res;
    int64_t n;
    int64_t last_id;
} UsersCtx;

static int users_on_row(const Row *row, void *vctx) {
    UsersCtx *u = vctx;

    if (json_row(u->res, row, u->n == 0) != 0) {
        return 1;
    }
    u->n++;
    u->last_id = row->id;
    return 0;
}

static const char *access_path_name(PageUsed u) {
    switch (u) {
    case PAGE_USED_SCAN: return "scan";
    case PAGE_USED_AGE_INDEX: return "age-index";
    default: return "id";
    }
}

/* note: 요청 로그에 붙일 계측 요약 (200일 때만 채움) */
static int handle_users(Server *s, int fd, const HttpRequest *req, const char **logcode, char *note,
                        size_t note_cap) {
    char msg[200], hdr[256];
    int64_t after, limit, engine_us = 0;
    PageFilter filter;
    JsonBuf res;
    UsersCtx u;
    DbError err = {{0}, DBE_NONE};
    PageStats ps;
    double t0;
    int rc;

    /* 1) 입력 검증: 틀리면 엔진을 부르지 않는다 */
    if (parse_users_query(s, req, &after, &limit, &filter, msg, sizeof msg) != 0) {
        *logcode = "INVALID_PARAM";
        send_simple_error(fd, 400, "INVALID_PARAM", msg, 0, NULL);
        return 400;
    }
    /* 2) 페이지 읽기 (공유 잠금, 콜백은 메모리 버퍼에만 쓴다). 응답 상한은 page_max로 미리 보장했다(D08).
       엔진 시간(DB-P6-D10) = db_page 호출 앞뒤: 잠금 대기 + 탐색 + 맞는 행의 JSON 추가 */
    memset(&ps, 0, sizeof ps);
    jb_init(&res, s->max_response);
    u.res = &res;
    u.n = 0;
    u.last_id = 0;
    rc = jb_lit(&res, USERS_HEAD);
    if (rc == 0) {
        t0 = now_ms();
        rc = db_page(s->db, after, limit, filter.has_age ? &filter : NULL, users_on_row, &u, &ps, &err);
        engine_us = (int64_t)((now_ms() - t0) * 1000.0);
    }
    if (rc != 0 ||
        jb_lit(&res, ps.has_more ? "],\"has_more\":true,\"next_cursor\":" : "],\"has_more\":false,\"next_cursor\":") != 0 ||
        (u.n > 0 ? jb_i64(&res, u.last_id) : jb_lit(&res, "null")) != 0 || jb_lit(&res, "}") != 0) {
        jb_free(&res);
        if (err.kind == DBE_HALTED) {
            snprintf(msg, sizeof msg, "엔진이 중단 상태입니다 (%s). 서버를 재시작해야 합니다",
                     halt_reason_code(db_state(s->db)));
            *logcode = "HALTED";
            send_simple_error(fd, 503, "HALTED", msg, 0, NULL);
            return 503;
        }
        if (err.kind == DBE_STORAGE) {
            *logcode = "STORAGE";
            send_simple_error(fd, 500, "STORAGE", err.msg, 0, NULL);
            return 500;
        }
        *logcode = "INTERNAL"; /* 버퍼 메모리 부족 (상한 초과는 page_max 계산으로 생기지 않는다) */
        send_internal(fd);
        return 500;
    }
    /* 3) 계측 헤더 (DB-P6-D09·D16). 본문은 Phase 5와 같다 */
    snprintf(hdr, sizeof hdr,
             "X-Rows-Examined: %" PRId64 "\r\nX-Rows-Examined-After-Last: %" PRId64 "\r\nX-Age-Index-Keys: %" PRId64
             "\r\nX-Access-Path: %s\r\nX-Engine-Us: %" PRId64 "\r\n",
             ps.rows_examined, ps.examined_after_last, ps.index_keys, access_path_name(ps.used), engine_us);
    snprintf(note, note_cap, ", %s R=%" PRId64 " E=%" PRId64 " T=%" PRId64 " K=%" PRId64 " 엔진 %" PRId64 " us",
             access_path_name(ps.used), ps.rows_returned, ps.rows_examined, ps.examined_after_last, ps.index_keys,
             engine_us);
    send_body(fd, 200, hdr, res.data, res.len, "", 0, "", 0);
    jb_free(&res);
    return 200;
}

/* ---------- 연결 하나 ---------- */

static const char *code_for_status(int status) {
    switch (status) {
    case 408: return "TIMEOUT";
    case 413: return "BODY_TOO_LARGE";
    case 431: return "HEADER_TOO_LARGE";
    case 500: return "INTERNAL";
    default: return "BAD_REQUEST";
    }
}

static void handle_connection(Server *s, int fd) {
    HttpRequest req;
    char why[200] = "";
    double t0 = now_ms();
    int r = http_read_request(fd, &req, why, sizeof why);
    int status;
    size_t nstmt = 0;
    const char *code = "";
    char note[200] = "";

    if (r < 0) {
        fprintf(stderr, "[요청] (응답 없음: 연결이 닫히거나 읽기 실패)\n");
        return;
    }
    if (r > 0) {
        send_simple_error(fd, r, code_for_status(r), why, 0, NULL);
        fprintf(stderr, "[요청] %d %s (%s)\n", r, code_for_status(r), why);
        return;
    }
    if (strcmp(req.path, "/query") == 0) {
        if (strcmp(req.method, "POST") == 0) {
            status = handle_query(s, fd, &req, &nstmt, &code);
        } else {
            status = 405;
            code = "METHOD";
            send_simple_error(fd, 405, "METHOD", "/query는 POST만 받습니다", 0, "Allow: POST\r\n");
        }
    } else if (strcmp(req.path, "/health") == 0) {
        if (strcmp(req.method, "GET") == 0) {
            status = handle_health(s, fd);
        } else {
            status = 405;
            code = "METHOD";
            send_simple_error(fd, 405, "METHOD", "/health는 GET만 받습니다", 0, "Allow: GET\r\n");
        }
    } else if (strcmp(req.path, "/users") == 0) {
        if (strcmp(req.method, "GET") == 0) {
            status = handle_users(s, fd, &req, &code, note, sizeof note);
        } else {
            status = 405;
            code = "METHOD";
            send_simple_error(fd, 405, "METHOD", "/users는 GET만 받습니다", 0, "Allow: GET\r\n");
        }
    } else {
        status = 404;
        code = "NOT_FOUND";
        send_simple_error(fd, 404, "NOT_FOUND", "없는 경로입니다 (POST /query, GET /health, GET /users)", 0, NULL);
    }
    if (strcmp(req.path, "/query") == 0 && strcmp(req.method, "POST") == 0) {
        fprintf(stderr, "[요청] %s %s → %d%s%s, 문장 %zu개, %.2f ms\n", req.method, req.path, status,
                code[0] ? " " : "", code, nstmt, now_ms() - t0);
    } else {
        fprintf(stderr, "[요청] %s %s → %d%s%s%s, %.2f ms\n", req.method, req.path, status, code[0] ? " " : "",
                code, note, now_ms() - t0);
    }
    http_request_free(&req);
}

/* ---------- 워커 ---------- */

typedef struct {
    Server *s;
    int idx;
} WorkerArg;

static void *worker_main(void *p) {
    WorkerArg *w = p;
    Server *s = w->s;
    int fd;

    while (wq_pop(&s->queue, &fd) == 0) {
        pthread_mutex_lock(&s->act_mu);
        s->active[w->idx] = fd;
        if (atomic_load(&g_force)) {
            shutdown(fd, SHUT_RDWR); /* 유예 시간이 이미 지났다: 꺼내자마자 끊는다 */
        }
        pthread_mutex_unlock(&s->act_mu);

        handle_connection(s, fd);

        pthread_mutex_lock(&s->act_mu);
        s->active[w->idx] = -1; /* close 전에 비운다: main이 재사용된 fd 번호를 건드리지 않게 */
        pthread_mutex_unlock(&s->act_mu);
        http_close(fd);
    }
    pthread_mutex_lock(&s->act_mu);
    s->live--;
    pthread_mutex_unlock(&s->act_mu);
    return NULL;
}

static int live_workers(Server *s) {
    int n;
    pthread_mutex_lock(&s->act_mu);
    n = s->live;
    pthread_mutex_unlock(&s->act_mu);
    return n;
}

/* 유예 시간이 지났을 때: 문장 경계 플래그를 세우고 처리 중인 연결의 입출력을 끊는다.
   엔진 안에서 실행 중인 문장은 끊지 않는다(소켓만 끊으므로 db_exec는 그대로 끝난다). */
static int force_close_active(Server *s) {
    int i, n = 0;

    pthread_mutex_lock(&s->act_mu);
    atomic_store(&g_force, 1);
    for (i = 0; i < s->threads; i++) {
        if (s->active[i] >= 0) {
            shutdown(s->active[i], SHUT_RDWR);
            n++;
        }
    }
    pthread_mutex_unlock(&s->act_mu);
    return n;
}

/* ---------- 시작 ---------- */

static void print_usage(FILE *out) {
    fprintf(out,
            "사용법: minidb-server [--data-dir 디렉터리] [--host 주소] [--port 번호]\n"
            "                      [--threads 개수] [--queue 개수]\n"
            "                      [--max-rows 행수] [--max-response 바이트] [--age-path scan|index]\n"
            "  --data-dir      데이터 파일(users.tsv) 위치. 기본값: data\n"
            "  --host          받을 IPv4 주소. 기본값: 127.0.0.1 (이 컴퓨터에서만)\n"
            "  --port          포트. 기본값: 8080. 0이면 빈 포트를 골라 시작 줄에 알려 준다\n"
            "  --threads       워커 스레드 수(1~%d). 기본값: %d\n"
            "  --queue         처리를 기다리는 연결 수 상한(1~%d). 넘치면 503 BUSY. 기본값: %d\n"
            "  --max-rows      요청 하나의 SELECT 결과 행 합계 상한. 기본값: %d\n"
            "  --max-response  요청 하나의 JSON 응답 본문 상한(바이트, %d 이상). 기본값: %d\n"
            "  --age-path      GET /users?age=의 탐색 경로. index(기본): age 보조 인덱스, scan: 선형 탐색(비교용).\n"
            "                  응답 본문은 같고 계측 헤더(X-Access-Path 등)만 다르다. age 인덱스는 어느 쪽이든 유지한다\n"
            "  GET /users의 limit 상한은 min(1000, --max-rows, --max-response에 최악의 행이 들어가는 수)이다\n",
            MAX_THREADS, DEFAULT_THREADS, MAX_QUEUE, DEFAULT_QUEUE, DEFAULT_MAX_ROWS, MIN_MAX_RESPONSE,
            DEFAULT_MAX_RESPONSE);
}

/* 10진 정수 옵션 값. 범위 밖이거나 숫자가 아니면 -1 */
static long long parse_num(const char *s, long long lo, long long hi) {
    char *end;
    long long v;

    if (s == NULL || *s == '\0' || *s == '-' || *s == '+') {
        return -1;
    }
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    return v;
}

static int usage_error(const char *msg, const char *arg) {
    fprintf(stderr, "오류: %s%s\n", msg, arg != NULL ? arg : "");
    print_usage(stderr);
    return EXIT_USAGE;
}

#ifdef MINIDB_TESTING
/* 테스트 전용 빌드(minidb-server-test): 배포용 minidb-server에는 없는 조절 장치.
 *   MINIDB_TEST_FAULT=index|write   저장 후 실패 경로 재현
 *   MINIDB_TEST_GRACE_MS=밀리초      종료 유예 시간(기본 10초)을 줄인다
 *   MINIDB_TEST_STMT_DELAY_MS=밀리초 문장마다 실행 전에 잠깐 쉰다(잠금 밖). 종료 절차를 재현하려고 */
static void apply_test_knobs(Server *s) {
    const char *f = getenv("MINIDB_TEST_FAULT");
    const char *g = getenv("MINIDB_TEST_GRACE_MS");
    const char *d = getenv("MINIDB_TEST_STMT_DELAY_MS");

    if (f != NULL && strcmp(f, "index") == 0) {
        bpt_test_fail_alloc_after(0); /* 다음 트리 노드 할당(분할)이 실패 */
    } else if (f != NULL && strcmp(f, "write") == 0) {
        table_test_fail_write(1); /* 다음 INSERT가 줄 일부만 쓰고 실패 */
    }
    if (g != NULL) {
        s->grace_ms = atol(g);
    }
    if (d != NULL) {
        s->test_stmt_delay_ms = atol(d);
    }
}
#endif

int main(int argc, char **argv) {
    const char *data_dir = "data";
    const char *host = "127.0.0.1";
    long long port = 8080;
    Server s;
    DbError err = {{0}, DBE_NONE};
    struct sockaddr_in addr;
    struct sigaction sa;
    sigset_t block, old;
    socklen_t alen = sizeof addr;
    int lfd, one = 1, i, started = 0, rc = 0;
    double t0;
    IndexStats ix;
    pthread_t *tids = NULL;
    WorkerArg *wargs = NULL;
    int *rest = NULL;
    size_t nrest, k;

    memset(&s, 0, sizeof s);
    s.max_rows = DEFAULT_MAX_ROWS;
    s.max_response = DEFAULT_MAX_RESPONSE;
    s.threads = DEFAULT_THREADS;
    s.queue_cap = DEFAULT_QUEUE;
    s.grace_ms = SHUTDOWN_GRACE_MS;
    s.age_path = PAGE_PATH_INDEX; /* DB-P6-D17 */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(a, "--data-dir") == 0) {
            if (v == NULL || v[0] == '\0') {
                return usage_error("--data-dir 뒤에 디렉터리가 필요합니다", NULL);
            }
            data_dir = v;
        } else if (strcmp(a, "--host") == 0) {
            if (v == NULL) {
                return usage_error("--host 뒤에 주소가 필요합니다", NULL);
            }
            host = v;
        } else if (strcmp(a, "--port") == 0) {
            if ((port = parse_num(v, 0, 65535)) < 0) {
                return usage_error("--port는 0~65535 정수여야 합니다: ", v);
            }
        } else if (strcmp(a, "--max-rows") == 0) {
            long long n = parse_num(v, 1, 1000000000LL);
            if (n < 0) {
                return usage_error("--max-rows는 1 이상의 정수여야 합니다: ", v);
            }
            s.max_rows = n;
        } else if (strcmp(a, "--max-response") == 0) {
            long long n = parse_num(v, MIN_MAX_RESPONSE, 1LL << 30);
            if (n < 0) {
                return usage_error("--max-response는 4096 ~ 1073741824 사이 정수여야 합니다: ", v);
            }
            s.max_response = (size_t)n;
        } else if (strcmp(a, "--age-path") == 0) {
            if (v != NULL && strcmp(v, "scan") == 0) {
                s.age_path = PAGE_PATH_SCAN;
            } else if (v != NULL && strcmp(v, "index") == 0) {
                s.age_path = PAGE_PATH_INDEX;
            } else {
                return usage_error("--age-path는 scan 또는 index여야 합니다: ", v != NULL ? v : "(없음)");
            }
        } else if (strcmp(a, "--threads") == 0) {
            long long n = parse_num(v, 1, MAX_THREADS);
            if (n < 0) {
                return usage_error("--threads는 1 ~ 256 사이 정수여야 합니다: ", v);
            }
            s.threads = (int)n;
        } else if (strcmp(a, "--queue") == 0) {
            long long n = parse_num(v, 1, MAX_QUEUE);
            if (n < 0) {
                return usage_error("--queue는 1 ~ 4096 사이 정수여야 합니다: ", v);
            }
            s.queue_cap = (size_t)n;
        } else {
            return usage_error("알 수 없는 인자: ", a);
        }
        i++;
    }
    /* GET /users의 페이지 상한 (DB-P5-D08): 최악의 행으로 채워도 --max-response·--max-rows 안에 든다.
       --max-response ≥ 4096이므로 바이트로 들어가는 행은 2 이상이다 */
    {
        int64_t by_bytes = (int64_t)((s.max_response - USERS_FIXED_JSON_MAX) / USERS_ROW_JSON_MAX);
        s.page_max = USERS_LIMIT_MAX;
        if (s.max_rows < s.page_max) {
            s.page_max = s.max_rows;
        }
        if (by_bytes < s.page_max) {
            s.page_max = by_bytes;
        }
        s.page_default = s.page_max < USERS_LIMIT_DEFAULT ? s.page_max : USERS_LIMIT_DEFAULT;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return usage_error("--host는 IPv4 주소여야 합니다 (예: 127.0.0.1, 0.0.0.0): ", host);
    }

    /* 신호: SIGINT·SIGTERM은 종료 요청, SIGPIPE는 무시(끊긴 소켓에 쓰면 EPIPE로 받는다) */
    if (pipe(g_wake) != 0) {
        perror("pipe");
        return EXIT_FAIL;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    t0 = now_ms();
    if (db_open(data_dir, &s.db, &err) != 0) {
        fprintf(stderr, "오류: %s\n", err.msg);
        return EXIT_FAIL;
    }
#ifdef MINIDB_TESTING
    apply_test_knobs(&s);
#endif
    db_stats(s.db, &ix);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lfd, 128) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) {
        char eb[128];
        fprintf(stderr, "오류: %s:%lld에서 연결을 받을 수 없습니다 (%s)\n", host, port,
                db_errstr(errno, eb, sizeof eb));
        if (lfd >= 0) {
            close(lfd);
        }
        db_close(s.db, &err);
        return EXIT_FAIL;
    }

    /* 큐와 워커. 워커는 SIGINT·SIGTERM을 막아 둔 채로 만든다: 신호는 main(accept 스레드)만 받는다. */
    tids = calloc((size_t)s.threads, sizeof *tids);
    wargs = calloc((size_t)s.threads, sizeof *wargs);
    s.active = malloc((size_t)s.threads * sizeof *s.active);
    rest = malloc(s.queue_cap * sizeof *rest);
    if (tids == NULL || wargs == NULL || s.active == NULL || rest == NULL || wq_init(&s.queue, s.queue_cap) != 0) {
        fprintf(stderr, "오류: 메모리가 부족합니다\n");
        free(tids);
        free(wargs);
        free(s.active);
        free(rest);
        close(lfd);
        db_close(s.db, &err);
        return EXIT_FAIL;
    }
    pthread_mutex_init(&s.act_mu, NULL);
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    for (i = 0; i < s.threads; i++) {
        s.active[i] = -1;
        wargs[i].s = &s;
        wargs[i].idx = i;
        pthread_mutex_lock(&s.act_mu);
        s.live++;
        pthread_mutex_unlock(&s.act_mu);
        if (pthread_create(&tids[i], NULL, worker_main, &wargs[i]) != 0) {
            pthread_mutex_lock(&s.act_mu);
            s.live--;
            pthread_mutex_unlock(&s.act_mu);
            break;
        }
        started++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (started < s.threads) {
        fprintf(stderr, "오류: 워커 스레드를 만들 수 없습니다 (%d/%d)\n", started, s.threads);
        g_stop = 1;
        rc = EXIT_FAIL;
    } else {
        fprintf(stderr,
                "[서버] 시작: %.1f ms (%" PRId64 "행), %s:%d, 워커 %d개, 큐 %zu, 응답 상한 %zu바이트·%" PRId64
                "행, 페이지 상한 %" PRId64 "행(기본 %" PRId64 "), age 경로 %s\n",
                now_ms() - t0, ix.keys, host, ntohs(addr.sin_port), s.threads, s.queue_cap, s.max_response,
                s.max_rows, s.page_max, s.page_default, s.age_path == PAGE_PATH_SCAN ? "scan" : "index");
    }

    /* accept 스레드: 연결을 큐에 넣기만 한다. 큐에서 기다리거나 요청을 읽지 않는다. */
    while (!g_stop) {
        struct pollfd p[2];
        int fd;

        p[0].fd = lfd;
        p[0].events = POLLIN;
        p[1].fd = g_wake[0];
        p[1].events = POLLIN;
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (g_stop || !(p[0].revents & POLLIN)) {
            continue;
        }
        fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        if (wq_try_push(&s.queue, fd) != 0) {
            send_fast_503(fd, "BUSY", k_busy_msg);
            fprintf(stderr, "[요청] 503 BUSY (큐 %zu개가 가득 참)\n", s.queue_cap);
        }
    }

    /* 종료 절차 (spec "종료") */
    close(lfd); /* 1) 새 연결을 받지 않는다 */
    nrest = wq_close(&s.queue, rest);
    for (k = 0; k < nrest; k++) { /* 2) 큐에서 기다리던 연결에는 503 SHUTTING_DOWN */
        send_fast_503(rest[k], "SHUTTING_DOWN", k_shutdown_msg);
    }
    if (rc == 0) {
        double t_stop = now_ms();
        fprintf(stderr, "[서버] 종료 중: 대기하던 연결 %zu개에 503, 처리 중인 요청을 기다림(유예 %ld ms)\n", nrest,
                s.grace_ms);
        /* 3) 처리 중인 요청은 끝까지. 4) 유예 시간이 지나면 남은 연결을 끊고 문장 경계에서 멈추게 한다 */
        while (live_workers(&s) > 0 && now_ms() - t_stop < (double)s.grace_ms) {
            sleep_ms(20);
        }
        if (live_workers(&s) > 0) {
            int n = force_close_active(&s);
            fprintf(stderr, "[서버] 유예 시간이 지나 연결 %d개를 끊음. 실행 중인 문장은 끝까지 기다림\n", n);
        }
    }
    for (i = 0; i < started; i++) { /* 5) 실행 중인 문장이 끝날 때까지 기다린다(상한 없음) */
        pthread_join(tids[i], NULL);
    }
    wq_destroy(&s.queue);
    pthread_mutex_destroy(&s.act_mu);
    free(tids);
    free(wargs);
    free(s.active);
    free(rest);

    if (db_close(s.db, &err) != 0) { /* 6) 파일을 닫는다 */
        fprintf(stderr, "오류: %s\n", err.msg);
        return EXIT_FAIL;
    }
    if (rc == 0) {
        fprintf(stderr, "[서버] 종료\n");
    }
    return rc;
}
