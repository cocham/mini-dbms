#ifndef MINIDB_EXECUTOR_H
#define MINIDB_EXECUTOR_H

#include <stdint.h>

#include "common.h"
#include "parser.h"
#include "storage.h"

/*
 * Executor: Statement의 값을 검증하고 Storage에 저장·조회를 요청한다.
 * stdout에 직접 쓰지 않고, 결과는 ResultSink 콜백으로 넘긴다.
 */

/* ---------- 실행 계획 (EXPLAIN) ---------- */

/* SELECT가 탈 조회 경로. 경로를 고르는 곳은 executor_plan 한 곳뿐이다. */
typedef enum {
    PLAN_FULL_SCAN,       /* 파일을 처음부터 끝까지 읽으며 조건 검사 (조건 없음, WHERE age) */
    PLAN_INDEX_LOOKUP,    /* B+ 트리로 바이트 위치를 찾아 그 한 줄만 읽음 (WHERE id = ?) */
    PLAN_INDEX_RANGE_SCAN /* 시작 리프만 트리로 찾고 리프 연결을 따라 범위 안의 줄만 읽음 */
} PlanKind;

/* id 범위를 양 끝 포함 [lo, hi]로 정규화한 것. < <= > >= BETWEEN이 모두 이 형태가 된다. */
typedef struct {
    int64_t lo;
    int64_t hi;
    int empty; /* 1이면 어떤 id도 해당하지 않음 (BETWEEN 20 AND 10, id < INT64_MIN 등) */
} IdRange;

/*
 * 계획 시점 통계: 계획을 만들 때 메모리의 인덱스에서 읽은 값이다.
 * 조회를 실제로 실행해서 잰 값(읽은 줄 수, 걸린 시간 등)이 아니다.
 * 실제 실행 측정값은 P2-04에서 별도 구조체로 다루고 이 구조체와 섞지 않는다.
 */
typedef struct {
    int tree_height;    /* B+ 트리 높이 = INDEX LOOKUP 한 번이 방문할 노드 수 */
    int64_t total_rows; /* 전체 행 수 = FULL SCAN이 읽을 줄 수 */
} PlanStats;

typedef struct {
    PlanKind kind;
    WhereKind where;     /* 계획이 다루는 조건 (Statement에서 복사) */
    int64_t where_value; /* where가 WHERE_NONE이 아닐 때의 비교값. BETWEEN이면 아래쪽 끝 */
    PlanStats stats;     /* 계획 시점 통계. 경로와 상관없이 둘 다 채운다 */
    CmpOp where_op;      /* 사용자가 쓴 비교 형태 (출력용) */
    int64_t where_value2; /* CMP_BETWEEN의 위쪽 끝 (출력용) */
    IdRange range;       /* PLAN_INDEX_RANGE_SCAN일 때 실제로 읽을 범위 */
} QueryPlan;

/*
 * SELECT 문의 실행 계획을 out에 채운다. 조회는 실행하지 않는다.
 * 통계는 시작 시 구축된 인덱스에서 얻으므로, 계획을 만들면서 파일을 추가로 읽지 않는다.
 * 0 성공, -1 실패(SELECT가 아니면 err에 원인).
 */
int executor_plan(const Table *t, const Statement *st, QueryPlan *out, DbError *err);

/* 계획을 사람이 읽는 문구로. CLI(EXPLAIN 한 줄)와 서버(JSON 필드)가 같은 문구를 쓴다.
 *   executor_plan_name:      "INDEX LOOKUP" / "INDEX RANGE SCAN" / "FULL SCAN"
 *   executor_plan_condition: 사용자가 쓴 조건 형태. "id = 3", "id BETWEEN 10 AND 20", "age = 25", "조건 없음" */
const char *executor_plan_name(PlanKind kind);
void executor_plan_condition(const QueryPlan *plan, char *buf, size_t cap);

/* ---------- 실행 ---------- */

typedef struct {
    int (*on_insert)(int64_t id, void *ctx); /* 0이 아니면 실패로 처리 */
    RowFn on_row;
    void *ctx;
    int (*on_plan)(const QueryPlan *plan, void *ctx); /* EXPLAIN 결과. 0이 아니면 실패로 처리 */
} ResultSink;

/*
 * 0 성공, -1 실패(err에 원인). sink나 그 안의 콜백은 NULL이어도 된다.
 * SELECT는 executor_plan이 고른 경로로 실행한다. st->explain이면 계획을 on_plan으로 넘기고
 * 조회는 실행하지 않는다(on_row 호출 없음).
 */
int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err);

#endif
