#ifndef MINIDB_COMMON_H
#define MINIDB_COMMON_H

#include <stddef.h>

/* 모든 모듈이 공유하는 오류 정보와 스키마 상수. */

/*
 * 오류 종류 (Phase 3). 메시지는 사람용, 종류는 프로그램이 판단할 때 쓴다(서버의 상태 코드 등).
 * CLI는 종류를 쓰지 않는다. db_error_set은 종류를 바꾸지 않고, db_error_setk가 함께 정한다.
 */
typedef enum {
    DBE_NONE = 0,     /* 정하지 않음 (Parser 등) */
    DBE_SQL,          /* 문장의 값·조건 오류. 파일은 건드리지 않음 */
    DBE_SINK,         /* 결과를 받는 쪽(ResultSink 콜백)이 실패를 돌려줌 */
    DBE_STORAGE,      /* 읽기 오류·인덱스 불일치·파일을 건드리기 전의 실패. 이 문장은 기록되지 않음 */
    DBE_INDEX_FAILED, /* 파일 기록 완료(fprintf·fflush 성공) 뒤 인덱스 등록 실패. 그 줄은 파일에 남음 */
    DBE_WRITE_FAILED, /* 파일 쓰기 도중 실패. 그 줄이 기록됐는지 알 수 없음(일부만 남았을 수 있음) */
    DBE_HALTED        /* 엔진이 중단 상태라 실행하지 않음 (db.h) */
} DbErrorKind;

typedef struct {
    char msg[256];
    DbErrorKind kind;
} DbError;

#if defined(__MINGW32__)
/* Windows 빌드는 __USE_MINGW_ANSI_STDIO=1로 C99 printf(%lld 등)를 쓰므로 그 규칙으로 검사한다 */
#define DB_PRINTF_LIKE(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__)
#define DB_PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define DB_PRINTF_LIKE(f, a)
#endif

/* err가 NULL이면 아무것도 하지 않는다. 메시지가 길면 잘린다. 종류는 그대로 둔다. */
void db_error_set(DbError *err, const char *fmt, ...) DB_PRINTF_LIKE(2, 3);

/* db_error_set과 같고, 종류도 정한다. */
void db_error_setk(DbError *err, DbErrorKind kind, const char *fmt, ...) DB_PRINTF_LIKE(3, 4);

/* errno 값의 설명을 buf에 담아 돌려준다. strerror와 달리 여러 스레드에서 동시에 불러도 안전하다. */
const char *db_errstr(int errnum, char *buf, size_t cap);

/* users 테이블 스키마 (spec.md 입출력 계약) */
#define NAME_MAX_BYTES 255
#define AGE_MIN 0
#define AGE_MAX 150

#endif
