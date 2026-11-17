/*
 * 부하 생성기 (Phase 3 / P3-05, POSIX 전용). tools/load.py가 부른다.
 *
 * 사용법: loadgen 포트 종류 클라이언트수 초 행수
 *   종류: id    SELECT ... WHERE id = 무작위(1..행수)
 *         mix   id 조회 80%, id 범위(21행) 10%, INSERT 10%
 *         scan  SELECT ... WHERE age = 150 (선형 탐색)
 *
 * 클라이언트마다 스레드 하나. 응답을 받자마자 다음 요청을 보낸다(닫힌 고리).
 * 요청마다 새 연결: 연결 → 요청 보내기 → 서버가 닫을 때까지 응답 받기. 이 전체 시간을 잰다.
 * 파이썬 클라이언트는 요청마다 쓰는 CPU가 커서 클라이언트가 먼저 병목이 됐다. 그래서 C로 만들었다.
 *
 * 출력(한 줄): ok=성공 busy=503 reset=끊김 other=기타 p50=.. p95=.. p99=.. max=.. (ms)
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int id;
    double *lat; /* 성공한 요청의 응답 시간(ms) */
    size_t n, cap;
    long ok, busy, reset, other;
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

/* 상태 코드, 연결이 끊기면 -1 */
static int one_request(const char *req, size_t len) {
    struct sockaddr_in a;
    char buf[65536];
    char first[16];
    size_t got_first = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ssize_t n;
    size_t sent = 0;

    if (fd < 0) {
        return -1;
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    while (sent < len) {
        n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    for (;;) {
        n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            /* 0: 서버가 응답을 다 보내고 닫음(정상 끝), <0: 끊김 — 상태 줄을 받았으면 그것으로 판정.
               응답을 끝까지 받은 뒤에는 SO_LINGER 0으로 닫아 연결 재설정(RST)을 보낸다. 그러면 서버와
               클라이언트 어느 쪽에도 TIME_WAIT 연결이 남지 않는다. 서버가 먼저 닫는 구조라 보통은 서버 쪽에
               60초씩 남는데, 초당 수천 개가 쌓여 상한(32,768)에 닿으면 처리량이 절반 가까이 떨어졌다.
               측정하는 일(연결·요청·응답 받기)은 같고, 응답을 다 받은 뒤의 정리 방식만 다르다(실제 클라이언트와 다름). */
            struct linger lg = {1, 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
            break;
        }
        if (got_first < sizeof first - 1) { /* 상태 줄 앞부분("HTTP/1.1 200")만 모아 둔다 */
            size_t k = sizeof first - 1 - got_first;
            if (k > (size_t)n) {
                k = (size_t)n;
            }
            memcpy(first + got_first, buf, k);
            got_first += k;
        }
    }
    close(fd);
    first[got_first] = '\0';
    if (got_first < 12 || strncmp(first, "HTTP/1.", 7) != 0) {
        return -1;
    }
    return atoi(first + 9);
}

static void *client_main(void *arg) {
    Client *c = arg;
    char body[200], req[400];
    long seq = 0;

    while (now_s() < g_start) {
    }
    while (now_s() < g_end) {
        int bl = make_body(c, seq++, body, sizeof body);
        int rl = snprintf(req, sizeof req, "POST /query HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s", bl, body);
        double t = now_s();
        int st = one_request(req, (size_t)rl);
        double took = (now_s() - t) * 1000.0;

        if (st == 200) {
            if (c->n == c->cap) {
                size_t nc = c->cap ? c->cap * 2 : 4096;
                double *g = realloc(c->lat, nc * sizeof *g);
                if (g == NULL) {
                    break;
                }
                c->lat = g;
                c->cap = nc;
            }
            c->lat[c->n++] = took;
            c->ok++;
        } else if (st == 503) {
            c->busy++;
        } else if (st < 0) {
            c->reset++;
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
    long ok = 0, busy = 0, reset = 0, other = 0;
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
        reset += cs[i].reset;
        other += cs[i].other;
    }
    all = malloc((total ? total : 1) * sizeof *all);
    if (all == NULL) {
        return 1;
    }
    for (i = 0; i < g_clients; i++) {
        memcpy(all + k, cs[i].lat, cs[i].n * sizeof *all);
        k += cs[i].n;
        free(cs[i].lat);
    }
    qsort(all, total, sizeof *all, cmp_double);
    printf("ok=%ld busy=%ld reset=%ld other=%ld p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", ok, busy, reset, other,
           pct(all, total, 50), pct(all, total, 95), pct(all, total, 99), total ? all[total - 1] : -1.0);
    free(all);
    free(cs);
    free(th);
    return 0;
}
