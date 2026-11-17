/*
 * 부하 생성기 (Phase 3 / P3-05, POSIX 전용). tools/load.py가 부른다.
 *
 * 사용법: loadgen 포트 종류 클라이언트수 초 행수
 *   종류: id    SELECT ... WHERE id = 무작위(1..행수)
 *         mix   id 조회 80%, id 범위(21행) 10%, INSERT 10%
 *         scan  SELECT ... WHERE age = 150 (선형 탐색)
 *
 * 클라이언트마다 스레드 하나. 응답을 받자마자 다음 요청을 보낸다(닫힌 고리).
 * 요청마다 새 연결: 연결 → 요청 보내기 → 응답(헤더 + Content-Length만큼의 본문) 받기. 이 전체 시간을 잰다.
 * 파이썬 클라이언트는 요청마다 쓰는 CPU가 커서 클라이언트가 먼저 병목이 됐다. 그래서 C로 만들었다.
 *
 * 집계 기준 (P3-05 Codex 재검증 반영):
 *   - 완전한 응답만 분류한다: 상태 줄, 헤더 끝(빈 줄), Content-Length, 그 길이만큼의 본문을 모두 받아야 한다.
 *     덜 받은 채 끊기거나(EOF·연결 재설정) 기한(30초)을 넘기면 "불완전"이다. 상태 줄도 못 받았으면 "연결 실패".
 *   - 측정 구간은 시작 시각부터 정확히 '초'만큼이다. 구간 안에서 **완료된** 요청만 센다.
 *     구간이 끝나면 새 요청을 보내지 않고, 이미 보낸 요청은 끝까지 받되(드레인) 집계하지 않고 "늦음"으로만 센다.
 *     그래서 처리량 = 구간 안 성공 수 / 구간 길이이고, 드레인 시간은 들어가지 않는다.
 *   - 백분위는 구간 안에서 완료된 성공(200) 요청만의 응답 시간이다.
 *
 * 출력(한 줄): ok=.. busy=.. other=.. incomplete=.. connfail=.. late=.. window=초 p50=.. p95=.. p99=.. max=.. (ms)
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define IO_TIMEOUT_S 30
#define HEAD_MAX 4096

typedef struct {
    int id;
    double *lat; /* 구간 안에서 완료된 성공 요청의 응답 시간(ms) */
    size_t n, cap;
    long ok, busy, other, incomplete, connfail, late;
    uint64_t rnd;
} Client;

static int g_port, g_kind, g_clients;
static long g_rows;
static double g_seconds, g_start, g_end;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t next_rnd(uint64_t *s) { /* xorshift64 */
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static int make_body(Client *c, long seq, char *buf, size_t cap) {
    long id = 1 + (long)(next_rnd(&c->rnd) % (uint64_t)g_rows);
    unsigned r = (unsigned)(next_rnd(&c->rnd) % 100);

    if (g_kind == 2) {
        return snprintf(buf, cap, "SELECT * FROM users WHERE age = 150;");
    }
    if (g_kind == 1 && r >= 80) {
        if (r < 90) {
            long a = id > g_rows - 20 ? g_rows - 20 : id;
            return snprintf(buf, cap, "SELECT * FROM users WHERE id BETWEEN %ld AND %ld;", a, a + 20);
        }
        return snprintf(buf, cap, "INSERT INTO users (name, age) VALUES ('c%d_%ld', %ld);", c->id, seq, seq % 151);
    }
    return snprintf(buf, cap, "SELECT * FROM users WHERE id = %ld;", id);
}

/* 헤더 블록 [h, h+len)에서 Content-Length 값. 없거나 잘못되면 -1 */
static long content_length(const char *h, size_t len) {
    const char *p = h, *end = h + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = (size_t)((eol ? eol : end) - p);
        if (ll > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v = p + 15;
            long n = 0;
            while (v < p + ll && *v == ' ') {
                v++;
            }
            if (v >= p + ll || *v < '0' || *v > '9') {
                return -1;
            }
            while (v < p + ll && *v >= '0' && *v <= '9') {
                n = n * 10 + (*v++ - '0');
            }
            return n;
        }
        p = eol ? eol + 1 : end;
    }
    return -1;
}

enum { R_COMPLETE, R_INCOMPLETE, R_CONNFAIL };

/* 요청 하나. 완전한 응답이면 R_COMPLETE와 *status. */
static int one_request(const char *req, size_t len, int *status) {
    struct sockaddr_in a;
    struct timeval tv = {IO_TIMEOUT_S, 0};
    struct linger lg = {1, 0};
    char head[HEAD_MAX + 1];
    char buf[65536];
    size_t hlen = 0, sent = 0;
    long clen = -1, body = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0), rc = R_CONNFAIL;
    ssize_t n;

    if (fd < 0) {
        return R_CONNFAIL;
    }
    /* 연결·보내기·받기 모두 기한: 잘못된 서버가 측정기를 붙잡지 못하게 (connect에는 SO_SNDTIMEO가 적용된다) */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return R_CONNFAIL;
    }
    while (sent < len) {
        n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return R_CONNFAIL;
        }
        sent += (size_t)n;
    }
    for (;;) {
        n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            break; /* EOF·연결 재설정·기한 초과: 여기까지 받은 것으로 판정 */
        }
        if (clen < 0) { /* 아직 헤더를 다 받지 못함 */
            size_t take = (size_t)n < HEAD_MAX - hlen ? (size_t)n : HEAD_MAX - hlen;
            char *hend;
            memcpy(head + hlen, buf, take);
            hlen += take;
            head[hlen] = '\0';
            hend = strstr(head, "\r\n\r\n");
            if (hend == NULL) {
                if (hlen == HEAD_MAX) {
                    break; /* 헤더가 너무 김: 불완전으로 본다 */
                }
                continue;
            }
            clen = content_length(head, (size_t)(hend - head));
            if (clen < 0 || hlen < 12 || strncmp(head, "HTTP/1.", 7) != 0) {
                break;
            }
            *status = atoi(head + 9);
            /* 이번 recv에서 헤더 뒤에 온 본문 바이트 */
            body = (long)((size_t)n - ((size_t)(hend + 4 - head) - (hlen - take)));
        } else {
            body += (long)n;
        }
        if (body >= clen) {
            break; /* 본문까지 다 받음 */
        }
    }
    if (clen >= 0 && body == clen) {
        rc = R_COMPLETE;
    } else if (hlen > 0) {
        rc = R_INCOMPLETE;
    }
    /* 응답을 다 받은 뒤(또는 포기한 뒤) RST로 닫는다: 서버 쪽에 TIME_WAIT가 쌓이지 않게(benchmark.md 5).
       정상 FIN 종료 클라이언트와는 다른 조건이다. */
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(fd);
    return rc;
}

static void *client_main(void *arg) {
    Client *c = arg;
    char body[200], req[400];
    long seq = 0;

    while (now_s() < g_start) {
    }
    while (now_s() < g_end) { /* 구간이 끝나면 새 요청을 보내지 않는다 */
        int bl = make_body(c, seq++, body, sizeof body);
        int rl = snprintf(req, sizeof req, "POST /query HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s", bl, body);
        int st = 0;
        double t = now_s();
        int r = one_request(req, (size_t)rl, &st);
        double done = now_s();

        if (done > g_end) { /* 구간이 끝난 뒤 완료: 집계하지 않는다 */
            c->late++;
            break;
        }
        if (r == R_CONNFAIL) {
            c->connfail++;
        } else if (r == R_INCOMPLETE) {
            c->incomplete++;
        } else if (st == 200) {
            if (c->n == c->cap) {
                size_t nc = c->cap ? c->cap * 2 : 4096;
                double *g = realloc(c->lat, nc * sizeof *g);
                if (g == NULL) {
                    break;
                }
                c->lat = g;
                c->cap = nc;
            }
            c->lat[c->n++] = (done - t) * 1000.0;
            c->ok++;
        } else if (st == 503) {
            c->busy++;
        } else {
            c->other++;
        }
    }
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(const double *v, size_t n, double p) {
    size_t k;
    if (n == 0) {
        return -1;
    }
    k = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    return v[k < n ? k : n - 1];
}

int main(int argc, char **argv) {
    Client *cs;
    pthread_t *th;
    double *all;
    size_t total = 0, k = 0;
    long ok = 0, busy = 0, other = 0, incomplete = 0, connfail = 0, late = 0;
    int i;

    if (argc != 6) {
        fprintf(stderr, "사용법: loadgen 포트 id|mix|scan 클라이언트수 초 행수\n");
        return 2;
    }
    g_port = atoi(argv[1]);
    g_kind = strcmp(argv[2], "mix") == 0 ? 1 : strcmp(argv[2], "scan") == 0 ? 2 : 0;
    g_clients = atoi(argv[3]);
    g_seconds = atof(argv[4]);
    g_rows = atol(argv[5]);
    if (g_port <= 0 || g_clients <= 0 || g_seconds <= 0 || g_rows < 21) {
        fprintf(stderr, "잘못된 인자\n");
        return 2;
    }
    cs = calloc((size_t)g_clients, sizeof *cs);
    th = calloc((size_t)g_clients, sizeof *th);
    if (cs == NULL || th == NULL) {
        return 1;
    }
    g_start = now_s() + 0.2; /* 모든 클라이언트가 같은 순간에 시작 */
    g_end = g_start + g_seconds;
    for (i = 0; i < g_clients; i++) {
        cs[i].id = i;
        cs[i].rnd = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1);
        if (pthread_create(&th[i], NULL, client_main, &cs[i]) != 0) {
            fprintf(stderr, "스레드를 만들 수 없습니다\n");
            return 1;
        }
    }
    for (i = 0; i < g_clients; i++) {
        pthread_join(th[i], NULL);
        total += cs[i].n;
        ok += cs[i].ok;
        busy += cs[i].busy;
        other += cs[i].other;
        incomplete += cs[i].incomplete;
        connfail += cs[i].connfail;
        late += cs[i].late;
    }
    all = malloc((total ? total : 1) * sizeof *all);
    if (all == NULL) {
        return 1;
    }
    for (i = 0; i < g_clients; i++) {
        if (cs[i].n > 0) { /* 성공이 0건이면 lat는 NULL이다: memcpy에 NULL을 넘기지 않는다(UBSan) */
            memcpy(all + k, cs[i].lat, cs[i].n * sizeof *all);
            k += cs[i].n;
        }
        free(cs[i].lat);
    }
    qsort(all, total, sizeof *all, cmp_double);
    printf("ok=%ld busy=%ld other=%ld incomplete=%ld connfail=%ld late=%ld window=%.3f "
           "p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
           ok, busy, other, incomplete, connfail, late, g_seconds, pct(all, total, 50), pct(all, total, 95),
           pct(all, total, 99), total ? all[total - 1] : -1.0);
    free(all);
    free(cs);
    free(th);
    return 0;
}
