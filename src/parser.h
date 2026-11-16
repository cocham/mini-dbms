#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/* Parser: SQL 텍스트 → Statement. 파일 저장은 하지 않는다. */

typedef enum { STMT_INSERT, STMT_SELECT } StmtKind;

/* SELECT의 WHERE 조건: 없음, id = 값, age = 값, id 범위(< <= > >= BETWEEN) */
typedef enum { WHERE_NONE, WHERE_ID, WHERE_AGE, WHERE_ID_RANGE } WhereKind;

/* 사용자가 쓴 비교 형태. WHERE_ID·WHERE_AGE는 CMP_EQ, WHERE_ID_RANGE는 나머지 중 하나 */
typedef enum { CMP_EQ, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_BETWEEN } CmpOp;

typedef struct {
    StmtKind kind;
    char *name;          /* INSERT: 따옴표를 풀어낸 문자열. Statement가 소유(malloc) */
    int age;             /* INSERT */
    WhereKind where;     /* SELECT */
    int64_t where_value; /* SELECT: where가 WHERE_NONE이 아닐 때 비교할 값. BETWEEN이면 아래쪽 끝 */
    int explain;         /* SELECT: EXPLAIN SELECT ...이면 1. 계획만 만들고 조회는 실행하지 않는다 */
    CmpOp where_op;      /* SELECT: 비교 형태 (EXPLAIN 출력에 원래 형태를 보여 주려고 보존) */
    int64_t where_value2; /* SELECT: CMP_BETWEEN일 때 위쪽 끝. 그 밖에는 0 */
} Statement;

/*
 * sql[*pos]부터 문장 하나를 파싱한다.
 *  1: out에 문장 하나를 채움. *pos는 세미콜론 바로 다음으로 이동
 *  0: 남은 입력이 공백뿐 (입력 끝)
 * -1: 오류. err에 원인. out은 해제된 상태라 statement_free가 필요 없다
 * 성공 시 호출자는 사용 후 statement_free(out)를 호출한다.
 */
int parser_next(const char *sql, size_t *pos, Statement *out, DbError *err);

/* 여러 번 호출해도 안전하다. */
void statement_free(Statement *st);

#endif
