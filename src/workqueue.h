#ifndef MINIDB_WORKQUEUE_H
#define MINIDB_WORKQUEUE_H

#include <pthread.h>
#include <stddef.h>

/*
 * 작업 큐 (Phase 3, P3-03): 크기가 고정된 고리 버퍼에 연결 fd를 담는다.
 *
 *   accept 스레드 ── wq_try_push ──▶ [ fd fd fd . . ] ──▶ wq_pop ── 워커 N개
 *                     (기다리지 않음)                      (빌 때까지 잠듦)
 *
 * - wq_try_push는 기다리지 않는다. 가득 차면 바로 실패해서 accept 스레드가 503 BUSY를 보내게 한다.
 * - wq_close는 큐를 닫고, 아직 아무 워커도 꺼내지 않은 fd를 한꺼번에 돌려준다(종료 때 503 SHUTTING_DOWN용).
 *   닫힌 뒤의 wq_pop은 기다리지 않고 -1을 돌려주므로 워커는 루프를 끝낸다.
 * - 잠금은 큐 안의 mutex 하나뿐이고, 그 안에서 다른 잠금을 잡지 않는다(spec C7).
 * - 서버만 쓰지만 pthread만 쓰므로 Windows 단위 테스트에서도 빌드·검사한다.
 */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    int *items;
    size_t cap;
    size_t head; /* 다음에 꺼낼 위치 */
    size_t len;
    int closed;
} WorkQueue;

/* 0 성공, -1 메모리 부족 또는 cap == 0 */
int wq_init(WorkQueue *q, size_t cap);
void wq_destroy(WorkQueue *q);

/* 0 넣음, 1 가득 참, 2 이미 닫힘. 기다리지 않는다. */
int wq_try_push(WorkQueue *q, int fd);

/* 0 꺼냄(*fd), -1 닫힘. 비어 있으면 넣을 때까지 기다린다. */
int wq_pop(WorkQueue *q, int *fd);

/* 큐를 닫고 남은 fd를 rest에 넣는다(rest는 cap개 이상). 돌려준 개수를 반환한다. 두 번째 호출부터는 0. */
size_t wq_close(WorkQueue *q, int *rest);

#endif
