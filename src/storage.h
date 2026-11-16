#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include <stdint.h>

#include "common.h"

/*
 * Storage: users.tsv 파일 열기, 순차 읽기, 추가, 다음 ID 관리, id 인덱스(B+ 트리) 관리.
 * SQL 문법은 알지 않는다.
 * 파일 형식: 헤더 없이 한 줄에 "id<TAB>name<TAB>age<LF>".
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

/* 다음 ID(최대 ID + 1)를 부여해 레코드 한 건을 파일 끝에 추가하고 인덱스에 등록한다. */
int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err);

/* 저장 순서대로 레코드를 하나씩 읽어 fn을 호출한다. fn이 NULL이면 검증만 한다. */
int table_scan(Table *t, RowFn fn, void *ctx, DbError *err);

/* id 인덱스로 레코드 한 건을 찾는다. 있으면 그 위치의 한 줄만 읽어 fn을 한 번 호출하고,
 * 없으면 파일을 읽지 않고 fn 없이 0을 반환한다. 읽은 줄의 id가 다르면 -1(인덱스 불일치). */
int table_find_by_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err);

/*
 * 인덱스 통계 (실행 계획용). 메모리의 B+ 트리에서 바로 읽는다.
 * table_open이 시작 시 파일 전체를 읽어 인덱스를 만든 뒤로는, 이 함수들이 파일을 추가로 읽지 않는다.
 * 실제로 조회를 실행해서 잰 값이 아니라 "지금 실행하면 얼마나 읽게 될지"의 근거다.
 */
int table_index_height(const Table *t); /* B+ 트리 높이 = id 조회 한 번이 방문하는 노드 수. 빈 테이블도 1 */
int64_t table_row_count(const Table *t); /* 전체 행 수 = 인덱스 키 수 = 선형 탐색이 읽는 줄 수 */

/* 파일을 닫고 메모리를 해제한다. 실패해도 t는 해제된다. t가 NULL이면 0. */
int table_close(Table *t, DbError *err);

#endif
