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
#define NEXT_FILE "users.next" /* 마지막으로 부여한 id (Phase 4, DB-P4-D04) */
#define TMP_SUFFIX ".tmp"
#define ID_MAX_DIGITS 19  /* INT64_MAX = 9223372036854775807 */
#define AGE_MAX_DIGITS 3
/* 가장 긴 레코드: 19 + 1 + 255 + 1 + 3 = 279바이트. 넉넉히 잡고 넘으면 손상으로 본다. */
#define LINE_CAP 320
#define SCAN_BUF (64 * 1024) /* 선형 탐색·범위 조회가 호출마다 따로 쓰는 읽기 버퍼 */

struct Table {
    FILE *fp;      /* "a+b". INSERT(쓰기)만 쓴다. 조회는 이 FILE*의 위치·버퍼를 건드리지 않는다.
                      Windows에서 다시 쓰기 뒤 다시 열기에 실패하면 NULL(엔진이 중단 상태가 된다) */
    int fd;        /* fp의 파일 서술자. 조회는 위치를 지정해 읽는다(read_at) */
    char *path;          /* data_dir/users.tsv */
    char *tmp_path;      /* data_dir/users.tsv.tmp: 다시 쓰기(UPDATE·DELETE)가 새 내용을 쓰는 곳 */
    char *next_path;     /* data_dir/users.next */
    char *next_tmp_path; /* data_dir/users.next.tmp */
    int64_t next_saved;  /* users.next에 적힌 값. 없으면 0 */
    int64_t last_id;   /* 마지막으로 부여한 ID = max(파일의 최대 ID, users.next). 빈 테이블이면 0 */
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

/* dir/name+suffix를 malloc해 돌려준다. 메모리가 부족하면 NULL */
static char *join_path(const char *dir, const char *name, const char *suffix) {
    size_t n = strlen(dir) + 1 + strlen(name) + strlen(suffix) + 1;
    char *p = malloc(n);

    if (p != NULL) {
        snprintf(p, n, "%s/%s%s", dir, name, suffix);
    }
    return p;
}

/* FILE*는 닫은 뒤 부른다 */
static void table_free(Table *t) {
    bpt_destroy(t->index);
    free(t->path);
    free(t->tmp_path);
    free(t->next_path);
    free(t->next_tmp_path);
    free(t);
}

/* users.next를 읽어 t->next_saved에 담는다. 없으면 0. 형식: 양의 정수 한 줄("5\n") */
static int read_next_file(Table *t, DbError *err) {
    char buf[ID_MAX_DIGITS + 3];
    size_t n;
    int bad;
    int64_t v = 0;
    char eb[128];
    FILE *fp = fopen(t->next_path, "rb");

    t->next_saved = 0;
    if (fp == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        db_error_setk(err, DBE_STORAGE, "다음 id 파일을 열 수 없습니다: %s (%s)", t->next_path,
                      db_errstr(errno, eb, sizeof eb));
        return -1;
    }
    n = fread(buf, 1, sizeof buf - 1, fp);
    bad = ferror(fp) || !feof(fp); /* 다 못 읽었으면 너무 길다 */
    fclose(fp);
    buf[n] = '\0';
    if (bad || n < 2 || buf[n - 1] != '\n') {
        bad = 1;
    } else {
        size_t i;
        /* parse_digits는 NUL에서 멈추므로, 읽은 바이트 전체(LF 앞)가 숫자인지 먼저 본다.
           "1\0" "00\n" 같은 파일을 1로 읽으면 지운 id를 다시 줄 수 있다 */
        for (i = 0; i + 1 < n; i++) {
            if (buf[i] < '0' || buf[i] > '9') {
                bad = 1;
            }
        }
    }
    if (!bad) {
        buf[n - 1] = '\0';
        bad = parse_digits(buf, ID_MAX_DIGITS, &v) != 0 || v <= 0;
    }
    if (bad) {
        db_error_setk(err, DBE_STORAGE, "다음 id 파일 손상: %s (양의 정수 한 줄이어야 합니다)", t->next_path);
        return -1;
    }
    t->next_saved = v;
    return 0;
}

/* ---------- 공개 함수 ---------- */

int table_open(const char *data_dir, Table **out, DbError *err) {
    Table *t;
    int saved_errno;
    char eb[128];

    *out = NULL;
    t = calloc(1, sizeof *t);
    if (t == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    t->fd = -1;
    t->path = join_path(data_dir, TABLE_FILE, "");
    t->tmp_path = join_path(data_dir, TABLE_FILE, TMP_SUFFIX);
    t->next_path = join_path(data_dir, NEXT_FILE, "");
    t->next_tmp_path = join_path(data_dir, NEXT_FILE, TMP_SUFFIX);
    if (t->path == NULL || t->tmp_path == NULL || t->next_path == NULL || t->next_tmp_path == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        goto fail;
    }
    /* 이전 실행이 다시 쓰기 도중 끝났으면 임시 파일이 남는다. 바꿔치기 전까지 원래 파일은 온전하므로 지운다. */
    remove(t->tmp_path);
    remove(t->next_tmp_path);

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
    if (scan_file(t, t->index, NULL, NULL, INT64_MAX, &t->last_id, &t->file_size, err) != 0 ||
        read_next_file(t, err) != 0) {
        goto fail;
    }
    if (t->next_saved > t->last_id) {
        t->last_id = t->next_saved; /* 가장 큰 id의 행이 지워졌다: 그 id를 다시 쓰지 않는다 */
    }
    *out = t;
    return 0;

fail:
    if (t->fp != NULL) {
        fclose(t->fp);
    }
    table_free(t);
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

/* table_find_id_range·table_page가 bpt_range 콜백으로 넘기는 상태(호출마다 지역 변수) */
typedef struct {
    Table *t;
    RowFn fn;
    void *ctx;
    DbError *err;
    Reader rd;
    int64_t next_pos; /* rd가 다음에 넘겨줄 바이트의 위치. -1이면 아직 이동 전 */
    int64_t limit;    /* table_page: 넘길 최대 행 수. 제한 없으면 -1 */
    int64_t lines;    /* 파일에서 읽은 줄 수 */
    int has_more;     /* table_page: limit+1번째 키를 만났으면 1 */
} RangeRead;

#define RANGE_STOP 2 /* range_visit의 정상 멈춤. bpt_range가 그대로 돌려준다 */

#ifdef MINIDB_TESTING
static _Thread_local int64_t g_page_lines; /* 스레드마다 따로: 동시 테스트에서도 경쟁 없음 */
int64_t table_test_page_lines_read(void) { return g_page_lines; }
#endif

/*
 * 리프에서 (id, 위치)를 하나 받을 때마다 그 줄을 읽는다.
 * id가 증가 순서로 저장되므로 범위 안의 줄은 파일에서도 이어져 있다. 그래서 보통 첫 줄에서만
 * 위치를 정하고 나머지는 버퍼에서 이어서 읽는다. 위치가 이어지지 않으면(예상 밖 상황) 그 위치로 다시 정한다.
 * limit만큼 넘긴 뒤 키를 하나 더 받으면 그 줄은 읽지 않고 has_more만 표시한다(N+1, DB-P5-D02).
 * 반환: 0 계속, 1 오류(err 설정 완료), RANGE_STOP 정상 멈춤
 */
static int range_visit(int64_t id, int64_t offset, void *vctx) {
    RangeRead *rr = vctx;
    char line[LINE_CAP];
    size_t len;
    const char *why = NULL;
    Row row;
    int r;

    if (rr->limit >= 0 && rr->lines >= rr->limit) {
        rr->has_more = 1;
        return RANGE_STOP;
    }
    rr->lines++;
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

/* [lo, hi]를 id 순서로 읽는다. limit ≥ 0이면 limit개를 넘긴 뒤 다음 키에서 정상으로 멈춘다. */
static int range_read(Table *t, int64_t lo, int64_t hi, int64_t limit, RowFn fn, void *ctx, int *has_more,
                      int64_t *lines, DbError *err) {
    RangeRead rr;
    char *buf = malloc(SCAN_BUF);
    int r;

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
    rr.limit = limit;
    rr.lines = 0;
    rr.has_more = 0;
    r = bpt_range(t->index, lo, hi, range_visit, &rr);
    free(buf);
    if (has_more != NULL) {
        *has_more = rr.has_more;
    }
    if (lines != NULL) {
        *lines = rr.lines;
    }
    return (r == 0 || r == RANGE_STOP) ? 0 : -1;
}

int table_find_id_range(Table *t, int64_t lo, int64_t hi, RowFn fn, void *ctx, DbError *err) {
    return range_read(t, lo, hi, -1, fn, ctx, NULL, NULL, err);
}

int table_page(Table *t, int64_t after_id, int64_t limit, RowFn fn, void *ctx, int *has_more, DbError *err) {
    int64_t lines = 0;
    int more = 0;
    int rc;

    if (has_more != NULL) {
        *has_more = 0;
    }
#ifdef MINIDB_TESTING
    g_page_lines = 0;
#endif
    if (limit < 1 || after_id < 0) {
        db_error_setk(err, DBE_SQL, "페이지 조회 값이 올바르지 않습니다 (after_id %" PRId64 ", limit %" PRId64 ")",
                     after_id, limit);
        return -1;
    }
    if (after_id == INT64_MAX) {
        return 0; /* 그보다 큰 id는 없다. after_id + 1은 넘치므로 트리를 보지 않는다 */
    }
    rc = range_read(t, after_id + 1, INT64_MAX, limit, fn, ctx, &more, &lines, err);
#ifdef MINIDB_TESTING
    g_page_lines = lines;
#endif
    if (rc == 0 && has_more != NULL) {
        *has_more = more;
    }
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

/* ---------- 다시 쓰기 (Phase 4: UPDATE·DELETE, DB-P4-D01·D05) ----------
 *
 * 바뀐 내용 전체를 users.tsv.tmp에 쓰고 users.tsv와 바꿔치기(rename)한다. 원래 파일은 바꿔치기 전까지
 * 건드리지 않으므로, 그 앞에서 실패하면 파일·인덱스·조회 결과가 모두 그대로다(임시 파일은 지운다).
 * 호출자(db.c)가 배타 잠금을 잡고 부른다. 그래서 다시 쓰는 동안 조회·INSERT가 끼어들지 않는다.
 */

#ifdef MINIDB_TESTING
static int g_fail_rewrite; /* REWRITE_FAIL_*: 다음 다시 쓰기 한 번이 그 단계에서 실패한다 */
void table_test_fail_rewrite(int stage) { g_fail_rewrite = stage; }
static int take_fail(int stage) {
    if (g_fail_rewrite != stage) {
        return 0;
    }
    g_fail_rewrite = REWRITE_FAIL_NONE;
    errno = EIO;
    return 1;
}
#define FAIL_AT(stage) take_fail(stage)
#else
#define FAIL_AT(stage) 0
#endif

/* from을 to로 바꾼다(to가 있으면 덮어씀). POSIX rename은 한 번에 일어난다.
   Windows rename은 있는 파일을 덮어쓰지 않으므로 MoveFileExA를 쓴다.
   Windows에서는 방금 닫거나 쓴 파일을 다른 프로세스(백신 검사·색인 등)가 잠깐 열고 있으면
   MoveFileExA가 ERROR_ACCESS_DENIED·ERROR_SHARING_VIOLATION으로 실패한다(단위 테스트의 동시 다시 쓰기에서
   여러 번 관찰). 그 두 오류만 짧게 기다리며 다시 시도한다(최대 약 1초). 바꿔치기 전이므로 기다려도 원래 상태다. */
static int replace_file(const char *from, const char *to) {
#ifdef _WIN32
    DWORD code;
    int attempt;

    for (attempt = 0;; attempt++) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
            return 0;
        }
        code = GetLastError();
        if ((code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) || attempt >= 20) {
            break;
        }
        Sleep((DWORD)(attempt < 5 ? 10 : 50));
    }
    errno = EACCES;
    return -1;
#else
    return rename(from, to);
#endif
}

/* users.next에 v를 쓴다: users.next.tmp에 쓰고 바꿔치기. 실패하면 users.next는 이전 내용 그대로다. */
static int write_next_file(Table *t, int64_t v, DbError *err) {
    char eb[128];
    int saved;
    FILE *fp = fopen(t->next_tmp_path, "wb");
    int ok = fp != NULL && fprintf(fp, "%" PRId64 "\n", v) > 0 && !FAIL_AT(REWRITE_FAIL_NEXT);

    if (fp != NULL && fclose(fp) != 0) {
        ok = 0;
    }
    if (ok && replace_file(t->next_tmp_path, t->next_path) != 0) {
        ok = 0;
    }
    if (ok) {
        return 0;
    }
    saved = errno;
    remove(t->next_tmp_path);
    db_error_setk(err, DBE_STORAGE, "다음 id 파일 쓰기 실패: %s (%s)", t->next_path, db_errstr(saved, eb, sizeof eb));
    return -1;
}

typedef struct {
    RowPred match;
    void *match_ctx;
    const RowUpdate *upd; /* NULL이면 DELETE */
    FILE *out;            /* users.tsv.tmp */
    BpTree *index;        /* 새 파일 기준 id → 바이트 위치 */
    int64_t size;         /* 새 파일에 쓴 바이트 수 = 다음 줄이 시작할 위치 */
    int64_t max_id;       /* 새 파일의 최대 id. 비면 0 */
    int64_t count;        /* 조건에 맞은 행 수 */
    const char *tmp_path;
    DbError err;          /* 콜백에서 난 오류. failed가 1일 때만 의미가 있다 */
    int failed;
} RewriteCtx;

/* 원래 파일의 행을 하나씩 받아 새 파일에 쓴다: 지울 행은 건너뛰고, 고칠 행은 새 값으로, 나머지는 그대로. */
static int rewrite_row(const Row *row, void *vctx) {
    RewriteCtx *rc = vctx;
    const char *name = row->name;
    int age = row->age;
    char eb[128];
    int n;

    if (rc->match(row, rc->match_ctx)) {
        rc->count++;
        if (rc->upd == NULL) {
            return 0; /* DELETE: 새 파일에 쓰지 않는다 */
        }
        if (rc->upd->set_name) {
            name = rc->upd->name;
        }
        if (rc->upd->set_age) {
            age = rc->upd->age;
        }
    }
    n = fprintf(rc->out, "%" PRId64 "\t%s\t%d\n", row->id, name, age);
    if (n >= 0 && FAIL_AT(REWRITE_FAIL_WRITE)) {
        fflush(rc->out); /* 임시 파일에 내용이 남은 채로 실패해야 정리를 확인할 수 있다 */
        n = -1;
    }
    if (n < 0) {
        db_error_setk(&rc->err, DBE_STORAGE, "임시 파일 쓰기 실패: %s (%s)", rc->tmp_path,
                      db_errstr(errno, eb, sizeof eb));
        rc->failed = 1;
        return 1;
    }
    /* id는 원래 파일에서 증가 순서였고 그 순서대로 쓰므로 중복은 없다. 실패 원인은 메모리 부족뿐이다. */
    if (bpt_insert(rc->index, row->id, rc->size) != BPT_OK) {
        db_error_setk(&rc->err, DBE_STORAGE, "메모리가 부족해 새 인덱스를 만들 수 없습니다");
        rc->failed = 1;
        return 1;
    }
    rc->size += n;
    rc->max_id = row->id;
    return 0;
}

#ifdef _WIN32
static int reopen_table_file(Table *t) {
    t->fp = fopen(t->path, "a+b");
    if (t->fp == NULL) {
        t->fd = -1;
        return -1;
    }
    t->fd = _fileno(t->fp);
    return 0;
}
#endif

int table_rewrite(Table *t, RowPred match, void *match_ctx, const RowUpdate *upd, int64_t *out_count,
                  DbError *err) {
    RewriteCtx rc;
    char eb[128];
    int saved;
#ifndef _WIN32
    FILE *nfp;
#endif

    *out_count = 0;
    /* 파일 형식을 깨는 값은 Executor 검증과 별개로 여기서도 막는다(table_append와 같은 규칙). */
    if (upd != NULL &&
        ((upd->set_name && (upd->name == NULL || upd->name[0] == '\0' || strlen(upd->name) > NAME_MAX_BYTES ||
                            strpbrk(upd->name, "\t\r\n") != NULL)) ||
         (upd->set_age && (upd->age < AGE_MIN || upd->age > AGE_MAX)))) {
        db_error_setk(err, DBE_STORAGE, "저장할 수 없는 레코드입니다");
        return -1;
    }
    memset(&rc, 0, sizeof rc);
    rc.match = match;
    rc.match_ctx = match_ctx;
    rc.upd = upd;
    rc.tmp_path = t->tmp_path;
    rc.index = bpt_create(BPT_DEFAULT_ORDER);
    if (rc.index == NULL) {
        db_error_setk(err, DBE_STORAGE, "메모리가 부족합니다");
        return -1;
    }
    rc.out = fopen(t->tmp_path, "wb");
    if (rc.out == NULL) {
        db_error_setk(err, DBE_STORAGE, "임시 파일을 만들 수 없습니다: %s (%s)", t->tmp_path,
                      db_errstr(errno, eb, sizeof eb));
        goto fail;
    }

    /* 1) 원래 파일을 처음부터 읽으며(위치 지정 읽기, 원래 FILE*는 건드리지 않음) 새 파일과 새 트리를 만든다 */
    if (scan_file(t, NULL, rewrite_row, &rc, t->file_size, NULL, NULL, err) != 0) {
        if (rc.failed) {
            *err = rc.err;
        }
        goto fail;
    }
    /* 2) 새 파일 마무리 */
    saved = fflush(rc.out) != 0 || ferror(rc.out);
    if (fclose(rc.out) != 0) {
        saved = 1;
    }
    rc.out = NULL;
    if (saved) {
        db_error_setk(err, DBE_STORAGE, "임시 파일 쓰기 실패: %s (%s)", t->tmp_path, db_errstr(errno, eb, sizeof eb));
        goto fail;
    }
    /* 3) 다음 id: 가장 큰 id의 행이 새 파일에서 사라지면 그 id를 users.next에 남긴다(DB-P4-D04).
          users.next는 "이보다 큰 id를 써라"라는 뜻뿐이라, 뒤 단계가 실패해도 남아 있어 해가 없다. */
    if (rc.max_id < t->last_id && t->next_saved < t->last_id) {
        if (write_next_file(t, t->last_id, err) != 0) {
            goto fail;
        }
        t->next_saved = t->last_id;
    }

#ifndef _WIN32
    /* 4) INSERT가 쓸 새 FILE*를 바꿔치기 전에 연다. 바꿔치기 뒤에 실패할 단계를 남기지 않기 위해서다 */
    nfp = fopen(t->tmp_path, "a+b");
    if (nfp == NULL) {
        db_error_setk(err, DBE_STORAGE, "임시 파일을 열 수 없습니다: %s (%s)", t->tmp_path,
                      db_errstr(errno, eb, sizeof eb));
        goto fail;
    }
    /* 5) 바꿔치기. rename은 한 번에 일어난다: 다른 프로세스는 옛 파일이나 새 파일 하나만 본다 */
    if (FAIL_AT(REWRITE_FAIL_RENAME) || rename(t->tmp_path, t->path) != 0) {
        saved = errno;
        fclose(nfp);
        db_error_setk(err, DBE_STORAGE, "데이터 파일 바꿔치기 실패: %s (%s)", t->path, db_errstr(saved, eb, sizeof eb));
        goto fail;
    }
    /* 6) 교체. 여기부터는 실패할 단계가 없다. 옛 FILE*는 모든 INSERT 뒤 fflush했으므로 닫기만 한다 */
    fclose(t->fp);
    t->fp = nfp;
    t->fd = fileno(nfp);
#else
    /* Windows: 열린 파일은 바꿀 수 없어 옛 FILE*를 닫고 바꾼 뒤 다시 연다(design.md "Windows (CLI만)") */
    fclose(t->fp);
    t->fp = NULL;
    t->fd = -1;
    if (FAIL_AT(REWRITE_FAIL_RENAME) || replace_file(t->tmp_path, t->path) != 0) {
        saved = errno;
        if (reopen_table_file(t) != 0) {
            remove(t->tmp_path);
            bpt_destroy(rc.index);
            db_error_setk(err, DBE_REOPEN_FAILED,
                          "데이터 파일 바꿔치기 실패 뒤 원래 파일을 다시 열지 못했습니다: %s (변경 없음, 다시 실행하면 복원)",
                          t->path);
            return -1;
        }
        db_error_setk(err, DBE_STORAGE, "데이터 파일 바꿔치기 실패: %s (%s)", t->path, db_errstr(saved, eb, sizeof eb));
        goto fail;
    }
#endif
    bpt_destroy(t->index);
    t->index = rc.index;
    t->file_size = rc.size;
    *out_count = rc.count;
#ifdef _WIN32
    /* 바꿔치기 뒤라 되돌릴 수 없다. 실패하면 엔진을 중단 상태로 둔다(파일은 온전, 다시 실행하면 복원) */
    if (FAIL_AT(REWRITE_FAIL_REOPEN) || reopen_table_file(t) != 0) {
        if (t->fp != NULL) {
            fclose(t->fp);
            t->fp = NULL;
            t->fd = -1;
        }
        db_error_setk(err, DBE_REOPEN_FAILED,
                      "데이터 파일을 바꾼 뒤 다시 열지 못했습니다: %s (변경은 파일에 반영됨, 다시 실행하면 복원)", t->path);
        return -1;
    }
#endif
    return 0;

fail:
    if (rc.out != NULL) {
        fclose(rc.out);
    }
    remove(t->tmp_path);
    bpt_destroy(rc.index);
    return -1;
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
    rc = t->fp != NULL ? fclose(t->fp) : 0;
    if (rc != 0) {
        db_error_setk(err, DBE_STORAGE, "데이터 파일 닫기 실패: %s (%s)", t->path, db_errstr(errno, eb, sizeof eb));
    }
    table_free(t);
    return rc == 0 ? 0 : -1;
}
