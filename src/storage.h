#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/*
 * Storage: users.tsv 파일 열기, 순차 읽기, 추가, 다시 쓰기(UPDATE·DELETE), 다음 ID 관리,
 * id 인덱스(B+ 트리) 관리. SQL 문법은 알지 않는다.
 * 파일 형식: 헤더 없이 한 줄에 "id<TAB>name<TAB>age<LF>". id는 증가 순서.
 * users.next: 가장 큰 id의 행을 지웠을 때 그 id를 남기는 파일(Phase 4). 없으면 파일의 최대 id만 본다.
 * 인덱스: id → 그 줄이 시작하는 바이트 위치. 메모리에만 두고 table_open마다 파일에서 다시 만든다.
 */

typedef struct Table Table; /* 불투명 타입. 내부는 storage.c에만 있다 */

typedef struct {
    int64_t id;
    const char *name; /* 콜백이 실행되는 동안만 유효. 보관하려면 복사한다 */
    int age;
} Row;

/* 0을 반환하면 계속, 0이 아니면 순회를 멈추고 table_scan이 -1을 반환한다. */
typedef int (*RowFn)(const Row *row, void *ctx);

/* 반환 규칙: 0 성공, -1 실패(err에 원인) */

/* data_dir/users.tsv를 연다. 없으면 만든다(data_dir 한 단계 포함).
 * 기존 레코드 전체를 한 번 검증하고 최대 ID를 복원하며, id 인덱스를 만든다. */
int table_open(const char *data_dir, Table **out, DbError *err);

/*
 * 다음 ID(최대 ID + 1)를 부여해 레코드 한 건을 파일 끝에 추가하고 인덱스에 등록한다.
 * 실패하면 err->kind로 어디서 실패했는지 구분한다.
 *   DBE_STORAGE      파일을 바꾸기 전 실패. *out_id = 0
 *   DBE_WRITE_FAILED 쓰기 도중 실패. 그 줄이 기록됐는지 알 수 없다. *out_id = 0
 *   DBE_INDEX_FAILED 파일 기록 완료 뒤 인덱스 등록 실패(메모리 부족). *out_id = 기록된 id
 */
int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err);

/* 마지막으로 부여한 id(빈 테이블이면 0). 시작 시 max(파일의 최대 id, users.next)로 복원되고
 * table_append만 늘린다. 다음 INSERT는 이 값 + 1을 받는다. */
int64_t table_last_id(const Table *t);

/* ---------- 다시 쓰기 (Phase 4: UPDATE·DELETE) ---------- */

/* 행이 대상이면 0이 아닌 값 */
typedef int (*RowPred)(const Row *row, void *ctx);

/* UPDATE가 바꿀 값. set_name·set_age가 0인 컬럼은 그대로 둔다 */
typedef struct {
    int set_name;
    const char *name;
    int set_age;
    int age;
} RowUpdate;

/*
 * match가 참인 행을 고치거나(upd != NULL) 지운다(upd == NULL). *out_count = 조건에 맞은 행 수.
 * 파일 전체를 data_dir/users.tsv.tmp에 새로 쓰고 users.tsv와 바꿔치기한 뒤 인덱스를 새로 만든 것으로 바꾼다.
 * 파일은 계속 id 증가 순서다. 가장 큰 id의 행을 지우면 data_dir/users.next에 그 id를 남겨
 * 다음 실행에서도 그 id를 다시 쓰지 않는다. 0행이어도 다시 쓰므로 호출자가 먼저 대상이 있는지 본다.
 *   DBE_STORAGE       바꿔치기 전 실패. 파일·인덱스·다음 id 그대로, 임시 파일은 지움. *out_count = 0
 *   DBE_REOPEN_FAILED Windows 전용. 바꿔치기 뒤 파일을 다시 열지 못함. 파일은 온전하지만 이 Table로는
 *                     더 쓸 수 없다(db.c가 중단 상태로 둔다)
 * 호출자가 배타 잠금을 잡고 부른다.
 */
int table_rewrite(Table *t, RowPred match, void *match_ctx, const RowUpdate *upd, int64_t *out_count,
                  DbError *err);

/* 저장 순서대로 레코드를 하나씩 읽어 fn을 호출한다. fn이 NULL이면 검증만 한다. */
int table_scan(Table *t, RowFn fn, void *ctx, DbError *err);

/* id 인덱스로 레코드 한 건을 찾는다. 있으면 그 위치의 한 줄만 읽어 fn을 한 번 호출하고,
 * 없으면 파일을 읽지 않고 fn 없이 0을 반환한다. 읽은 줄의 id가 다르면 -1(인덱스 불일치). */
int table_find_by_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err);

/* id 인덱스로 lo ≤ id ≤ hi인 레코드를 id 오름차순으로 찾아 하나씩 fn을 호출한다(양 끝 포함).
 * lo > hi거나 범위에 키가 없으면 파일을 읽지 않고 0. 범위 밖의 줄은 읽지 않는다.
 * 읽은 줄의 id가 인덱스와 다르면 -1(인덱스 불일치). fn이 0이 아니면 멈추고 -1. */
int table_find_id_range(Table *t, int64_t lo, int64_t hi, RowFn fn, void *ctx, DbError *err);

/*
 * 커서 페이지 조회 (Phase 5, DB-P5-D02·D07). id > after_id인 레코드를 id 오름차순으로 최대 limit개 찾아
 * 하나씩 fn을 호출한다. limit+1번째 키가 인덱스에 있으면 *has_more = 1, 없으면 0.
 * limit+1번째 키를 확인하면 그 줄은 읽지 않고 멈춘다. 이 멈춤은 오류가 아니다(정상 조기 종료).
 * 파일에서 읽는 줄은 min(limit, 남은 행)개다.
 * after_id가 INT64_MAX면 키를 보지 않고 행 없이 0(*has_more = 0).
 * 0 성공, -1 실패: limit < 1이거나 after_id < 0이면 DBE_SQL. 읽기 오류·인덱스 불일치는 DBE_STORAGE,
 * fn이 0이 아닌 값을 돌려주면 DBE_SINK(table_find_id_range와 같다).
 */
int table_page(Table *t, int64_t after_id, int64_t limit, RowFn fn, void *ctx, int *has_more, DbError *err);

/*
 * 인덱스 통계 (실행 계획용). 메모리의 B+ 트리에서 바로 읽는다.
 * table_open이 시작 시 파일 전체를 읽어 인덱스를 만든 뒤로는, 이 함수들이 파일을 추가로 읽지 않는다.
 * 실제로 조회를 실행해서 잰 값이 아니라 "지금 실행하면 얼마나 읽게 될지"의 근거다.
 */
int table_index_height(const Table *t); /* B+ 트리 높이 = id 조회 한 번이 방문하는 노드 수. 빈 테이블도 1 */
int64_t table_row_count(const Table *t); /* 전체 행 수 = 인덱스 키 수 = 선형 탐색이 읽는 줄 수 */

/* 인덱스 모양과 메모리 실측 (P2-04 측정 보고용). 노드를 모두 훑으므로 O(노드 수). 파일은 읽지 않는다. */
typedef struct {
    int height;
    int64_t keys;
    size_t leaves;
    size_t internals;
    size_t bytes; /* 트리에 요청한 바이트 합 (malloc 관리 오버헤드 제외) */
} IndexStats;

void table_index_stats(const Table *t, IndexStats *out);

#ifdef MINIDB_TESTING
/* 테스트 전용: 1이면 다음 table_append가 줄의 앞부분만 쓰고 DBE_WRITE_FAILED로 실패한다(한 번). */
void table_test_fail_write(int on);
/* 테스트 전용: 다음 table_rewrite 한 번이 그 단계에서 실패한다 */
enum {
    REWRITE_FAIL_NONE = 0,
    REWRITE_FAIL_WRITE,  /* 임시 파일에 첫 행을 쓴 직후 (임시 파일에 내용이 남은 상태) */
    REWRITE_FAIL_NEXT,   /* users.next.tmp 쓰기 */
    REWRITE_FAIL_RENAME, /* users.tsv.tmp → users.tsv 바꿔치기 */
    REWRITE_FAIL_REOPEN  /* Windows 전용: 바꿔치기 뒤 다시 열기 */
};
void table_test_fail_rewrite(int stage);
/* 이 스레드에서 마지막으로 부른 table_page가 파일에서 읽은 줄 수 (조기 종료 확인용) */
int64_t table_test_page_lines_read(void);
/* 같은 호출에서 인덱스 리프에서 받은 키 수 = min(limit + 1, 남은 행). 키를 끝까지 훑지 않는지 확인용 */
int64_t table_test_page_keys_seen(void);
/* 테스트 전용: 메모리 인덱스의 불변식 검사(bpt_validate). 0이면 정상 */
int table_test_validate_index(const Table *t, char *why, size_t cap);
#endif

/* 파일을 닫고 메모리를 해제한다. 실패해도 t는 해제된다. t가 NULL이면 0. */
int table_close(Table *t, DbError *err);

#endif
