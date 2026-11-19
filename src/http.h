#ifndef MINIDB_HTTP_H
#define MINIDB_HTTP_H

#include <stddef.h>

/*
 * HTTP/1.1의 필요한 부분만 (Phase 3, 서버 전용, POSIX). 연결 하나에 요청 하나.
 * 지원: 요청 줄, 헤더, Content-Length 본문, Expect: 100-continue.
 * 미지원: chunked 등 Transfer-Encoding(400으로 거절), keep-alive(항상 Connection: close), HTTPS.
 *
 * 기한 (spec "느린 클라이언트와 기한"): 소켓 호출 하나 5초, 요청 수신 전체 10초, 응답 전송 전체 10초.
 */

#define HTTP_MAX_HEADER (8 * 1024)    /* 요청 줄 + 헤더 + 빈 줄 */
#define HTTP_MAX_BODY (1024 * 1024)   /* 요청 본문 */
#define HTTP_CALL_TIMEOUT_MS 5000
#define HTTP_RECV_DEADLINE_MS 10000
#define HTTP_SEND_DEADLINE_MS 10000

#define HTTP_MAX_QUERY 256 /* 보관하는 쿼리 문자열의 최대 바이트 (P5-02, GET /users) */

typedef struct {
    char method[16];
    char path[256]; /* 쿼리 문자열(?...) 제외 */
    /* '?' 뒤의 쿼리 문자열. '?'가 없거나 비었으면 "". HTTP_MAX_QUERY를 넘으면 ""로 두고 query_over = 1.
       보이는 ASCII만 들어 있다(요청 대상 검사). 디코딩(%XX)은 하지 않는다. 쓰는 쪽(/users)이 해석한다 */
    char query[HTTP_MAX_QUERY + 1];
    int query_over;
    char *body;     /* malloc, 끝에 NUL을 붙여 둔다. 본문 안의 NUL은 그대로 남는다 */
    size_t body_len;
} HttpRequest;

/*
 * fd에서 요청 하나를 읽는다.
 *  0   성공. 다 쓰면 http_request_free.
 *  >0  돌려줄 HTTP 상태 코드(400, 408, 413, 431, 500). why에 사람용 설명
 *  -1  응답할 수 없음(클라이언트가 먼저 끊음, 읽기 오류)
 */
int http_read_request(int fd, HttpRequest *req, char *why, size_t why_cap);
void http_request_free(HttpRequest *req);

/*
 * 응답 하나를 보낸다. 본문은 여러 조각(parts)을 이어 붙인 것이고 Content-Length는 그 합이다.
 * extra_headers는 NULL이거나 "Allow: GET\r\n"처럼 CRLF로 끝나는 헤더 줄들.
 * 0 성공, -1 실패(끊김·기한 초과).
 */
int http_send_response(int fd, int status, const char *extra_headers, const char *const parts[],
                       const size_t lens[], int nparts);

const char *http_reason(int status);

/* 쓰기 쪽을 닫고, 클라이언트가 보내던 나머지를 잠깐(최대 1초) 읽어 버린 뒤 닫는다.
 * 읽지 않은 데이터가 남은 채로 닫으면 응답보다 연결 재설정이 먼저 갈 수 있어서다. */
void http_close(int fd);

#endif
