#define _POSIX_C_SOURCE 200809L
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 소켓을 비차단으로 바꾼다. 읽기·쓰기는 poll로 기다린 뒤에만 한다. */
static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* events가 될 때까지 기다린다. 1 준비됨, 0 기한 초과(호출 5초 또는 전체 기한), -1 오류 */
static int wait_fd(int fd, short events, long long deadline) {
    for (;;) {
        struct pollfd p;
        long long left = deadline - now_ms();
        int r;

        if (left <= 0) {
            return 0;
        }
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        r = poll(&p, 1, (int)(left < HTTP_CALL_TIMEOUT_MS ? left : HTTP_CALL_TIMEOUT_MS));
        if (r > 0) {
            return 1;
        }
        if (r == 0) {
            return 0; /* 호출 하나가 5초 동안 진행되지 않음 */
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

/* 최대 cap바이트를 읽는다. >0 읽은 수, 0 상대가 닫음, -1 오류, -2 기한 초과 */
static long recv_some(int fd, char *buf, size_t cap, long long deadline) {
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        int w;

        if (n >= 0) {
            return (long)n;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        w = wait_fd(fd, POLLIN, deadline);
        if (w == 0) {
            return -2;
        }
        if (w < 0) {
            return -1;
        }
    }
}

static int send_all(int fd, const char *p, size_t n, long long deadline) {
    while (n > 0) {
        ssize_t k = send(fd, p, n, 0);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR) {
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) != 1) {
                return -1;
            }
            continue;
        }
        return -1; /* EPIPE 등. SIGPIPE는 서버가 무시한다 */
    }
    return 0;
}

const char *http_reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

static void set_why(char *why, size_t cap, const char *msg) {
    if (why != NULL && cap > 0) {
        snprintf(why, cap, "%s", msg);
    }
}

/* [a, b)에서 헤더 끝(빈 줄)을 찾는다. CRLF가 원칙이지만 LF만 쓴 요청도 받는다. 끝 바로 다음 위치 또는 NULL */
static const char *find_header_end(const char *a, const char *b) {
    const char *p;
    for (p = a; p < b; p++) {
        if (*p != '\n') {
            continue;
        }
        if (p + 1 < b && p[1] == '\n') {
            return p + 2;
        }
        if (p + 2 < b && p[1] == '\r' && p[2] == '\n') {
            return p + 3;
        }
    }
    return NULL;
}

/* 줄 하나: [*cur, end)에서 LF까지. 끝의 CR은 뺀다. 반환은 줄 시작, *len은 줄 길이 */
static const char *next_line(const char **cur, const char *end, size_t *len) {
    const char *s = *cur;
    const char *e = memchr(s, '\n', (size_t)(end - s));
    const char *stop = e != NULL ? e : end;

    *cur = e != NULL ? e + 1 : end;
    if (stop > s && stop[-1] == '\r') {
        stop--;
    }
    *len = (size_t)(stop - s);
    return s;
}

static int is_token_char(unsigned char c) { return c > 0x20 && c < 0x7F; }

int http_read_request(int fd, HttpRequest *req, char *why, size_t why_cap) {
    char head[HTTP_MAX_HEADER + 1];
    size_t got = 0;
    const char *hend = NULL;
    long long deadline;
    const char *cur, *line;
    size_t len;
    long long content_length = -1;
    int expect_continue = 0;
    size_t i, sp1, sp2;

    memset(req, 0, sizeof *req);
    if (set_nonblocking(fd) != 0) {
        return -1;
    }
    deadline = now_ms() + HTTP_RECV_DEADLINE_MS;

    /* 1) 빈 줄까지 읽는다 (최대 8 KiB) */
    while (hend == NULL) {
        long n;
        if (got >= HTTP_MAX_HEADER) {
            set_why(why, why_cap, "요청 헤더가 너무 큽니다 (최대 8 KiB)");
            return 431;
        }
        n = recv_some(fd, head + got, HTTP_MAX_HEADER - got, deadline);
        if (n == -2) {
            set_why(why, why_cap, "요청을 기한 안에 받지 못했습니다");
            return got > 0 ? 408 : -1;
        }
        if (n <= 0) {
            return -1;
        }
        hend = find_header_end(head + (got > 3 ? got - 3 : 0), head + got + (size_t)n);
        got += (size_t)n;
    }

    /* 2) 요청 줄: METHOD SP TARGET SP HTTP/1.x */
    cur = head;
    line = next_line(&cur, hend, &len);
    for (sp1 = 0; sp1 < len && line[sp1] != ' '; sp1++) {
    }
    for (sp2 = sp1 + 1; sp2 < len && line[sp2] != ' '; sp2++) {
    }
    if (sp1 == 0 || sp1 >= sizeof req->method || sp2 >= len || sp2 == sp1 + 1 ||
        len - sp2 - 1 != 8 || memcmp(line + sp2 + 1, "HTTP/1.", 7) != 0 ||
        (line[len - 1] != '0' && line[len - 1] != '1')) {
        set_why(why, why_cap, "요청 줄 형식이 올바르지 않습니다 (예: POST /query HTTP/1.1)");
        return 400;
    }
    for (i = 0; i < sp1; i++) {
        if (!is_token_char((unsigned char)line[i])) {
            set_why(why, why_cap, "요청 줄 형식이 올바르지 않습니다");
            return 400;
        }
    }
    memcpy(req->method, line, sp1);
    /* 요청 대상 전체(쿼리 문자열 포함)는 보이는 ASCII만 허용한다. NUL이 섞이면
       뒤의 문자열 비교가 NUL 앞까지만 보고 "/query"로 오인할 수 있다. */
    for (i = sp1 + 1; i < sp2; i++) {
        if (!is_token_char((unsigned char)line[i])) {
            set_why(why, why_cap, "요청 경로에 허용하지 않는 문자(NUL·제어 문자 등)가 있습니다");
            return 400;
        }
    }
    {
        size_t tlen = sp2 - sp1 - 1;
        const char *q = memchr(line + sp1 + 1, '?', tlen);
        if (q != NULL) {
            tlen = (size_t)(q - (line + sp1 + 1));
        }
        if (tlen == 0 || tlen >= sizeof req->path || line[sp1 + 1] != '/') {
            set_why(why, why_cap, "요청 경로가 올바르지 않습니다");
            return 400;
        }
        memcpy(req->path, line + sp1 + 1, tlen);
    }

    /* 3) 헤더 */
    for (;;) {
        const char *colon, *v, *ve;
        size_t nlen;

        line = next_line(&cur, hend, &len);
        if (len == 0) {
            break;
        }
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)line[i];
            if ((c < 0x20 && c != '\t') || c == 0x7F) {
                set_why(why, why_cap, "헤더에 허용하지 않는 문자(NUL·제어 문자 등)가 있습니다");
                return 400;
            }
        }
        colon = memchr(line, ':', len);
        if (colon == NULL || colon == line) {
            set_why(why, why_cap, "헤더 형식이 올바르지 않습니다");
            return 400;
        }
        nlen = (size_t)(colon - line);
        v = colon + 1;
        ve = line + len;
        while (v < ve && (*v == ' ' || *v == '\t')) {
            v++;
        }
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
            ve--;
        }
        if (nlen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
            long long n = 0;
            const char *p;
            if (v == ve) {
                set_why(why, why_cap, "Content-Length 값이 올바르지 않습니다");
                return 400;
            }
            for (p = v; p < ve; p++) {
                if (*p < '0' || *p > '9') {
                    set_why(why, why_cap, "Content-Length 값이 올바르지 않습니다");
                    return 400;
                }
                if (n <= HTTP_MAX_BODY) {
                    n = n * 10 + (*p - '0');
                }
            }
            if (content_length >= 0 && content_length != n) {
                set_why(why, why_cap, "Content-Length가 서로 다르게 여러 번 왔습니다");
                return 400;
            }
            content_length = n;
        } else if (nlen == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
            set_why(why, why_cap, "Transfer-Encoding(chunked 등)은 지원하지 않습니다. Content-Length로 보내 주세요");
            return 400;
        } else if (nlen == 6 && strncasecmp(line, "Expect", 6) == 0) {
            if ((size_t)(ve - v) == 12 && strncasecmp(v, "100-continue", 12) == 0) {
                expect_continue = 1;
            }
        }
    }
    if (content_length > HTTP_MAX_BODY) {
        set_why(why, why_cap, "요청 본문이 너무 큽니다 (최대 1 MiB)");
        return 413;
    }
    if (content_length < 0) {
        content_length = 0; /* 길이 없는 요청은 본문 없음 */
    }

    /* 4) 본문: 헤더와 함께 이미 받은 부분 + 나머지 */
    req->body = malloc((size_t)content_length + 1);
    if (req->body == NULL) {
        set_why(why, why_cap, "메모리가 부족합니다");
        return 500;
    }
    {
        size_t have = got - (size_t)(hend - head);
        size_t want = (size_t)content_length;
        if (have > want) {
            have = want; /* 한 연결에 요청 하나: 넘치는 바이트는 버린다 */
        }
        memcpy(req->body, hend, have);
        if (have < want && expect_continue) {
            static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
            if (send_all(fd, cont, sizeof cont - 1, deadline) != 0) {
                http_request_free(req);
                return -1;
            }
        }
        while (have < want) {
            long n = recv_some(fd, req->body + have, want - have, deadline);
            if (n == -2) {
                http_request_free(req);
                set_why(why, why_cap, "요청 본문을 기한 안에 받지 못했습니다");
                return 408;
            }
            if (n <= 0) {
                http_request_free(req);
                return -1;
            }
            have += (size_t)n;
        }
        req->body[want] = '\0';
        req->body_len = want;
    }
    return 0;
}

void http_request_free(HttpRequest *req) {
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

int http_send_response(int fd, int status, const char *extra_headers, const char *const parts[],
                       const size_t lens[], int nparts) {
    char head[512];
    size_t total = 0;
    long long deadline;
    int i, n;

    for (i = 0; i < nparts; i++) {
        total += lens[i];
    }
    n = snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: application/json; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "%s\r\n",
                 status, http_reason(status), total, extra_headers != NULL ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof head) {
        return -1;
    }
    if (set_nonblocking(fd) != 0) {
        return -1;
    }
    deadline = now_ms() + HTTP_SEND_DEADLINE_MS;
    if (send_all(fd, head, (size_t)n, deadline) != 0) {
        return -1;
    }
    for (i = 0; i < nparts; i++) {
        if (send_all(fd, parts[i], lens[i], deadline) != 0) {
            return -1;
        }
    }
    return 0;
}

void http_close(int fd) {
    char sink[4096];
    long long deadline = now_ms() + 1000;
    size_t drained = 0;

    shutdown(fd, SHUT_WR);
    while (drained < 2 * HTTP_MAX_BODY) {
        long n = recv_some(fd, sink, sizeof sink, deadline);
        if (n <= 0) {
            break;
        }
        drained += (size_t)n;
    }
    close(fd);
}
