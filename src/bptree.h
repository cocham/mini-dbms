#ifndef MINIDB_BPTREE_H
#define MINIDB_BPTREE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 메모리 기반 B+ 트리: 정수 키(id) → 정수 값(레코드의 파일 바이트 위치).
 *
 *  - 모든 키·값은 리프에만 있다. 내부 노드는 "어느 자식으로 내려갈지" 안내하는 구분 키만 가진다.
 *  - 리프는 왼쪽에서 오른쪽으로 next 포인터로 이어져 있다(범위 조회용).
 *  - 차수(order) m: 노드 하나가 가질 수 있는 자식 수의 최대값. 키는 최대 m-1개.
 *  - 키 중복은 허용하지 않는다(id는 유일하다).
 *
 * SQL·파일을 모른다. Storage가 이 트리를 "id → 오프셋" 인덱스로 사용한다.
 */

#define BPT_MIN_ORDER 3
#define BPT_MAX_ORDER 1024
#define BPT_DEFAULT_ORDER 64

typedef struct BpTree BpTree; /* 불투명 타입. 내부는 bptree.c에만 있다 */

/* bpt_insert 반환값 */
enum { BPT_OK = 0, BPT_DUPLICATE = 1, BPT_NOMEM = -1 };

/* 빈 트리를 만든다. order가 BPT_MIN_ORDER~BPT_MAX_ORDER 밖이거나 메모리가 없으면 NULL. */
BpTree *bpt_create(int order);

/* 모든 노드를 해제한다. NULL이면 아무것도 하지 않는다. */
void bpt_destroy(BpTree *t);

/*
 * key → value를 넣는다.
 *  BPT_OK        넣음
 *  BPT_DUPLICATE 같은 키가 이미 있음. 트리는 바뀌지 않는다
 *  BPT_NOMEM     메모리 부족. 트리는 바뀌지 않는다(필요한 노드를 먼저 모두 확보한 뒤 수정하므로)
 */
int bpt_insert(BpTree *t, int64_t key, int64_t value);

/* key를 찾으면 1을 반환하고 *out_value에 값을 담는다. 없으면 0. out_value는 NULL이어도 된다. */
int bpt_search(const BpTree *t, int64_t key, int64_t *out_value);

/* bpt_range가 키 하나마다 부르는 콜백. 0이면 계속, 0이 아니면 멈춘다. */
typedef int (*BptVisitFn)(int64_t key, int64_t value, void *ctx);

/*
 * lo ≤ key ≤ hi인 키를 오름차순으로 fn에 넘긴다. lo > hi면 아무것도 하지 않는다.
 * 루트에서는 lo가 있을 리프 하나만 찾고, 그다음은 리프의 next 연결을 따라 옆으로 걷는다: O(log N + K).
 * fn이 0이 아닌 값을 반환하면 바로 멈추고 그 값을 반환한다. 끝까지 가면 0.
 */
int bpt_range(const BpTree *t, int64_t lo, int64_t hi, BptVisitFn fn, void *ctx);

/* 트리가 차지한 노드와 메모리 (측정·보고용) */
typedef struct {
    size_t leaves;    /* 리프 노드 수 */
    size_t internals; /* 내부 노드 수 */
    size_t bytes;     /* 트리·노드·배열에 요청한 바이트 합. malloc 자체의 관리 오버헤드는 제외 */
} BptMemStats;

/* 모든 노드를 한 번 훑어 센다: O(노드 수). */
void bpt_mem_stats(const BpTree *t, BptMemStats *out);

size_t bpt_size(const BpTree *t);   /* 저장된 키 개수 */
int bpt_height(const BpTree *t);    /* 루트에서 리프까지 노드 수. 빈 트리도 리프 하나라 1 */
int bpt_order(const BpTree *t);

/* ---------- 학습·디버깅용 출력 ---------- */

/* 트리를 레벨별로 출력하고 마지막에 리프 연결 순서를 출력한다. */
void bpt_dump(const BpTree *t, FILE *out);

/* key를 찾아 내려가는 경로(방문한 노드와 선택한 자식)를 출력한다. 찾으면 1, 없으면 0. */
int bpt_trace_search(const BpTree *t, int64_t key, FILE *out);

/*
 * 트리 불변식을 모두 검사한다(테스트용). 정상이면 0, 깨졌으면 -1과 why에 원인.
 * 정렬, 구분 키 범위, 모든 리프의 깊이, 노드 최소 채움, 리프 연결, 키 개수.
 */
int bpt_validate(const BpTree *t, char *why, size_t cap);

#ifdef MINIDB_TESTING
/* 테스트 전용: 앞으로 n번의 할당은 성공시키고 그다음부터 실패시킨다. -1이면 해제. */
void bpt_test_fail_alloc_after(long n);
#endif

#endif
