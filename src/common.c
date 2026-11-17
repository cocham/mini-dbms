#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* XSI판 strerror_r (int를 돌려준다) */
#endif
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void db_error_set(DbError *err, const char *fmt, ...) {
    va_list ap;

    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
}

void db_error_setk(DbError *err, DbErrorKind kind, const char *fmt, ...) {
    va_list ap;

    if (err == NULL) {
        return;
    }
    err->kind = kind;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
}

const char *db_errstr(int errnum, char *buf, size_t cap) {
    if (cap == 0) {
        return "";
    }
#ifdef _WIN32
    if (strerror_s(buf, cap, errnum) != 0) {
        snprintf(buf, cap, "오류 %d", errnum);
    }
#else
    if (strerror_r(errnum, buf, cap) != 0) {
        snprintf(buf, cap, "오류 %d", errnum);
    }
#endif
    return buf;
}
