#include "bptree.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/*
 * 노드 하나의 모양 (차수 m = 4, 키 최대 3개인 예)
 *
 *   내부 노드:  children[0] | keys[0] | children[1] | keys[1] | children[2]
 *               (키 < 10)      10       (10 ≤ 키 < 20)   20     (20 ≤ 키)
 *
 *   리프 노드:  keys[i] ↔ values[i] 쌍,  next → 오른쪽 리프
 *
 * 구분 키 규칙: keys[i]는 children[i+1] 아래에 있는 가장 작은 키 이하다.
 * 그래서 찾는 키 k는 "k 이하인 구분 키의 개수"번째 자식으로 내려간다.
 *
 * 삽입은 일단 노드에 넣고, 키가 m개(한 칸 초과)가 되면 둘로 나눈다(split).
 * 그래서 배열은 m-1이 아니라 m칸(자식은 m+1칸)을 잡아 둔다.
 */

#define BPT_MAX_DEPTH 64 /* 차수 3 이상이면 2^63개 키도 높이 64를 넘지 않는다 */

typedef struct BptNode BptNode;

struct BptNode {
    int is_leaf;
    int nkeys;
    int64_t *keys;      /* 용량 order (분할 직전 한 칸 초과 허용) */
    int64_t *values;    /* 리프만: 용량 order */
    BptNode **children; /* 내부만: 용량 order + 1 */
    BptNode *next;      /* 리프만: 오른쪽 리프. 마지막 리프는 NULL */
};

struct BpTree {
    int order;    /* 자식 최대 수 m */
    int max_keys; /* m - 1 */
    int height;
    size_t size;
    BptNode *root;
};

/* ---------- 메모리 할당 (테스트에서 실패를 주입할 수 있게 한곳으로 모은다) ---------- */

#ifdef MINIDB_TESTING
static long g_fail_after = -1;
void bpt_test_fail_alloc_after(long n) { g_fail_after = n; }
static int alloc_should_fail(void) {
    if (g_fail_after < 0) {
        return 0;
    }
    if (g_fail_after == 0) {
        return 1;
    }
    g_fail_after--;
    return 0;
}
#define ALLOC_FAIL() alloc_should_fail()
#else
#define ALLOC_FAIL() 0
#endif

static void *bpt_malloc(size_t n) { return ALLOC_FAIL() ? NULL : malloc(n); }
static void *bpt_calloc(size_t n, size_t size) { return ALLOC_FAIL() ? NULL : calloc(n, size); }

/* ---------- 노드 생성·해제 ---------- */

static void node_free(BptNode *n) {
    if (n == NULL) {
        return;
    }
    free(n->keys);
    free(n->values);
    free(n->children);
    free(n);
}

static BptNode *node_new(int order, int is_leaf) {
    BptNode *n = bpt_calloc(1, sizeof *n);

    if (n == NULL) {
        return NULL;
    }
    n->is_leaf = is_leaf;
    n->keys = bpt_malloc((size_t)order * sizeof *n->keys);
    if (is_leaf) {
        n->values = bpt_malloc((size_t)order * sizeof *n->values);
    } else {
        n->children = bpt_malloc((size_t)(order + 1) * sizeof *n->children);
    }
    if (n->keys == NULL || (is_leaf ? (void *)n->values : (void *)n->children) == NULL) {
        node_free(n);
        return NULL;
    }
    return n;
}

static void subtree_free(BptNode *n) {
    int i;

    if (n == NULL) {
        return;
    }
    if (!n->is_leaf) {
        for (i = 0; i <= n->nkeys; i++) {
            subtree_free(n->children[i]);
        }
    }
    node_free(n);
}

/* ---------- 노드 안 위치 찾기 (이진 탐색) ---------- */

/* key 이상인 첫 키의 위치. 리프에서 key가 있을 자리를 찾는다. */
static int lower_bound(const BptNode *n, int64_t key) {
    int lo = 0;
    int hi = n->nkeys;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (n->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* key 이하인 구분 키의 개수 = 내려갈 자식 번호. */
static int child_index(const BptNode *n, int64_t key) {
    int lo = 0;
    int hi = n->nkeys;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (n->keys[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* ---------- 공개 함수: 생성·해제·조회 ---------- */

BpTree *bpt_create(int order) {
    BpTree *t;

    if (order < BPT_MIN_ORDER || order > BPT_MAX_ORDER) {
        return NULL;
    }
    t = bpt_calloc(1, sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->order = order;
    t->max_keys = order - 1;
    t->height = 1;
    t->root = node_new(order, 1); /* 빈 트리도 빈 리프 하나를 루트로 둔다 */
    if (t->root == NULL) {
        free(t);
        return NULL;
    }
    return t;
}

void bpt_destroy(BpTree *t) {
    if (t == NULL) {
        return;
    }
    subtree_free(t->root);
    free(t);
}

int bpt_search(const BpTree *t, int64_t key, int64_t *out_value) {
    const BptNode *n = t->root;
    int pos;

    /* 루트에서 리프까지 높이만큼만 내려간다: O(log N) */
    while (!n->is_leaf) {
        n = n->children[child_index(n, key)];
    }
    pos = lower_bound(n, key);
    if (pos < n->nkeys && n->keys[pos] == key) {
        if (out_value != NULL) {
            *out_value = n->values[pos];
        }
        return 1;
    }
    return 0;
}

size_t bpt_size(const BpTree *t) { return t->size; }
int bpt_height(const BpTree *t) { return t->height; }
int bpt_order(const BpTree *t) { return t->order; }

/* ---------- 삽입과 분할 ---------- */

/*
 * 리프 분할: 넘친 리프(키 m개)의 뒤쪽 절반을 right로 옮긴다.
 * 부모에 올라갈 키는 right의 첫 키를 "복사"한 것이다(리프에는 그대로 남는다).
 */
static int64_t split_leaf(BptNode *left, BptNode *right) {
    int keep = (left->nkeys + 1) / 2;
    int move = left->nkeys - keep;

    memcpy(right->keys, left->keys + keep, (size_t)move * sizeof *right->keys);
    memcpy(right->values, left->values + keep, (size_t)move * sizeof *right->values);
    right->nkeys = move;
    left->nkeys = keep;
    right->next = left->next; /* 리프 연결 리스트에 끼워 넣기 */
    left->next = right;
    return right->keys[0];
}

/*
 * 내부 노드 분할: 가운데 키는 부모로 "이동"한다(어느 쪽에도 남지 않는다).
 *   keys:     k0 .. k(mid-1) | k(mid) | k(mid+1) .. k(n-1)
 *             └── left ───┘   위로   └──── right ────┘
 */
static int64_t split_internal(BptNode *left, BptNode *right) {
    int n = left->nkeys;
    int mid = n / 2;
    int64_t up = left->keys[mid];

    right->nkeys = n - mid - 1;
    memcpy(right->keys, left->keys + mid + 1, (size_t)right->nkeys * sizeof *right->keys);
    memcpy(right->children, left->children + mid + 1,
           (size_t)(right->nkeys + 1) * sizeof *right->children);
    left->nkeys = mid;
    return up;
}

int bpt_insert(BpTree *t, int64_t key, int64_t value) {
    BptNode *path[BPT_MAX_DEPTH]; /* 루트부터 리프 직전까지 지나온 내부 노드 */
    int slot[BPT_MAX_DEPTH];      /* 각 내부 노드에서 내려간 자식 번호 */
    BptNode *spare_internal[BPT_MAX_DEPTH + 1];
    BptNode *spare_leaf = NULL;
    int n_internal = 0;
    int used_internal = 0;
    int depth = 0;
    BptNode *leaf = t->root;
    BptNode *right;
    int64_t up_key;
    int pos;
    int d;

    /* 1) 키가 들어갈 리프까지 내려가며 경로를 기억한다. */
    while (!leaf->is_leaf) {
        if (depth >= BPT_MAX_DEPTH) {
            return BPT_NOMEM; /* 차수 3 이상에서는 도달할 수 없다 */
        }
        path[depth] = leaf;
        slot[depth] = child_index(leaf, key);
        leaf = leaf->children[slot[depth]];
        depth++;
    }
    pos = lower_bound(leaf, key);
    if (pos < leaf->nkeys && leaf->keys[pos] == key) {
        return BPT_DUPLICATE;
    }

    /*
     * 2) 분할이 몇 번 일어날지 미리 세고 새 노드를 먼저 모두 확보한다.
     *    리프가 꽉 찼으면 리프가 분할되고, 그 위로 꽉 찬 부모가 이어지는 만큼 연쇄 분할된다.
     *    루트까지 꽉 찼으면 새 루트가 하나 더 필요하다.
     *    중간에 메모리가 부족해 트리가 반쯤 바뀐 채로 남는 일을 막기 위해서다.
     */
    if (leaf->nkeys == t->max_keys) {
        for (d = depth - 1; d >= 0 && path[d]->nkeys == t->max_keys; d--) {
            n_internal++;
        }
        if (d < 0) {
            n_internal++; /* 새 루트 */
        }
        spare_leaf = node_new(t->order, 1);
        if (spare_leaf == NULL) {
            return BPT_NOMEM;
        }
        for (d = 0; d < n_internal; d++) {
            spare_internal[d] = node_new(t->order, 0);
            if (spare_internal[d] == NULL) {
                while (d-- > 0) {
                    node_free(spare_internal[d]);
                }
                node_free(spare_leaf);
                return BPT_NOMEM;
            }
        }
    }

    /* 3) 리프에 끼워 넣는다. 여기부터는 실패하지 않는다. */
    memmove(leaf->keys + pos + 1, leaf->keys + pos, (size_t)(leaf->nkeys - pos) * sizeof *leaf->keys);
    memmove(leaf->values + pos + 1, leaf->values + pos,
            (size_t)(leaf->nkeys - pos) * sizeof *leaf->values);
    leaf->keys[pos] = key;
    leaf->values[pos] = value;
    leaf->nkeys++;
    t->size++;
    if (leaf->nkeys <= t->max_keys) {
        return BPT_OK;
    }

    /* 4) 넘쳤으면 분할하고, 부모에 (구분 키, 새 오른쪽 노드)를 올려 보낸다. */
    right = spare_leaf;
    up_key = split_leaf(leaf, right);
    for (d = depth - 1; d >= 0; d--) {
        BptNode *parent = path[d];
        int c = slot[d];

        memmove(parent->keys + c + 1, parent->keys + c,
                (size_t)(parent->nkeys - c) * sizeof *parent->keys);
        memmove(parent->children + c + 2, parent->children + c + 1,
                (size_t)(parent->nkeys - c) * sizeof *parent->children);
        parent->keys[c] = up_key;
        parent->children[c + 1] = right;
        parent->nkeys++;
        if (parent->nkeys <= t->max_keys) {
            return BPT_OK;
        }
        right = spare_internal[used_internal++];
        up_key = split_internal(parent, right);
    }

    /* 5) 루트까지 분할됐다: 새 루트를 만들고 트리 높이가 1 늘어난다. */
    {
        BptNode *root = spare_internal[used_internal++];
        root->keys[0] = up_key;
        root->children[0] = t->root;
        root->children[1] = right;
        root->nkeys = 1;
        t->root = root;
        t->height++;
    }
    return BPT_OK;
}

/* ---------- 학습·디버깅용 출력 ---------- */

static void print_keys(const BptNode *n, FILE *out) {
    int i;

    fputc('[', out);
    for (i = 0; i < n->nkeys; i++) {
        fprintf(out, i == 0 ? "%" PRId64 : " %" PRId64, n->keys[i]);
    }
    fputc(']', out);
}

static void print_level(const BptNode *n, int level, FILE *out) {
    int i;

    if (level == 0) {
        fputc(' ', out);
        print_keys(n, out);
        return;
    }
    for (i = 0; i <= n->nkeys; i++) {
        print_level(n->children[i], level - 1, out);
    }
}

static const BptNode *leftmost_leaf(const BptNode *n) {
    while (!n->is_leaf) {
        n = n->children[0];
    }
    return n;
}

void bpt_dump(const BpTree *t, FILE *out) {
    const BptNode *leaf;
    int level;

    fprintf(out, "B+ 트리: 차수 %d (노드당 키 최대 %d개), 키 %zu개, 높이 %d\n", t->order,
            t->max_keys, t->size, t->height);
    for (level = 0; level < t->height; level++) {
        const char *tag = level == 0 ? (t->height == 1 ? "루트=리프" : "루트")
                          : level == t->height - 1 ? "리프"
                                                   : "내부";
        fprintf(out, "  깊이 %d (%s):", level, tag);
        print_level(t->root, level, out);
        fputc('\n', out);
    }
    fprintf(out, "  리프 연결:");
    for (leaf = leftmost_leaf(t->root); leaf != NULL; leaf = leaf->next) {
        fputc(' ', out);
        print_keys(leaf, out);
        if (leaf->next != NULL) {
            fprintf(out, " →");
        }
    }
    fputc('\n', out);
}

int bpt_trace_search(const BpTree *t, int64_t key, FILE *out) {
    const BptNode *n = t->root;
    int level = 0;
    int pos;

    fprintf(out, "id %" PRId64 " 찾기\n", key);
    while (!n->is_leaf) {
        int c = child_index(n, key);
        fprintf(out, "  깊이 %d 내부 ", level);
        print_keys(n, out);
        if (c == 0) {
            fprintf(out, ": %" PRId64 " < %" PRId64, key, n->keys[0]);
        } else if (c == n->nkeys) {
            fprintf(out, ": %" PRId64 " ≤ %" PRId64, n->keys[c - 1], key);
        } else {
            fprintf(out, ": %" PRId64 " ≤ %" PRId64 " < %" PRId64, n->keys[c - 1], key, n->keys[c]);
        }
        fprintf(out, " → 자식 %d번으로\n", c);
        n = n->children[c];
        level++;
    }
    pos = lower_bound(n, key);
    fprintf(out, "  깊이 %d 리프 ", level);
    print_keys(n, out);
    if (pos < n->nkeys && n->keys[pos] == key) {
        fprintf(out, ": 찾음 → 값 %" PRId64 "\n", n->values[pos]);
        return 1;
    }
    fprintf(out, ": 없음\n");
    return 0;
}

/* ---------- 불변식 검사 (테스트용) ---------- */

typedef struct {
    const BpTree *t;
    const BptNode *prev_leaf;
    size_t count;
    char *why;
    size_t cap;
} Check;

static int fail(Check *c, const char *msg) {
    if (c->why != NULL && c->cap > 0) {
        snprintf(c->why, c->cap, "%s", msg);
    }
    return -1;
}

/* n 아래의 모든 키는 [lo, hi) 안에 있어야 한다. has_lo/has_hi가 0이면 그쪽은 무한대. */
static int check_node(Check *c, const BptNode *n, int level, int64_t lo, int has_lo, int64_t hi,
                      int has_hi) {
    const BpTree *t = c->t;
    int is_root = n == t->root;
    int i;

    if (n->nkeys > t->max_keys) {
        return fail(c, "노드의 키가 최대 개수를 넘음");
    }
    for (i = 0; i < n->nkeys; i++) {
        if (i > 0 && n->keys[i - 1] >= n->keys[i]) {
            return fail(c, "노드 안 키가 오름차순이 아님");
        }
        if ((has_lo && n->keys[i] < lo) || (has_hi && n->keys[i] >= hi)) {
            return fail(c, "키가 부모 구분 키의 범위를 벗어남");
        }
    }
    if (n->is_leaf) {
        if (level != t->height - 1) {
            return fail(c, "리프의 깊이가 서로 다름");
        }
        if (!is_root && n->nkeys < t->order / 2) {
            return fail(c, "리프가 최소 채움보다 적음");
        }
        if (c->prev_leaf != NULL && c->prev_leaf->next != n) {
            return fail(c, "리프 연결 순서가 트리 순서와 다름");
        }
        if (c->prev_leaf == NULL && n != leftmost_leaf(t->root)) {
            return fail(c, "첫 리프가 가장 왼쪽 리프가 아님");
        }
        c->prev_leaf = n;
        c->count += (size_t)n->nkeys;
        return 0;
    }
    if (level >= t->height - 1) {
        return fail(c, "내부 노드가 리프 깊이에 있음");
    }
    if (n->nkeys < 1 || (!is_root && n->nkeys < (t->order + 1) / 2 - 1)) {
        return fail(c, "내부 노드가 최소 채움보다 적음");
    }
    for (i = 0; i <= n->nkeys; i++) {
        int64_t clo = i == 0 ? lo : n->keys[i - 1];
        int chas_lo = i == 0 ? has_lo : 1;
        int64_t chi = i == n->nkeys ? hi : n->keys[i];
        int chas_hi = i == n->nkeys ? has_hi : 1;

        if (n->children[i] == NULL) {
            return fail(c, "내부 노드의 자식이 NULL");
        }
        if (check_node(c, n->children[i], level + 1, clo, chas_lo, chi, chas_hi) != 0) {
            return -1;
        }
    }
    return 0;
}

int bpt_validate(const BpTree *t, char *why, size_t cap) {
    Check c;

    c.t = t;
    c.prev_leaf = NULL;
    c.count = 0;
    c.why = why;
    c.cap = cap;
    if (t == NULL || t->root == NULL) {
        return fail(&c, "트리가 없음");
    }
    if (check_node(&c, t->root, 0, 0, 0, 0, 0) != 0) {
        return -1;
    }
    if (c.prev_leaf->next != NULL) {
        return fail(&c, "마지막 리프의 next가 NULL이 아님");
    }
    if (c.count != t->size) {
        return fail(&c, "리프의 키 개수 합이 size와 다름");
    }
    return 0;
}
