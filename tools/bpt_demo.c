/*
 * B+ 트리 관찰 도구 (학습용). SQL·파일 없이 트리 모듈만 사용한다.
 *
 *   bpt_demo [--shuffle] 차수 개수 [찾을 id ...]
 *
 * 1..개수를 차례로(또는 --shuffle이면 섞어서) 넣은 뒤 트리 모양을 출력하고,
 * 찾을 id를 주면 루트에서 리프까지 내려가는 경로를 보여 준다.
 * 값은 실제 파일 위치 대신 id × 100을 넣는다.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bptree.h"

static int parse_int(const char *s, long long lo, long long hi, long long *out) {
    char *end;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = v;
    return 0;
}

static void usage(void) {
    fprintf(stderr, "사용법: bpt_demo [--shuffle] 차수(%d~%d) 개수(0~10000) [찾을 id ...]\n",
            BPT_MIN_ORDER, BPT_MAX_ORDER);
}

int main(int argc, char **argv) {
    int argi = 1;
    int shuffle = 0;
    long long order;
    long long n;
    int64_t *keys;
    BpTree *t;
    long long i;
    char why[128];

    if (argi < argc && strcmp(argv[argi], "--shuffle") == 0) {
        shuffle = 1;
        argi++;
    }
    if (argc - argi < 2 || parse_int(argv[argi], BPT_MIN_ORDER, BPT_MAX_ORDER, &order) != 0 ||
        parse_int(argv[argi + 1], 0, 10000, &n) != 0) {
        usage();
        return 2;
    }
    argi += 2;

    keys = malloc((size_t)(n > 0 ? n : 1) * sizeof *keys);
    t = bpt_create((int)order);
    if (keys == NULL || t == NULL) {
        fprintf(stderr, "메모리가 부족합니다\n");
        free(keys);
        bpt_destroy(t);
        return 1;
    }
    for (i = 0; i < n; i++) {
        keys[i] = i + 1;
    }
    if (shuffle) {
        srand(12345); /* 매번 같은 순서가 나오게 고정 */
        for (i = n - 1; i > 0; i--) {
            long long j = rand() % (i + 1);
            int64_t tmp = keys[i];
            keys[i] = keys[j];
            keys[j] = tmp;
        }
        printf("넣는 순서:");
        for (i = 0; i < n; i++) {
            printf(" %" PRId64, keys[i]);
        }
        printf("\n");
    }
    for (i = 0; i < n; i++) {
        if (bpt_insert(t, keys[i], keys[i] * 100) != BPT_OK) {
            fprintf(stderr, "삽입 실패: %" PRId64 "\n", keys[i]);
            free(keys);
            bpt_destroy(t);
            return 1;
        }
    }
    bpt_dump(t, stdout);
    if (bpt_validate(t, why, sizeof why) != 0) {
        printf("불변식 위반: %s\n", why);
    }
    for (; argi < argc; argi++) {
        long long key;
        if (parse_int(argv[argi], INT64_MIN, INT64_MAX, &key) != 0) {
            fprintf(stderr, "찾을 id가 정수가 아닙니다: %s\n", argv[argi]);
            continue;
        }
        printf("\n");
        bpt_trace_search(t, (int64_t)key, stdout);
    }
    free(keys);
    bpt_destroy(t);
    return 0;
}
