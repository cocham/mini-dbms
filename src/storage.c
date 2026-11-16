#include "storage.h"

#include "bptree.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) mkdir((path), 0755)
#endif

#define TABLE_FILE "users.tsv"
#define ID_MAX_DIGITS 19  /* INT64_MAX = 9223372036854775807 */
#define AGE_MAX_DIGITS 3
/* 가장 긴 레코드: 19 + 1 + 255 + 1 + 3 = 279바이트. 넉넉히 잡고 넘으면 손상으로 본다. */
#define LINE_CAP 320

struct Table {
    FILE *fp;      /* "a+b": 읽기는 아무 위치에서, 쓰기는 항상 파일 끝 */
    char *path;
    int64_t last_id; /* 파일에 있는 최대 ID. 빈 테이블이면 0 */
    BpTree *index;   /* id → 그 레코드 줄이 시작하는 바이트 위치. 메모리에만 있다 */
};

/* ---------- 레코드 읽기 ---------- */

/* 1: 한 줄 읽음(LF 제외), 0: 파일 끝, -1: 형식 손상(*why 설정), -2: 읽기 오류 */
static int read_line(FILE *fp, char *buf, size_t cap, size_t *out_len, const char **why) {
    size_t n = 0;
    int c;

    while ((c = getc(fp)) != EOF) {
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
    if (ferror(fp)) {
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

static int scan_file(Table *t, BpTree *build, RowFn fn, void *ctx, DbError *err);

/* ---------- 공개 함수 ---------- */

int table_open(const char *data_dir, Table **out, DbError *err) {
    Table *t;
    size_t path_len;
    int saved_errno;

    *out = NULL;
    t = calloc(1, sizeof *t);
    if (t == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    path_len = strlen(data_dir) + 1 + strlen(TABLE_FILE) + 1;
    t->path = malloc(path_len);
    if (t->path == NULL) {
        db_error_set(err, "메모리가 부족합니다");
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
            db_error_set(err, "데이터 파일을 열 수 없습니다: %s (%s)", t->path, strerror(saved_errno));
            goto fail;
        }
    }

    /* 시작할 때 한 번만 전체를 검증하며 최대 ID를 복원하고, id 인덱스를 메모리에 다시 만든다. */
    t->index = bpt_create(BPT_DEFAULT_ORDER);
    if (t->index == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        goto fail;
    }
    if (scan_file(t, t->index, NULL, NULL, err) != 0) {
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
 * 파일을 처음부터 끝까지 한 줄씩 읽어 검증하고 fn을 호출한다.
 * build가 NULL이 아니면 각 레코드의 (id, 줄 시작 바이트 위치)를 build 인덱스에 넣는다.
 * 위치는 ftell 대신 직접 센다: 줄 길이 + LF 1바이트. 바이너리 모드라 바이트 수가 정확하다.
 */
static int scan_file(Table *t, BpTree *build, RowFn fn, void *ctx, DbError *err) {
    char line[LINE_CAP];
    size_t len;
    long lineno = 0;
    int64_t prev_id = 0;
    int64_t offset = 0; /* 다음에 읽을 줄이 시작하는 바이트 위치 */

    /* 쓰기 직후 읽기로 바꿀 때는 flush/seek가 필요하다(C 표준 규칙). */
    if (fflush(t->fp) != 0 || fseek(t->fp, 0, SEEK_SET) != 0) {
        db_error_set(err, "데이터 파일 읽기 준비 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    for (;;) {
        Row row;
        const char *why = NULL;
        int64_t line_start = offset;
        int r = read_line(t->fp, line, sizeof line, &len, &why);

        if (r == 0) {
            break;
        }
        lineno++;
        if (r == -2) {
            db_error_set(err, "데이터 파일 읽기 실패: %s", t->path);
            return -1;
        }
        if (r == -1 || parse_record(line, len, prev_id, &row, &why) != 0) {
            db_error_set(err, "데이터 파일 손상: %s %ld번째 줄: %s", t->path, lineno, why);
            return -1;
        }
        offset += (int64_t)len + 1;
        prev_id = row.id;
        if (build != NULL && bpt_insert(build, row.id, line_start) != BPT_OK) {
            /* id는 위에서 증가 순서를 확인했으므로 중복은 없다. 남은 원인은 메모리 부족뿐이다. */
            db_error_set(err, "메모리가 부족해 인덱스를 만들 수 없습니다");
            return -1;
        }
        if (fn != NULL && fn(&row, ctx) != 0) {
            db_error_set(err, "조회 결과 처리에 실패했습니다");
            return -1;
        }
    }
    t->last_id = prev_id;
    return 0;
}

int table_scan(Table *t, RowFn fn, void *ctx, DbError *err) {
    return scan_file(t, NULL, fn, ctx, err);
}

int table_find_by_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err) {
    char line[LINE_CAP];
    size_t len;
    int64_t offset;
    const char *why = NULL;
    Row row;
    int r;

    /* 1) B+ 트리에서 id → 바이트 위치. 없으면 파일을 읽지 않고 끝난다. */
    if (!bpt_search(t->index, id, &offset)) {
        return 0;
    }
    if (offset > LONG_MAX) {
        db_error_set(err, "데이터 파일이 너무 커서 위치로 이동할 수 없습니다: %s", t->path);
        return -1;
    }
    /* 2) 그 위치로 바로 이동해 한 줄만 읽는다. */
    if (fflush(t->fp) != 0 || fseek(t->fp, (long)offset, SEEK_SET) != 0) {
        db_error_set(err, "데이터 파일 위치 이동 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    r = read_line(t->fp, line, sizeof line, &len, &why);
    if (r == -2) {
        db_error_set(err, "데이터 파일 읽기 실패: %s", t->path);
        return -1;
    }
    /* 3) 읽은 줄이 정말 그 id인지 확인한다. 열린 뒤 파일이 바뀌었으면 여기서 드러난다. */
    if (r != 1 || parse_record(line, len, 0, &row, &why) != 0 || row.id != id) {
        db_error_set(err, "인덱스와 데이터 파일이 일치하지 않습니다: %s (id %" PRId64 ", 위치 %" PRId64 ")",
                     t->path, id, offset);
        return -1;
    }
    if (fn != NULL && fn(&row, ctx) != 0) {
        db_error_set(err, "조회 결과 처리에 실패했습니다");
        return -1;
    }
    return 0;
}

/* table_find_id_range가 bpt_range 콜백으로 넘기는 상태 */
typedef struct {
    Table *t;
    RowFn fn;
    void *ctx;
    DbError *err;
    int64_t next_pos; /* 파일 포인터가 지금 있는 위치. -1이면 아직 이동 전 */
} RangeRead;

/*
 * 리프에서 (id, 위치)를 하나 받을 때마다 그 줄을 읽는다.
 * id가 증가 순서로 저장되므로 범위 안의 줄은 파일에서도 이어져 있다. 그래서 보통 첫 줄에서만
 * fseek하고 나머지는 이어서 읽는다. 위치가 이어지지 않으면(예상 밖 상황) 그 위치로 다시 이동한다.
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
        if (offset > LONG_MAX) {
            db_error_set(rr->err, "데이터 파일이 너무 커서 위치로 이동할 수 없습니다: %s", rr->t->path);
            return 1;
        }
        if ((rr->next_pos < 0 && fflush(rr->t->fp) != 0) ||
            fseek(rr->t->fp, (long)offset, SEEK_SET) != 0) {
            db_error_set(rr->err, "데이터 파일 위치 이동 실패: %s (%s)", rr->t->path, strerror(errno));
            return 1;
        }
    }
    r = read_line(rr->t->fp, line, sizeof line, &len, &why);
    if (r == -2) {
        db_error_set(rr->err, "데이터 파일 읽기 실패: %s", rr->t->path);
        return 1;
    }
    /* table_find_by_id와 같은 재확인: 열린 뒤 파일이 바뀌었으면 엉뚱한 행 대신 오류 */
    if (r != 1 || parse_record(line, len, 0, &row, &why) != 0 || row.id != id) {
        db_error_set(rr->err, "인덱스와 데이터 파일이 일치하지 않습니다: %s (id %" PRId64 ", 위치 %" PRId64 ")",
                     rr->t->path, id, offset);
        return 1;
    }
    rr->next_pos = offset + (int64_t)len + 1;
    if (rr->fn != NULL && rr->fn(&row, rr->ctx) != 0) {
        db_error_set(rr->err, "조회 결과 처리에 실패했습니다");
        return 1;
    }
    return 0;
}

int table_find_id_range(Table *t, int64_t lo, int64_t hi, RowFn fn, void *ctx, DbError *err) {
    RangeRead rr;

    rr.t = t;
    rr.fn = fn;
    rr.ctx = ctx;
    rr.err = err;
    rr.next_pos = -1;
    return bpt_range(t->index, lo, hi, range_visit, &rr) == 0 ? 0 : -1;
}

int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err) {
    size_t name_len = strlen(name);
    int64_t id;
    long offset;

    /* 파일 형식을 깨는 값은 Executor 검증과 별개로 여기서도 막는다. */
    if (name_len == 0 || name_len > NAME_MAX_BYTES || strpbrk(name, "\t\r\n") != NULL ||
        age < AGE_MIN || age > AGE_MAX) {
        db_error_set(err, "저장할 수 없는 레코드입니다");
        return -1;
    }
    if (t->last_id == INT64_MAX) {
        db_error_set(err, "더 이상 부여할 ID가 없습니다");
        return -1;
    }
    id = t->last_id + 1;

    /* 새 줄이 시작할 위치(= 현재 파일 크기)를 기록 전에 구해 둔다. */
    if (fseek(t->fp, 0, SEEK_END) != 0 || (offset = ftell(t->fp)) < 0 ||
        fprintf(t->fp, "%" PRId64 "\t%s\t%d\n", id, name, age) < 0 ||
        fflush(t->fp) != 0) {
        db_error_set(err, "데이터 파일 쓰기 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    t->last_id = id;
    /* 파일에 기록한 뒤 인덱스에 등록한다. 실패는 메모리 부족뿐이고, 이때 레코드는 파일에 남는다.
     * 다음 실행의 table_open이 파일에서 인덱스를 다시 만들므로 파일 기준으로는 일관된다. */
    if (bpt_insert(t->index, id, (int64_t)offset) != BPT_OK) {
        db_error_set(err, "메모리가 부족해 id %" PRId64 "를 인덱스에 등록하지 못했습니다 "
                     "(레코드는 저장됨, 다시 실행하면 인덱스가 재구성됨)", id);
        return -1;
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return 0;
}

int table_index_height(const Table *t) { return bpt_height(t->index); }

/* 모든 레코드가 인덱스에 등록되므로(table_open·table_append) 키 수가 곧 행 수다. */
int64_t table_row_count(const Table *t) { return (int64_t)bpt_size(t->index); }

int table_close(Table *t, DbError *err) {
    int rc;

    if (t == NULL) {
        return 0;
    }
    rc = fclose(t->fp);
    if (rc != 0) {
        db_error_set(err, "데이터 파일 닫기 실패: %s (%s)", t->path, strerror(errno));
    }
    bpt_destroy(t->index);
    free(t->path);
    free(t);
    return rc == 0 ? 0 : -1;
}
