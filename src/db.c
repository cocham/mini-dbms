#include "db.h"

#include <pthread.h>
#include <stdlib.h>

/*
 * MINIDB_NO_LOCK: 경쟁 재현 실험(make race-demo) 전용. 잠금을 끄고 엔진을 그대로 동시에 부른다.
 * 정상 빌드와 테스트에서는 절대 정의하지 않는다(DB-P3-D11).
 */
#ifdef MINIDB_NO_LOCK
#define LOCK(db) ((void)(db))
#define UNLOCK(db) ((void)(db))
#else
#define LOCK(db) pthread_mutex_lock(&(db)->mu)
#define UNLOCK(db) pthread_mutex_unlock(&(db)->mu)
#endif

struct Db {
    Table *table;
    pthread_mutex_t mu;
    DbState state; /* mu 안에서만 읽고 쓴다 */
};

int db_open(const char *data_dir, Db **out, DbError *err) {
    Db *db;

    *out = NULL;
    db = calloc(1, sizeof *db);
    if (db == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    if (pthread_mutex_init(&db->mu, NULL) != 0) {
        free(db);
        db_error_setk(err, DBE_STORAGE, "잠금을 만들 수 없습니다");
        return -1;
    }
    if (table_open(data_dir, &db->table, err) != 0) {
        pthread_mutex_destroy(&db->mu);
        free(db);
        return -1;
    }
    db->state = DB_OK;
    *out = db;
    return 0;
}

static const char *halt_reason(DbState s) {
    return s == DB_HALTED_INDEX
               ? "인덱스 등록 실패 이후. 재시작하면 파일에서 인덱스를 다시 만듭니다"
               : "데이터 파일 쓰기 실패 이후. 파일 끝이 손상됐을 수 있어 재시작 전에 확인이 필요합니다";
}

int db_exec(Db *db, const Statement *st, const ResultSink *sink, int64_t *stored_id, DbError *err) {
    DbError local = {{0}, DBE_NONE};
    DbError *e = err != NULL ? err : &local; /* 종류를 보고 상태를 바꿔야 하므로 err가 NULL이어도 받는다 */
    int rc;

    if (stored_id != NULL) {
        *stored_id = 0;
    }
    e->kind = DBE_NONE;

    LOCK(db);
    if (db->state != DB_OK) {
        db_error_setk(e, DBE_HALTED, "엔진이 중단 상태입니다 (%s)", halt_reason(db->state));
        UNLOCK(db);
        return -1;
    }
    rc = executor_run(db->table, st, sink, e);
    if (rc != 0) {
        if (e->kind == DBE_INDEX_FAILED) {
            db->state = DB_HALTED_INDEX;
            if (stored_id != NULL) {
                *stored_id = table_last_id(db->table); /* 방금 기록된 줄의 id (잠금 안이라 다른 INSERT 없음) */
            }
        } else if (e->kind == DBE_WRITE_FAILED) {
            db->state = DB_HALTED_WRITE;
        }
    }
    UNLOCK(db);
    return rc;
}

DbState db_state(Db *db) {
    DbState s;

    LOCK(db);
    s = db->state;
    UNLOCK(db);
    return s;
}

void db_stats(Db *db, IndexStats *out) {
    LOCK(db);
    table_index_stats(db->table, out);
    UNLOCK(db);
}

int db_close(Db *db, DbError *err) {
    int rc;

    if (db == NULL) {
        return 0;
    }
    rc = table_close(db->table, err);
    pthread_mutex_destroy(&db->mu);
    free(db);
    return rc;
}

#ifdef MINIDB_TESTING
int db_test_validate(Db *db, char *why, size_t cap) {
    int r;

    LOCK(db);
    r = table_test_validate_index(db->table, why, cap);
    UNLOCK(db);
    return r;
}
#endif
