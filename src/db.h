#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include <stdint.h>

#include "common.h"
#include "executor.h"
#include "parser.h"
#include "storage.h"

/*
 * DB 엔진 진입점 (Phase 3, DB-P3-D04).
 * CLI와 서버는 Executor·Storage를 직접 부르지 않고 이 함수들을 거친다.
 * 동시성 정책(잠금)과 엔진 상태(정상 / 중단)는 이 모듈 안에만 있다.
 * Parser는 전역 상태가 없으므로 잠금 밖에서 부른다.
 *
 * 잠금: P3-01은 전역 mutex 하나다. 모든 함수가 이 잠금 하나만 잡고, 잠금 안에서 다른 잠금을 잡지 않는다.
 */

typedef struct Db Db; /* 불투명 타입. Table + 잠금 + 상태 */

typedef enum {
    DB_OK = 0,
    DB_HALTED_INDEX, /* 파일 기록 완료 뒤 인덱스 등록 실패. 파일은 온전, 재시작하면 인덱스를 다시 만든다 */
    DB_HALTED_WRITE, /* 쓰기 도중 실패. 파일 끝이 깨졌을 수 있어 재시작이 거부될 수 있다 */
    DB_HALTED_REOPEN /* Windows 전용: UPDATE·DELETE의 바꿔치기 뒤 다시 열기 실패. 파일은 온전, 재시작하면 복원 */
} DbState;

/* data_dir/users.tsv를 연다(table_open). 0 성공, -1 실패. */
int db_open(const char *data_dir, Db **out, DbError *err);

/*
 * 문장 하나를 잠금 안에서 실행한다(executor_run). 0 성공, -1 실패(err에 원인과 종류).
 * sink 콜백도 잠금 안에서 호출된다. 콜백에서는 메모리 작업만 하고 네트워크 입출력은 하지 않는다.
 * - 중단 상태면 실행하지 않고 -1, err->kind = DBE_HALTED.
 * - INSERT가 DBE_INDEX_FAILED로 실패하면 엔진을 DB_HALTED_INDEX로 바꾸고, stored_id에 기록된 id를 담는다.
 * - INSERT가 DBE_WRITE_FAILED로 실패하면 엔진을 DB_HALTED_WRITE로 바꾼다. 기록 여부를 모르므로 stored_id는 0.
 * - UPDATE·DELETE가 DBE_REOPEN_FAILED(Windows 전용)로 실패하면 DB_HALTED_REOPEN으로 바꾼다.
 *   그 밖의 UPDATE·DELETE 실패는 파일·인덱스가 그대로라 상태를 바꾸지 않는다.
 * stored_id는 NULL이어도 된다. 그 밖의 경우 0.
 */
int db_exec(Db *db, const Statement *st, const ResultSink *sink, int64_t *stored_id, DbError *err);

DbState db_state(Db *db);
void db_stats(Db *db, IndexStats *out);

/* 파일을 닫고 해제한다. 사용 중인 스레드가 없을 때 부른다. db가 NULL이면 0. */
int db_close(Db *db, DbError *err);

#ifdef MINIDB_TESTING
/* 테스트 전용: 메모리 인덱스 불변식 검사. 0이면 정상 */
int db_test_validate(Db *db, char *why, size_t cap);
#endif

#endif
