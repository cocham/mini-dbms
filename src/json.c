#include "json.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jb_init(JsonBuf *b, size_t limit) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->limit = limit;
    b->over = 0;
    b->oom = 0;
}

void jb_free(JsonBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void jb_truncate(JsonBuf *b, size_t mark) {
    if (mark < b->len) {
        b->len = mark;
    }
}

int jb_reserve(JsonBuf *b, size_t n) {
    size_t need;
    size_t ncap;
    char *p;

    if (n > b->limit || b->len > b->limit - n) {
        b->over = 1;
        return -1;
    }
    need = b->len + n;
    if (need <= b->cap) {
        return 0;
    }
    /* 두 배씩 키우되 limit을 넘기지 않는다. need ≤ limit이 위에서 보장되므로 반드시 끝난다. */
    ncap = b->cap == 0 ? 256 : b->cap;
    if (ncap > b->limit) {
        ncap = b->limit;
    }
    while (ncap < need) {
        ncap = ncap > b->limit / 2 ? b->limit : ncap * 2;
    }
    p = realloc(b->data, ncap);
    if (p == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = p;
    b->cap = ncap;
    return 0;
}

int jb_raw(JsonBuf *b, const char *s, size_t n) {
    if (n == 0) {
        return 0; /* 아직 버퍼가 없을 수 있다(data == NULL). memcpy에 NULL을 넘기지 않는다 */
    }
    if (jb_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return 0;
}

int jb_lit(JsonBuf *b, const char *s) { return jb_raw(b, s, strlen(s)); }

int jb_i64(JsonBuf *b, int64_t v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%" PRId64, v);
    return jb_raw(b, tmp, (size_t)n);
}

/* s[i..]가 올바른 UTF-8 한 글자면 그 바이트 수(2~4), 아니면 0. ASCII는 따로 처리한다. */
static size_t utf8_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    size_t len, k;
    uint32_t cp;

    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (k = 1; k < len; k++) {
        if ((s[k] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[k] & 0x3F);
    }
    /* 너무 긴 표현, 서로게이트, 범위 밖 거부 */
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return len;
}

int jb_strn(JsonBuf *b, const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    size_t mark = b->len;
    size_t i = 0;

    if (jb_raw(b, "\"", 1) != 0) {
        return -1;
    }
    while (i < n) {
        unsigned char c = u[i];
        int r;

        if (c == '"') {
            r = jb_raw(b, "\\\"", 2);
            i++;
        } else if (c == '\\') {
            r = jb_raw(b, "\\\\", 2);
            i++;
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            r = jb_raw(b, esc, 6);
            i++;
        } else if (c < 0x80) {
            size_t j = i;
            while (j < n && u[j] >= 0x20 && u[j] < 0x80 && u[j] != '"' && u[j] != '\\') {
                j++;
            }
            r = jb_raw(b, s + i, j - i);
            i = j;
        } else {
            size_t len = utf8_len(u + i, n - i);
            if (len == 0) {
                r = jb_raw(b, "\\ufffd", 6);
                i++;
            } else {
                r = jb_raw(b, s + i, len);
                i += len;
            }
        }
        if (r != 0) {
            jb_truncate(b, mark);
            return -1;
        }
    }
    if (jb_raw(b, "\"", 1) != 0) {
        jb_truncate(b, mark);
        return -1;
    }
    return 0;
}

int jb_str(JsonBuf *b, const char *s) { return jb_strn(b, s, strlen(s)); }
