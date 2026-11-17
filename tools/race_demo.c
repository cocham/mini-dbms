/*
 * 경쟁 재현 실험 (Phase 3, P3-01, DB-P3-D11). make race-demo 전용, 판정 없음.
 *
 * -DMINIDB_NO_LOCK으로 빌드하면 db_exec가 잠금 없이 엔진을 부른다. 두 단계를 관찰한다.
 *   1단계 읽기만: 미리 넣은 행을 여러 스레드가 동시에 id로 조회한다. 파일은 바뀌지 않는다.
 *                공유 FILE*의 위치(fseek)가 스레드끼리 섞이면 엉뚱한 줄을 읽는다.
 *   2단계 쓰기:   여러 스레드가 동시에 INSERT한다. last_id + 1과 파일 끝 위치가 섞이면 id가 겹친다.
 *                엔진은 인덱스 등록 실패를 보면 중단 상태로 들어가므로, 이후 INSERT는 모두 거절된다.
 * 경쟁은 비결정적이라 실행마다 결과가 다르고, 이상이 없는 실행도 있을 수 있다. 프로세스가 죽을 수도 있다.
 * 잠금을 켠 정상 빌드의 정확성 판정은 tests/test_unit.c의 test_db_concurrent_correctness가 맡는다.
 *
 * 사용법: race_demo <데이터 디렉터리>   (그 안의 users.tsv를 비우고 시작한다)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "db.h"

#define THREADS 8
#define PRELOAD 2000
#define LOOKUPS 2000
#define INSERTS 500

typedef struct {
    Db *db;
    int tid;
    uint32_t seed;
    int ok;
    int wrong;  /* 결과는 왔지만 다른 행이거나 0행 / 2행 이상 */
    int errors; /* 엔진이 오류를 돌려줌 (인덱스 불일치 등) */
    int64_t ids[INSERTS];
    char first_error[256];
} Worker;

typedef struct {
    int64_t want;
    int found;
    int other;
} Probe;

static int remember_id(int64_t id, void *ctx) {
    *(int64_t *)ctx = id;
    return 0;
}

static int probe_row(const Row *row, void *ctx) {
    Probe *p = ctx;
    if (row->id == p->want) {
        p->found++;
    } else {
        p->other++;
    }
    return 0;
}

/* 중단 상태 거절(DBE_HALTED)은 결과일 뿐이므로, 그것을 뺀 첫 오류를 원인으로 남긴다. */
static void note_error(Worker *w, const DbError *err) {
    w->errors++;
    if (err->kind != DBE_HALTED && w->first_error[0] == '\0') {
        snprintf(w->first_error, sizeof w->first_error, "%s", err->msg);
    }
}

static void *read_work(void *arg) {
    Worker *w = arg;
    int i;

    for (i = 0; i < LOOKUPS; i++) {
        DbError err = {{0}, DBE_NONE};
        Probe p;
        Statement sel = {STMT_SELECT, NULL, 0, WHERE_ID, 0, 0, CMP_EQ, 0};
        ResultSink ss = {NULL, probe_row, &p, NULL};

        w->seed = w->seed * 1664525u + 1013904223u;
        p.want = 1 + (int64_t)((w->seed >> 8) % PRELOAD);
        p.found = p.other = 0;
        sel.where_value = p.want;
        if (db_exec(w->db, &sel, &ss, NULL, &err) != 0) {
            note_error(w, &err);
        } else if (p.found != 1 || p.other != 0) {
            w->wrong++;
        } else {
            w->ok++;
        }
    }
    return NULL;
}

static void *write_work(void *arg) {
    Worker *w = arg;
    int i;

    for (i = 0; i < INSERTS; i++) {
        char name[32];
        int64_t id = 0;
        DbError err = {{0}, DBE_NONE};
        Statement ins = {STMT_INSERT, name, 20, WHERE_NONE, 0, 0, CMP_EQ, 0};
        ResultSink is = {remember_id, NULL, &id, NULL};

        snprintf(name, sizeof name, "t%d_%d", w->tid, i);
        if (db_exec(w->db, &ins, &is, NULL, &err) != 0) {
            note_error(w, &err);
            continue;
        }
        w->ids[w->ok++] = id;
    }
    return NULL;
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void run(Db *db, Worker *w, void *(*fn)(void *)) {
    pthread_t th[THREADS];
    int i;

    for (i = 0; i < THREADS; i++) {
        memset(&w[i], 0, sizeof w[i]);
        w[i].db = db;
        w[i].tid = i;
        w[i].seed = 0x9E3779B9u * (uint32_t)(i + 1);
        pthread_create(&th[i], NULL, fn, &w[i]);
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
    }
}

static const char *first_error(const Worker *w) {
    int i;
    for (i = 0; i < THREADS; i++) {
        if (w[i].first_error[0] != '\0') {
            return w[i].first_error;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    static Worker w[THREADS];
    static int64_t all[THREADS * INSERTS];
    char path[512];
    DbError err = {{0}, DBE_NONE};
    Db *db = NULL;
    int i, j, n_all = 0, ok = 0, wrong = 0, errors = 0, dup = 0;
    const char *first;

    if (argc != 2) {
        fprintf(stderr, "사용법: race_demo <데이터 디렉터리>\n");
        return 2;
    }
    mkdir(argv[1], 0755);
    snprintf(path, sizeof path, "%s/users.tsv", argv[1]);
    remove(path);
    if (db_open(argv[1], &db, &err) != 0) {
        fprintf(stderr, "열기 실패: %s\n", err.msg);
        return 1;
    }
    /* 미리 넣기 (스레드 하나) */
    for (i = 0; i < PRELOAD; i++) {
        Statement ins = {STMT_INSERT, (char *)"pre", 20, WHERE_NONE, 0, 0, CMP_EQ, 0};
        if (db_exec(db, &ins, NULL, NULL, &err) != 0) {
            fprintf(stderr, "미리 넣기 실패: %s\n", err.msg);
            return 1;
        }
    }

    /* 1단계: 읽기만 */
    run(db, w, read_work);
    for (i = 0; i < THREADS; i++) {
        ok += w[i].ok;
        wrong += w[i].wrong;
        errors += w[i].errors;
    }
    first = first_error(w);
    printf("\n  읽기만: 조회 %d번 중 정상 %d, 엉뚱한 결과 %d, 오류 %d", THREADS * LOOKUPS, ok, wrong, errors);
    if (first != NULL) {
        printf("\n          첫 오류: %s", first);
    }

    /* 2단계: 동시 INSERT */
    run(db, w, write_work);
    ok = errors = 0;
    for (i = 0; i < THREADS; i++) {
        ok += w[i].ok;
        errors += w[i].errors;
        for (j = 0; j < w[i].ok; j++) {
            all[n_all++] = w[i].ids[j];
        }
    }
    qsort(all, (size_t)n_all, sizeof all[0], cmp_i64);
    for (i = 1; i < n_all; i++) {
        if (all[i] == all[i - 1]) {
            dup++;
        }
    }
    first = first_error(w);
    printf("\n  쓰기:   INSERT %d번 중 성공 %d, 실패 %d, 성공한 것 중 중복 id %d, 엔진 상태 %s",
           THREADS * INSERTS, ok, errors, dup, db_state(db) == DB_OK ? "정상" : "중단");
    if (first != NULL) {
        printf("\n          원인(중단 거절 제외 첫 오류): %s", first);
    }
    db_close(db, &err);

    if (db_open(argv[1], &db, &err) == 0) {
        printf("\n  재시작: 정상");
        db_close(db, &err);
    } else {
        printf("\n  재시작: 거부 (%s)", err.msg);
    }
    printf("\n");
    return 0;
}
