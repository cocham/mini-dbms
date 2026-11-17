#ifndef _WIN32
/* pthread_rwlock_t와 glibc의 pthread_rwlockattr_setkind_np. glibc는 -std=c17에서 이 선언들을 숨긴다 */
#define _GNU_SOURCE
#endif
#include "db.h"

#include <pthread.h>
#include <stdlib.h>

/*
 * 엔진 잠금 (Phase 3, DB-P3-D05). P3-04부터 읽기·쓰기 잠금(rwlock) 하나다.
 *   SELECT·EXPLAIN, db_state, db_stats → 공유(읽기) 잠금: 여러 개가 동시에 들어간다
 *   INSERT                              → 배타(쓰기) 잠금: 혼자 들어가고, 그동안 읽기도 기다린다
 * 읽기끼리 동시에 돌 수 있는 것은 조회 경로가 공유 상태를 쓰지 않기 때문이다(storage.c의 pread 읽기,
 * file_size 스냅숏). 잠금은 이것 하나이고, 그 안에서 다른 잠금을 잡지 않는다(spec C7).
 *
 * 쓰기 우선: glibc 기본 rwlock은 읽기를 우선한다. 선형 탐색이 겹쳐 계속 들어오면 공유 잠금이 한 번도
 * 비지 않아 INSERT가 끝없이 기다렸다(P3-04 측정: 20초 넘게 응답 없음). 그래서 glibc에서는
 * PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP로 만든다: INSERT가 기다리기 시작하면 새 읽기는 그 뒤에 선다.
 * "NONRECURSIVE"이므로 같은 스레드가 읽기 잠금을 겹쳐 잡으면 교착한다. 엔진은 잠금을 한 번만 잡고
 * 콜백(sink) 안에서 db_* 를 부르지 않는다(db.h 계약). glibc가 아닌 곳(Windows CLI·단위 테스트 등)은 기본값이다.
 *
 * 빌드 선택(정상 빌드와 테스트에서는 아무것도 정의하지 않는다):
 *   MINIDB_NO_LOCK      경쟁 재현 실험(make race-demo) 전용. 잠금을 끈다(DB-P3-D11)
 *   MINIDB_GLOBAL_LOCK  비교 측정 전용. 읽기도 배타 잠금을 잡아 P3-01~03의 전역 잠금처럼 동작한다
 */
#if defined(MINIDB_NO_LOCK)
#define RDLOCK(db) ((void)(db))
#define WRLOCK(db) ((void)(db))
#define UNLOCK(db) ((void)(db))
#elif defined(MINIDB_GLOBAL_LOCK)
#define RDLOCK(db) pthread_rwlock_wrlock(&(db)->lock)
#define WRLOCK(db) pthread_rwlock_wrlock(&(db)->lock)
#define UNLOCK(db) pthread_rwlock_unlock(&(db)->lock)
#else
#define RDLOCK(db) pthread_rwlock_rdlock(&(db)->lock)
#define WRLOCK(db) pthread_rwlock_wrlock(&(db)->lock)
#define UNLOCK(db) pthread_rwlock_unlock(&(db)->lock)
#endif

struct Db {
    Table *table;
    pthread_rwlock_t lock;
    DbState state; /* 읽기는 공유 잠금 안에서, 쓰기는 배타 잠금(INSERT 실패) 안에서만 */
};

static int lock_init(pthread_rwlock_t *lock) {
#ifdef __GLIBC__
    pthread_rwlockattr_t attr;
    int rc;

    if (pthread_rwlockattr_init(&attr) != 0) {
        return -1;
    }
    rc = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    if (rc == 0) {
        rc = pthread_rwlock_init(lock, &attr);
    }
    pthread_rwlockattr_destroy(&attr);
    return rc == 0 ? 0 : -1;
#else
    return pthread_rwlock_init(lock, NULL) == 0 ? 0 : -1;
#endif
}

int db_open(const char *data_dir, Db **out, DbError *err) {
    Db *db;

    *out = NULL;
    db = calloc(1, sizeof *db);
    if (db == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    if (lock_init(&db->lock) != 0) {
        free(db);
        db_error_setk(err, DBE_STORAGE, "잠금을 만들 수 없습니다");
        return -1;
    }
    if (table_open(data_dir, &db->table, err) != 0) {
        pthread_rwlock_destroy(&db->lock);
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
    int writer = st->kind == STMT_INSERT && !st->explain;
    int rc;

    if (stored_id != NULL) {
        *stored_id = 0;
    }
    e->kind = DBE_NONE;

    if (writer) {
        WRLOCK(db);
    } else {
        RDLOCK(db);
    }
    if (db->state != DB_OK) {
        db_error_setk(e, DBE_HALTED, "엔진이 중단 상태입니다 (%s)", halt_reason(db->state));
        UNLOCK(db);
        return -1;
    }
    rc = executor_run(db->table, st, sink, e);
    /* 상태를 바꾸는 실패(INDEX_FAILED·WRITE_FAILED)는 INSERT에서만 나오므로 항상 배타 잠금 안이다 */
    if (rc != 0 && writer) {
        if (e->kind == DBE_INDEX_FAILED) {
            db->state = DB_HALTED_INDEX;
            if (stored_id != NULL) {
                *stored_id = table_last_id(db->table); /* 방금 기록된 줄의 id (배타 잠금 안이라 다른 INSERT 없음) */
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

    RDLOCK(db);
    s = db->state;
    UNLOCK(db);
    return s;
}

void db_stats(Db *db, IndexStats *out) {
    RDLOCK(db);
    table_index_stats(db->table, out);
    UNLOCK(db);
}

int db_close(Db *db, DbError *err) {
    int rc;

    if (db == NULL) {
        return 0;
    }
    rc = table_close(db->table, err);
    pthread_rwlock_destroy(&db->lock);
    free(db);
    return rc;
}

#ifdef MINIDB_TESTING
int db_test_validate(Db *db, char *why, size_t cap) {
    int r;

    RDLOCK(db);
    r = table_test_validate_index(db->table, why, cap);
    UNLOCK(db);
    return r;
}
#endif
