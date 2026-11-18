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

static int json_on_row(const Row *row, void *vctx) {
    QueryCtx *q = vctx;

    if (++q->rows_total > q->max_rows) {
        q->over_rows = 1;
        return 1;
    }
    if ((!q->first_row && jb_lit(q->res, ",") != 0) || jb_lit(q->res, "[") != 0 ||
        jb_i64(q->res, row->id) != 0 || jb_lit(q->res, ",") != 0 || jb_str(q->res, row->name) != 0 ||
        jb_lit(q->res, ",") != 0 || jb_i64(q->res, row->age) != 0 || jb_lit(q->res, "]") != 0) {
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
    return s == DB_HALTED_INDEX ? "index_failed" : "write_failed";
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
        ResultSink sink = {json_on_insert, json_on_row, &q, json_on_plan};
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
            prep = jb_lit(&res, sep); /* P4-01: 실행 오류로 끝나므로 결과를 쓰지 않는다(JSON 결과는 P4-03) */
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
    } else {
        status = 404;
        code = "NOT_FOUND";
        send_simple_error(fd, 404, "NOT_FOUND", "없는 경로입니다 (POST /query, GET /health)", 0, NULL);
    }
    if (strcmp(req.path, "/query") == 0 && strcmp(req.method, "POST") == 0) {
        fprintf(stderr, "[요청] %s %s → %d%s%s, 문장 %zu개, %.2f ms\n", req.method, req.path, status,
                code[0] ? " " : "", code, nstmt, now_ms() - t0);
    } else {
        fprintf(stderr, "[요청] %s %s → %d%s%s, %.2f ms\n", req.method, req.path, status, code[0] ? " " : "",
                code, now_ms() - t0);
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
            "                      [--max-rows 행수] [--max-response 바이트]\n"
            "  --data-dir      데이터 파일(users.tsv) 위치. 기본값: data\n"
            "  --host          받을 IPv4 주소. 기본값: 127.0.0.1 (이 컴퓨터에서만)\n"
            "  --port          포트. 기본값: 8080. 0이면 빈 포트를 골라 시작 줄에 알려 준다\n"
            "  --threads       워커 스레드 수(1~%d). 기본값: %d\n"
            "  --queue         처리를 기다리는 연결 수 상한(1~%d). 넘치면 503 BUSY. 기본값: %d\n"
            "  --max-rows      요청 하나의 SELECT 결과 행 합계 상한. 기본값: %d\n"
            "  --max-response  요청 하나의 JSON 응답 본문 상한(바이트, %d 이상). 기본값: %d\n",
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
                "[서버] 시작: %.1f ms (%" PRId64 "행), %s:%d, 워커 %d개, 큐 %zu, 응답 상한 %zu바이트·%" PRId64 "행\n",
                now_ms() - t0, ix.keys, host, ntohs(addr.sin_port), s.threads, s.queue_cap, s.max_response,
                s.max_rows);
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
