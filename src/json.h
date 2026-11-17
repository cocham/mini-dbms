#ifndef MINIDB_JSON_H
#define MINIDB_JSON_H

#include <stddef.h>
#include <stdint.h>

/*
 * JSON 응답 버퍼 (Phase 3, 서버 전용). 크기 상한이 있는 늘어나는 바이트 버퍼다.
 * 쓰기 함수는 0 성공, -1 실패. 실패하면 버퍼 내용은 그 쓰기 직전 그대로이고 원인 플래그가 선다.
 *   over: limit을 넘는 쓰기 (응답 예산 초과, DB-P3-D07)
 *   oom:  메모리를 늘리지 못함
 * 용량은 limit을 넘겨 키우지 않는다. 그래서 버퍼 하나의 메모리는 대략 limit + 1바이트 이하다.
 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t limit;
    int over;
    int oom;
} JsonBuf;

void jb_init(JsonBuf *b, size_t limit);
void jb_free(JsonBuf *b);

/* len을 mark로 되돌린다(mark ≤ len). 플래그는 그대로 둔다. */
void jb_truncate(JsonBuf *b, size_t mark);

/* 앞으로 n바이트를 더 쓸 자리가 있는지 확인하고 미리 확보한다. 없으면 -1(over 또는 oom). */
int jb_reserve(JsonBuf *b, size_t n);

int jb_raw(JsonBuf *b, const char *s, size_t n);
int jb_lit(JsonBuf *b, const char *s); /* NUL로 끝나는 문자열을 그대로 */
int jb_i64(JsonBuf *b, int64_t v);

/*
 * JSON 문자열 값으로 쓴다(따옴표 포함). " \ 와 제어 문자(0x00~0x1F)는 이스케이프하고,
 * 올바른 UTF-8은 그대로 둔다. 올바르지 않은 UTF-8 바이트는 �(대체 문자)로 바꾼다.
 */
int jb_str(JsonBuf *b, const char *s);
int jb_strn(JsonBuf *b, const char *s, size_t n);

#endif
