#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* pread */
#endif
#include "storage.h"

#include "bptree.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <windows.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define make_dir(path) mkdir((path), 0755)
#endif

#define TABLE_FILE "users.tsv"
#define ID_MAX_DIGITS 19  /* INT64_MAX = 9223372036854775807 */
#define AGE_MAX_DIGITS 3
/* 가장 긴 레코드: 19 + 1 + 255 + 1 + 3 = 279바이트. 넉넉히 잡고 넘으면 손상으로 본다. */
#define LINE_CAP 320
#define SCAN_BUF (64 * 1024) /* 선형 탐색·범위 조회가 호출마다 따로 쓰는 읽기 버퍼 */

struct Table {
    FILE *fp;      /* "a+b". INSERT(쓰기)만 쓴다. 조회는 이 FILE*의 위치·버퍼를 건드리지 않는다 */
    int fd;        /* fp의 파일 서술자. 조회는 위치를 지정해 읽는다(read_at) */
    char *path;
    int64_t last_id;   /* 파일에 있는 최대 ID. 빈 테이블이면 0 */
    int64_t file_size; /* 끝까지 기록된 줄의 바이트 합. INSERT가 배타 잠금 안에서 늘린다 */
    BpTree *index;     /* id → 그 레코드 줄이 시작하는 바이트 위치. 메모리에만 있다 */
};

/* ---------- 레코드 읽기 (Phase 3 P3-04: 공유 상태를 쓰지 않는 읽기) ----------
 *
 * 조회는 여러 스레드에서 동시에 돌 수 있다(db.c의 공유 잠금). 그래서 조회 경로는 Table을 읽기만 한다.
 *   - 파일 위치: 공유 FILE*에 fseek하지 않고, 읽을 위치를 매번 지정한다(POSIX pread, Windows ReadFile+OVERLAPPED)
 *   - 버퍼: 호출마다 자기 Reader(지역 변수)와 버퍼를 쓴다
 *   - 파일 끝: 조회를 시작할 때의 file_size까지만 읽는다(C2: 시작 시점의 행만 본다)
 */

/* off에서 최대 n바이트를 읽는다. 읽은 바이트 수(0이면 파일 끝), -1 오류 */
static long read_at(int fd, char *buf, size_t n, int64_t off) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    DWORD got = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((uint64_t)off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) {
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (long)got;
#else
    for (;;) {
        ssize_t r = pread(fd, buf, n, (off_t)off);
        if (r >= 0) {
            return (long)r;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
#endif
}

typedef struct {
    int fd;
    char *buf;
    size_t cap;
    size_t len, at; /* buf[at, len): 아직 넘겨주지 않은 바이트 */
    int64_t next;   /* 다음 read_at 위치 */
    int64_t end;    /* 이 위치 앞까지만 읽는다 */
} Reader;

static void reader_init(Reader *r, int fd, char *buf, size_t cap, int64_t pos, int64_t end) {
    r->fd = fd;
    r->buf = buf;
    r->cap = cap;
    r->len = r->at = 0;
    r->next = pos;
    r->end = end;
}

static void reader_seek(Reader *r, int64_t pos) {
    r->len = r->at = 0;
    r->next = pos;
}

/* 바이트 하나(0~255), -1 끝, -2 읽기 오류 */
static int reader_getc(Reader *r) {
    if (r->at == r->len) {
        size_t want = r->cap;
        long n;

        if (r->next >= r->end) {
            return -1;
        }
        if ((uint64_t)(r->end - r->next) < want) {
            want = (size_t)(r->end - r->next);
        }
        n = read_at(r->fd, r->buf, want, r->next);
        if (n < 0) {
            return -2;
        }
        if (n == 0) {
            return -1;
        }
        r->len = (size_t)n;
        r->at = 0;
        r->next += n;
    }
    return (unsigned char)r->buf[r->at++];
}

/* 1: 한 줄 읽음(LF 제외), 0: 파일 끝, -1: 형식 손상(*why 설정), -2: 읽기 오류 */
static int read_line(Reader *rd, char *buf, size_t cap, size_t *out_len, const char **why) {
    size_t n = 0;
    int c;

    while ((c = reader_getc(rd)) >= 0) {
        if (c == '\n') {
            buf[n] = '\0';
            *out_len = n;
            return 1;
        }
        if (c == '\0') {
            *why = "NUL 바이트가 있습니다";
            return -1;
        }
        if (n + 1 >= cap) {
            *why = "줄이 너무 깁니다";
            return -1;
        }
        buf[n++] = (char)c;
    }
    if (c == -2) {
        return -2;
    }
    if (n > 0) {
        *why = "마지막 줄이 개행(LF)으로 끝나지 않습니다";
        return -1;
    }
    return 0;
}

/* s가 0 이상의 정규형 10진수(앞자리 0 없음)이면 *out에 담고 0. */
static int parse_digits(const char *s, int max_digits, int64_t *out) {
    int64_t v = 0;
    int n = 0;

    if (s[0] == '\0' || (s[0] == '0' && s[1] != '\0')) {
        return -1;
    }
    for (; s[n] != '\0'; n++) {
        int d;
        if (s[n] < '0' || s[n] > '9' || n >= max_digits) {
            return -1;
        }
        d = s[n] - '0';
        if (v > (INT64_MAX - d) / 10) {
            return -1;
        }
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* line을 제자리에서 잘라 Row로 만든다. row->name은 line 안을 가리킨다. */
static int parse_record(char *line, size_t len, int64_t prev_id, Row *row, const char **why) {
    char *tab1 = memchr(line, '\t', len);
    char *tab2;
    char *name;
    size_t name_len;
    int64_t v;

    if (tab1 == NULL ||
        (tab2 = memchr(tab1 + 1, '\t', len - (size_t)(tab1 + 1 - line))) == NULL ||
        memchr(tab2 + 1, '\t', len - (size_t)(tab2 + 1 - line)) != NULL) {
        *why = "필드가 3개(id, name, age)가 아닙니다";
        return -1;
    }
    *tab1 = '\0';
    *tab2 = '\0';
    name = tab1 + 1;
    name_len = (size_t)(tab2 - name);

    if (parse_digits(line, ID_MAX_DIGITS, &v) != 0 || v <= 0) {
        *why = "id가 양의 정수가 아닙니다";
        return -1;
    }
    if (v <= prev_id) {
        *why = "id가 증가 순서가 아닙니다";
        return -1;
    }
    row->id = v;

    if (name_len == 0 || name_len > NAME_MAX_BYTES || memchr(name, '\r', name_len) != NULL) {
        *why = "name이 비었거나 255바이트를 넘거나 CR을 포함합니다";
        return -1;
    }
    row->name = name;

    if (parse_digits(tab2 + 1, AGE_MAX_DIGITS, &v) != 0 || v < AGE_MIN || v > AGE_MAX) {
        *why = "age가 0~150 정수가 아닙니다";
        return -1;
    }
    row->age = (int)v;
    return 0;
}

static int scan_file(Table *t, BpTree *build, RowFn fn, void *ctx, int64_t end, int64_t *out_last_id,
                     int64_t *out_size, DbError *err);

/* ---------- 공개 함수 ---------- */

int table_open(const char *data_dir, Table **out, DbError *err) {
    Table *t;
    size_t path_len;
    int saved_errno;
    char eb[128];

    *out = NULL;
    t = calloc(1, sizeof *t);
    if (t == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    path_len = strlen(data_dir) + 1 + strlen(TABLE_FILE) + 1;
    t->path = malloc(path_len);
    if (t->path == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        goto fail;
    }
    snprintf(t->path, path_len, "%s/%s", data_dir, TABLE_FILE);

    t->fp = fopen(t->path, "a+b");
    if (t->fp == NULL) {
        saved_errno = errno;
        if (saved_errno == ENOENT && make_dir(data_dir) == 0) {
            t->fp = fopen(t->path, "a+b");
            saved_errno = errno;
        }
        if (t->fp == NULL) {
            db_error_setk(err, DBE_STORAGE, "데이터 파일을 열 수 없습니다: %s (%s)", t->path, db_errstr(saved_errno, eb, sizeof eb));
            goto fail;
        }
    }
#ifdef _WIN32
    t->fd = _fileno(t->fp);
#else
    t->fd = fileno(t->fp);
#endif

    /* 시작할 때 한 번만 전체(실제 파일 끝까지)를 검증하며 최대 ID와 파일 크기를 복원하고,
       id 인덱스를 메모리에 다시 만든다. */
    t->index = bpt_create(BPT_DEFAULT_ORDER);
    if (t->index == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        goto fail;
    }
    if (scan_file(t, t->index, NULL, NULL, INT64_MAX, &t->last_id, &t->file_size, err) != 0) {
        goto fail;
    }
    *out = t;
    return 0;

fail:
    if (t->fp != NULL) {
        fclose(t->fp);
    }
    bpt_destroy(t->index);
    free(t->path);
    free(t);
    return -1;
}

/*
 * 파일을 처음부터 end 앞까지 한 줄씩 읽어 검증하고 fn을 호출한다.
 * build가 NULL이 아니면 각 레코드의 (id, 줄 시작 바이트 위치)를 build 인덱스에 넣는다.
 * 위치는 직접 센다: 줄 길이 + LF 1바이트. 바이너리 모드라 바이트 수가 정확하다.
 * out_last_id·out_size가 NULL이 아니면 마지막 id와 읽은 바이트 수를 담는다. 시작 시 복원(table_open)만
 * 넘기고, 일반 조회(table_scan)는 NULL을 넘겨 공유 상태(Table)를 바꾸지 않는다(Phase 3, DB-P3-D05).
 */
static int scan_file(Table *t, BpTree *build, RowFn fn, void *ctx, int64_t end, int64_t *out_last_id,
                     int64_t *out_size, DbError *err) {
    char line[LINE_CAP];
    size_t len;
    long lineno = 0;
    int64_t prev_id = 0;
    int64_t offset = 0; /* 다음에 읽을 줄이 시작하는 바이트 위치 */
    char *buf = malloc(SCAN_BUF);
    Reader rd;
    int rc = 0;

    if (buf == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    reader_init(&rd, t->fd, buf, SCAN_BUF, 0, end);
    for (;;) {
        Row row;
        const char *why = NULL;
        int64_t line_start = offset;
        int r = read_line(&rd, line, sizeof line, &len, &why);

        if (r == 0) {
            break;
        }
        lineno++;
        if (r == -2) {
            db_error_setk(err, DBE_STORAGE, "데이터 파일 읽기 실패: %s", t->path);
            rc = -1;
            break;
        }
        if (r == -1 || parse_record(line, len, prev_id, &row, &why) != 0) {
            db_error_setk(err, DBE_STORAGE, "데이터 파일 손상: %s %ld번째 줄: %s", t->path, lineno, why);
            rc = -1;
            break;
        }
        offset += (int64_t)len + 1;
        prev_id = row.id;
        if (build != NULL && bpt_insert(build, row.id, line_start) != BPT_OK) {
            /* id는 위에서 증가 순서를 확인했으므로 중복은 없다. 남은 원인은 메모리 부족뿐이다. */
            db_error_setk(err, DBE_STORAGE, "메모리가 부족해 인덱스를 만들 수 없습니다");
            rc = -1;
            break;
        }
        if (fn != NULL && fn(&row, ctx) != 0) {
            db_error_setk(err, DBE_SINK, "조회 결과 처리에 실패했습니다");
            rc = -1;
            break;
        }
    }
    free(buf);
    if (rc == 0) {
        if (out_last_id != NULL) {
            *out_last_id = prev_id;
        }
        if (out_size != NULL) {
            *out_size = offset;
        }
    }
    return rc;
}

int table_scan(Table *t, RowFn fn, void *ctx, DbError *err) {
    return scan_file(t, NULL, fn, ctx, t->file_size, NULL, NULL, err);
}

int table_find_by_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err) {
    char line[LINE_CAP];
    char buf[LINE_CAP];
    size_t len;
    int64_t offset;
    const char *why = NULL;
    Reader rd;
    Row row;
    int r;

    /* 1) B+ 트리에서 id → 바이트 위치. 없으면 파일을 읽지 않고 끝난다. */
    if (!bpt_search(t->index, id, &offset)) {
        return 0;
    }
    /* 2) 그 위치에서 한 줄만 읽는다(한 줄은 LINE_CAP 안에 들어간다). */
    reader_init(&rd, t->fd, buf, sizeof buf, offset, t->file_size);
    r = read_line(&rd, line, sizeof line, &len, &why);
    if (r == -2) {
        db_error_setk(err, DBE_STORAGE, "데이터 파일 읽기 실패: %s", t->path);
        return -1;
    }
    /* 3) 읽은 줄이 정말 그 id인지 확인한다. 열린 뒤 파일이 바뀌었으면 여기서 드러난다. */
    if (r != 1 || parse_record(line, len, 0, &row, &why) != 0 || row.id != id) {
        db_error_setk(err, DBE_STORAGE, "인덱스와 데이터 파일이 일치하지 않습니다: %s (id %" PRId64 ", 위치 %" PRId64 ")",
                     t->path, id, offset);
        return -1;
    }
    if (fn != NULL && fn(&row, ctx) != 0) {
        db_error_setk(err, DBE_SINK, "조회 결과 처리에 실패했습니다");
        return -1;
    }
    return 0;
}

/* table_find_id_range가 bpt_range 콜백으로 넘기는 상태(호출마다 지역 변수) */
typedef struct {
    Table *t;
    RowFn fn;
    void *ctx;
    DbError *err;
    Reader rd;
    int64_t next_pos; /* rd가 다음에 넘겨줄 바이트의 위치. -1이면 아직 이동 전 */
} RangeRead;

/*
 * 리프에서 (id, 위치)를 하나 받을 때마다 그 줄을 읽는다.
 * id가 증가 순서로 저장되므로 범위 안의 줄은 파일에서도 이어져 있다. 그래서 보통 첫 줄에서만
 * 위치를 정하고 나머지는 버퍼에서 이어서 읽는다. 위치가 이어지지 않으면(예상 밖 상황) 그 위치로 다시 정한다.
 * 반환: 0 계속, 1 오류(err 설정 완료)
 */
static int range_visit(int64_t id, int64_t offset, void *vctx) {
    RangeRead *rr = vctx;
    char line[LINE_CAP];
    size_t len;
    const char *why = NULL;
    Row row;
    int r;

    if (offset != rr->next_pos) {
        reader_seek(&rr->rd, offset);
    }
    r = read_line(&rr->rd, line, sizeof line, &len, &why);
    if (r == -2) {
        db_error_setk(rr->err, DBE_STORAGE, "데이터 파일 읽기 실패: %s", rr->t->path);
        return 1;
    }
    /* table_find_by_id와 같은 재확인: 열린 뒤 파일이 바뀌었으면 엉뚱한 행 대신 오류 */
    if (r != 1 || parse_record(line, len, 0, &row, &why) != 0 || row.id != id) {
        db_error_setk(rr->err, DBE_STORAGE, "인덱스와 데이터 파일이 일치하지 않습니다: %s (id %" PRId64 ", 위치 %" PRId64 ")",
                     rr->t->path, id, offset);
        return 1;
    }
    rr->next_pos = offset + (int64_t)len + 1;
    if (rr->fn != NULL && rr->fn(&row, rr->ctx) != 0) {
        db_error_setk(rr->err, DBE_SINK, "조회 결과 처리에 실패했습니다");
        return 1;
    }
    return 0;
}

int table_find_id_range(Table *t, int64_t lo, int64_t hi, RowFn fn, void *ctx, DbError *err) {
    RangeRead rr;
    char *buf = malloc(SCAN_BUF);
    int rc;

    if (buf == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    rr.t = t;
    rr.fn = fn;
    rr.ctx = ctx;
    rr.err = err;
    reader_init(&rr.rd, t->fd, buf, SCAN_BUF, 0, t->file_size);
    rr.next_pos = -1;
    rc = bpt_range(t->index, lo, hi, range_visit, &rr) == 0 ? 0 : -1;
    free(buf);
    return rc;
}

#ifdef MINIDB_TESTING
static int g_fail_write; /* 1이면 다음 append가 줄의 앞부분만 쓰고 쓰기 실패로 끝난다 */
void table_test_fail_write(int on) { g_fail_write = on; }
#endif

int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err) {
    size_t name_len = strlen(name);
    int64_t id;
    long offset;
    int written;
    char eb[128];

    if (out_id != NULL) {
        *out_id = 0;
    }
    /* 파일 형식을 깨는 값은 Executor 검증과 별개로 여기서도 막는다. */
    if (name_len == 0 || name_len > NAME_MAX_BYTES || strpbrk(name, "\t\r\n") != NULL ||
        age < AGE_MIN || age > AGE_MAX) {
        db_error_setk(err, DBE_STORAGE, "저장할 수 없는 레코드입니다");
        return -1;
    }
    if (t->last_id == INT64_MAX) {
        db_error_setk(err, DBE_STORAGE, "더 이상 부여할 ID가 없습니다");
        return -1;
    }
    id = t->last_id + 1;

    /* 새 줄이 시작할 위치(= 현재 파일 크기)를 기록 전에 구해 둔다. 여기까지는 파일을 바꾸지 않았다. */
    if (fseek(t->fp, 0, SEEK_END) != 0 || (offset = ftell(t->fp)) < 0) {
        db_error_setk(err, DBE_STORAGE, "데이터 파일 위치 확인 실패: %s (%s)", t->path,
                      db_errstr(errno, eb, sizeof eb));
        return -1;
    }
#ifdef MINIDB_TESTING
    if (g_fail_write) {
        g_fail_write = 0;
        fprintf(t->fp, "%" PRId64 "\t", id); /* 줄의 앞부분만 남긴다 */
        fflush(t->fp);
        errno = EIO;
        db_error_setk(err, DBE_WRITE_FAILED, "데이터 파일 쓰기 실패: %s (%s)", t->path,
                      db_errstr(errno, eb, sizeof eb));
        return -1;
    }
#endif
    /* 쓰기 도중 실패하면 줄 전체·일부·없음 중 무엇이 남았는지 알 수 없다. */
    if ((written = fprintf(t->fp, "%" PRId64 "\t%s\t%d\n", id, name, age)) < 0 || fflush(t->fp) != 0) {
        db_error_setk(err, DBE_WRITE_FAILED, "데이터 파일 쓰기 실패: %s (%s)", t->path,
                      db_errstr(errno, eb, sizeof eb));
        return -1;
    }
    /* 여기부터는 파일 기록 완료(운영체제에 넘어감, fsync는 하지 않음).
       조회가 읽는 끝(file_size)은 줄 전체가 기록된 뒤에만 늘린다: 반쯤 쓰인 줄은 조회에 보이지 않는다. */
    t->last_id = id;
    t->file_size = (int64_t)offset + written;
    if (out_id != NULL) {
        *out_id = id;
    }
    /* 파일에 기록한 뒤 인덱스에 등록한다. 실패해도 레코드는 파일에 남는다.
     * 다음 실행의 table_open이 파일에서 인덱스를 다시 만든다. *out_id는 기록된 id 그대로 둔다.
     * 정상이라면 실패 원인은 메모리 부족뿐이다. 중복은 last_id 갱신이 동시에 일어난 경우(잠금 없음)에만 생긴다. */
    switch (bpt_insert(t->index, id, (int64_t)offset)) {
    case BPT_OK: break;
    case BPT_DUPLICATE:
        db_error_setk(err, DBE_INDEX_FAILED,
                      "id %" PRId64 "가 이미 인덱스에 있어 등록하지 못했습니다 (레코드는 저장됨)", id);
        return -1;
    default:
        db_error_setk(err, DBE_INDEX_FAILED,
                      "메모리가 부족해 id %" PRId64 "를 인덱스에 등록하지 못했습니다 "
                      "(레코드는 저장됨, 다시 실행하면 인덱스가 재구성됨)", id);
        return -1;
    }
    return 0;
}

int64_t table_last_id(const Table *t) { return t->last_id; }

#ifdef MINIDB_TESTING
int table_test_validate_index(const Table *t, char *why, size_t cap) { return bpt_validate(t->index, why, cap); }
#endif

int table_index_height(const Table *t) { return bpt_height(t->index); }

/* 모든 레코드가 인덱스에 등록되므로(table_open·table_append) 키 수가 곧 행 수다. */
int64_t table_row_count(const Table *t) { return (int64_t)bpt_size(t->index); }

void table_index_stats(const Table *t, IndexStats *out) {
    BptMemStats m;

    bpt_mem_stats(t->index, &m);
    out->height = bpt_height(t->index);
    out->keys = (int64_t)bpt_size(t->index);
    out->leaves = m.leaves;
    out->internals = m.internals;
    out->bytes = m.bytes;
}

int table_close(Table *t, DbError *err) {
    char eb[128];
    int rc;

    if (t == NULL) {
        return 0;
    }
    rc = fclose(t->fp);
    if (rc != 0) {
        db_error_setk(err, DBE_STORAGE, "데이터 파일 닫기 실패: %s (%s)", t->path, db_errstr(errno, eb, sizeof eb));
    }
    bpt_destroy(t->index);
    free(t->path);
    free(t);
    return rc == 0 ? 0 : -1;
}
