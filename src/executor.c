#include "executor.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *executor_plan_name(PlanKind kind) {
    switch (kind) {
    case PLAN_INDEX_LOOKUP: return "INDEX LOOKUP";
    case PLAN_INDEX_RANGE_SCAN: return "INDEX RANGE SCAN";
    default: return "FULL SCAN";
    }
}

void executor_plan_condition(const QueryPlan *plan, char *buf, size_t cap) {
    static const char *const ops[] = {"=", "<", "<=", ">", ">="}; /* CmpOp 순서 */

    switch (plan->where) {
    case WHERE_ID: snprintf(buf, cap, "id = %" PRId64, plan->where_value); break;
    case WHERE_AGE: snprintf(buf, cap, "age = %" PRId64, plan->where_value); break;
    case WHERE_ID_RANGE:
        if (plan->where_op == CMP_BETWEEN) {
            snprintf(buf, cap, "id BETWEEN %" PRId64 " AND %" PRId64, plan->where_value, plan->where_value2);
        } else {
            snprintf(buf, cap, "id %s %" PRId64, ops[plan->where_op], plan->where_value);
        }
        break;
    default: snprintf(buf, cap, "조건 없음"); break;
    }
}

static int validate_name(const char *name, DbError *err) {
    size_t n;

    if (name == NULL) {
        db_error_setk(err, DBE_SQL, "name 값이 없습니다");
        return -1;
    }
    n = strlen(name);
    if (n == 0) {
        db_error_setk(err, DBE_SQL, "name은 빈 문자열일 수 없습니다");
        return -1;
    }
    if (n > NAME_MAX_BYTES) {
        db_error_setk(err, DBE_SQL, "name이 %d바이트를 넘습니다 (%u바이트)", NAME_MAX_BYTES, (unsigned)n);
        return -1;
    }
    if (strpbrk(name, "\t\r\n") != NULL) {
        db_error_setk(err, DBE_SQL, "name에 탭이나 줄바꿈을 넣을 수 없습니다");
        return -1;
    }
    return 0;
}

static int validate_age(int age, DbError *err) {
    if (age < AGE_MIN || age > AGE_MAX) {
        db_error_setk(err, DBE_SQL, "age 값 %d이(가) 허용 범위(%d~%d)를 벗어났습니다", age, AGE_MIN, AGE_MAX);
        return -1;
    }
    return 0;
}

static int validate_insert(const Statement *st, DbError *err) {
    return validate_name(st->name, err) != 0 || validate_age(st->age, err) != 0 ? -1 : 0;
}

/* UPDATE의 SET 값: INSERT와 같은 규칙. SET에 없는 컬럼은 검사하지 않는다 */
static int validate_update(const Statement *st, DbError *err) {
    if ((st->set_cols & SET_COL_NAME) && validate_name(st->name, err) != 0) {
        return -1;
    }
    if ((st->set_cols & SET_COL_AGE) && validate_age(st->age, err) != 0) {
        return -1;
    }
    return 0;
}

/*
 * SELECT ... WHERE: 조회 경로는 executor_plan 한 곳에서 고른다.
 *  - WHERE id = 값        → 인덱스 조회: B+ 트리로 위치를 찾아 그 한 줄만 읽는다 (P2-01)
 *  - WHERE id 범위         → 인덱스 범위 조회: 시작 리프만 찾고 리프 연결을 따라간다 (P2-03)
 *  - 그 밖(조건 없음, age) → 선형 탐색: Storage가 레코드를 하나씩 넘겨주면 조건에 맞는 것만 전달
 * Storage는 SQL 조건을 모르고, CLI는 어느 경로로 찾았는지 모른다.
 */
typedef struct {
    const Statement *st;
    RowFn next;     /* 조건을 통과한 레코드를 받을 콜백. NULL이면 버린다 */
    void *next_ctx;
} FilterCtx;

/* id 범위 조건을 양 끝 포함 [lo, hi]로 바꾼다. 64비트 끝에서 ±1이 넘치지 않게 빈 범위로 처리한다. */
static IdRange id_range_of(const Statement *st) {
    IdRange r = {INT64_MIN, INT64_MAX, 0};
    int64_t v = st->where_value;

    switch (st->where_op) {
    case CMP_LT:
        if (v == INT64_MIN) {
            r.empty = 1;
        } else {
            r.hi = v - 1;
        }
        break;
    case CMP_LE: r.hi = v; break;
    case CMP_GT:
        if (v == INT64_MAX) {
            r.empty = 1;
        } else {
            r.lo = v + 1;
        }
        break;
    case CMP_GE: r.lo = v; break;
    case CMP_BETWEEN:
        r.lo = v;
        r.hi = st->where_value2;
        r.empty = r.lo > r.hi; /* SQL 표준: 뒤집힌 BETWEEN은 오류가 아니라 결과 없음 */
        break;
    case CMP_EQ:
        r.lo = v;
        r.hi = v;
        break;
    }
    return r;
}

/* 선형 탐색에서 쓰는 조건 검사. 인덱스 경로와 같은 결과를 내야 한다(테스트에서 비교). */
static int row_matches(const Statement *st, const Row *row) {
    switch (st->where) {
    case WHERE_NONE: return 1;
    case WHERE_ID: return row->id == st->where_value;
    case WHERE_AGE: return (int64_t)row->age == st->where_value;
    case WHERE_ID_RANGE: {
        IdRange r = id_range_of(st);
        return !r.empty && row->id >= r.lo && row->id <= r.hi;
    }
    }
    return 0;
}

static int filter_row(const Row *row, void *ctx) {
    FilterCtx *f = ctx;

    if (!row_matches(f->st, row) || f->next == NULL) {
        return 0;
    }
    return f->next(row, f->next_ctx);
}

/* WHERE에 맞는 행을 찾는 경로. SELECT와 UPDATE·DELETE의 대상 찾기가 함께 쓴다 */
static void build_plan(const Table *t, const Statement *st, QueryPlan *out) {
    /* 경로 선택 기준은 조건의 종류뿐이다. 값(where_value)이나 키 존재 여부는 보지 않는다. */
    switch (st->where) {
    case WHERE_ID: out->kind = PLAN_INDEX_LOOKUP; break;
    case WHERE_ID_RANGE: out->kind = PLAN_INDEX_RANGE_SCAN; break;
    default: out->kind = PLAN_FULL_SCAN; break;
    }
    out->where = st->where;
    out->where_value = st->where_value;
    out->where_op = st->where_op;
    out->where_value2 = st->where_value2;
    out->range = id_range_of(st);
    out->stats.tree_height = table_index_height(t);
    out->stats.total_rows = table_row_count(t);
}

int executor_plan(const Table *t, const Statement *st, QueryPlan *out, DbError *err) {
    if (st->kind != STMT_SELECT) {
        db_error_setk(err, DBE_SQL, "실행 계획은 SELECT 문에만 만들 수 있습니다");
        return -1;
    }
    build_plan(t, st, out);
    return 0;
}

/* plan이 고른 경로로 WHERE에 맞는 행을 하나씩 fn에 넘긴다 */
static int run_where(Table *t, const Statement *st, const QueryPlan *plan, RowFn fn, void *ctx, DbError *err) {
    FilterCtx f;

    switch (plan->kind) {
    case PLAN_INDEX_LOOKUP:
        return table_find_by_id(t, st->where_value, fn, ctx, err);
    case PLAN_INDEX_RANGE_SCAN:
        if (plan->range.empty) {
            return 0; /* 빈 범위: 트리도 파일도 보지 않는다 */
        }
        return table_find_id_range(t, plan->range.lo, plan->range.hi, fn, ctx, err);
    case PLAN_FULL_SCAN:
        f.st = st;
        f.next = fn;
        f.next_ctx = ctx;
        return table_scan(t, filter_row, &f, err);
    }
    db_error_setk(err, DBE_SQL, "알 수 없는 실행 계획입니다");
    return -1;
}

static int count_match(const Row *row, void *ctx) {
    (void)row;
    (*(int64_t *)ctx)++;
    return 0;
}

static int pred_matches(const Row *row, void *ctx) { return row_matches(ctx, row); }

/*
 * UPDATE·DELETE (Phase 4, DB-P4-D01·D06)
 *  1) SET 값 검증(INSERT와 같은 규칙). 실패하면 아무것도 바꾸지 않는다
 *  2) 대상 세기: SELECT와 같은 경로(인덱스 조회·범위 조회·선형 탐색). 0행이면 파일을 다시 쓰지 않는다
 *  3) 파일 다시 쓰기(table_rewrite): 원래 파일 전체를 훑으며 row_matches로 같은 행을 고르고 고치거나 지운다
 * 2와 3이 같은 행을 고르는 것은 row_matches가 인덱스 경로와 같은 결과를 내기 때문이다(테스트에서 비교).
 */
static int run_change(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    QueryPlan plan;
    int64_t n = 0;

    if (st->kind == STMT_UPDATE && validate_update(st, err) != 0) {
        return -1;
    }
    build_plan(t, st, &plan);
    if (run_where(t, st, &plan, count_match, &n, err) != 0) {
        return -1;
    }
    if (n > 0) {
        RowUpdate upd;

        upd.set_name = (st->set_cols & SET_COL_NAME) != 0;
        upd.name = st->name;
        upd.set_age = (st->set_cols & SET_COL_AGE) != 0;
        upd.age = st->age;
        if (table_rewrite(t, pred_matches, (void *)st, st->kind == STMT_UPDATE ? &upd : NULL, &n, err) != 0) {
            return -1;
        }
    }
    if (sink != NULL && sink->on_change != NULL && sink->on_change(st->kind, n, sink->ctx) != 0) {
        db_error_setk(err, DBE_SINK, "결과 출력에 실패했습니다");
        return -1;
    }
    return 0;
}

int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    switch (st->kind) {
    case STMT_INSERT: {
        int64_t id;
        /* 검증을 모두 통과한 뒤에만 기록한다. 실패한 INSERT는 파일에 남지 않는다. */
        if (validate_insert(st, err) != 0 ||
            table_append(t, st->name, st->age, &id, err) != 0) {
            return -1;
        }
        if (sink != NULL && sink->on_insert != NULL && sink->on_insert(id, sink->ctx) != 0) {
            db_error_setk(err, DBE_SINK, "결과 출력에 실패했습니다");
            return -1;
        }
        return 0;
    }
    case STMT_SELECT: {
        QueryPlan plan;

        if (executor_plan(t, st, &plan, err) != 0) {
            return -1;
        }
        if (st->explain) {
            if (sink != NULL && sink->on_plan != NULL && sink->on_plan(&plan, sink->ctx) != 0) {
                db_error_setk(err, DBE_SINK, "결과 출력에 실패했습니다");
                return -1;
            }
            return 0;
        }
        return run_where(t, st, &plan, sink != NULL ? sink->on_row : NULL, sink != NULL ? sink->ctx : NULL, err);
    }
    case STMT_UPDATE:
    case STMT_DELETE:
        return run_change(t, st, sink, err);
    }
    db_error_setk(err, DBE_SQL, "알 수 없는 문장 종류입니다");
    return -1;
}
