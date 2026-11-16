/*
 * API 서버 (Phase 3, POSIX 전용). P3-02: 스레드 하나로 연결을 하나씩 처리한다.
 *
 * 요청 하나의 흐름:
 *   accept → http_read_request(http.c) → 경로 확인
 *   → POST /query: 본문의 문장을 모두 parser_next로 파싱(잠금 밖)
 *                  → 문장마다 db_exec(db.c, 잠금 안) → 결과를 JSON 버퍼에(json.c)
 *   → http_send_response(잠금 밖) → http_close
 *
 * 종료 코드: 0 정상 종료(SIGINT·SIGTERM), 1 시작 실패, 2 잘못된 사용법
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
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

typedef struct {
    Db *db;
    int64_t max_rows;
    size_t max_response;
} Server;

static volatile sig_atomic_t g_stop;
static int g_wake[2] = {-1, -1};

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

        res.over = res.oom = 0;
        q.first_row = 1;
        if (st->kind == STMT_INSERT) {
            /* 결과를 쓸 자리가 없으면 실행하지 않는다: 저장된 INSERT가 응답에서 빠지지 않게 */
            prep = jb_reserve(&res, strlen(sep) + INSERT_RESULT_MAX) == 0 ? jb_lit(&res, sep) : -1;
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

/* ---------- 시작 ---------- */

static void print_usage(FILE *out) {
    fprintf(out,
            "사용법: minidb-server [--data-dir 디렉터리] [--host 주소] [--port 번호]\n"
            "                      [--max-rows 행수] [--max-response 바이트]\n"
            "  --data-dir      데이터 파일(users.tsv) 위치. 기본값: data\n"
            "  --host          받을 IPv4 주소. 기본값: 127.0.0.1 (이 컴퓨터에서만)\n"
            "  --port          포트. 기본값: 8080. 0이면 빈 포트를 골라 시작 줄에 알려 준다\n"
            "  --max-rows      요청 하나의 SELECT 결과 행 합계 상한. 기본값: %d\n"
            "  --max-response  요청 하나의 JSON 응답 본문 상한(바이트, %d 이상). 기본값: %d\n",
            DEFAULT_MAX_ROWS, MIN_MAX_RESPONSE, DEFAULT_MAX_RESPONSE);
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
/* 테스트 전용 빌드(minidb-server-test): 저장 후 실패 경로를 서버 테스트에서 재현한다. */
static void apply_test_fault(void) {
    const char *f = getenv("MINIDB_TEST_FAULT");
    if (f == NULL) {
        return;
    }
    if (strcmp(f, "index") == 0) {
        bpt_test_fail_alloc_after(0); /* 다음 트리 노드 할당(분할)이 실패 */
    } else if (strcmp(f, "write") == 0) {
        table_test_fail_write(1); /* 다음 INSERT가 줄 일부만 쓰고 실패 */
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
    socklen_t alen = sizeof addr;
    int lfd, one = 1, i;
    double t0;
    IndexStats ix;

    s.max_rows = DEFAULT_MAX_ROWS;
    s.max_response = DEFAULT_MAX_RESPONSE;
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
    apply_test_fault();
#endif
    db_stats(s.db, &ix);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lfd, 64) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) {
        char eb[128];
        fprintf(stderr, "오류: %s:%lld에서 연결을 받을 수 없습니다 (%s)\n", host, port,
                db_errstr(errno, eb, sizeof eb));
        db_close(s.db, &err);
        return EXIT_FAIL;
    }
    fprintf(stderr, "[서버] 시작: %.1f ms (%" PRId64 "행), %s:%d, 응답 상한 %zu바이트·%" PRId64 "행\n",
            now_ms() - t0, ix.keys, host, ntohs(addr.sin_port), s.max_response, s.max_rows);

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
        handle_connection(&s, fd);
        http_close(fd);
    }

    close(lfd);
    if (db_close(s.db, &err) != 0) {
        fprintf(stderr, "오류: %s\n", err.msg);
        return EXIT_FAIL;
    }
    fprintf(stderr, "[서버] 종료\n");
    return 0;
}
