#include "workqueue.h"

#include <stdlib.h>

int wq_init(WorkQueue *q, size_t cap) {
    q->items = NULL;
    q->cap = cap;
    q->head = 0;
    q->len = 0;
    q->closed = 0;
    if (cap == 0) {
        return -1;
    }
    q->items = malloc(cap * sizeof *q->items);
    if (q->items == NULL) {
        return -1;
    }
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->items);
        q->items = NULL;
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q->items);
        q->items = NULL;
        return -1;
    }
    return 0;
}

void wq_destroy(WorkQueue *q) {
    if (q->items == NULL) {
        return;
    }
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
    free(q->items);
    q->items = NULL;
}

int wq_try_push(WorkQueue *q, int fd) {
    int rc;

    pthread_mutex_lock(&q->mu);
    if (q->closed) {
        rc = 2;
    } else if (q->len == q->cap) {
        rc = 1;
    } else {
        q->items[(q->head + q->len) % q->cap] = fd;
        q->len++;
        rc = 0;
        pthread_cond_signal(&q->not_empty); /* 잠든 워커 하나를 깨운다 */
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int wq_pop(WorkQueue *q, int *fd) {
    int rc;

    pthread_mutex_lock(&q->mu);
    while (q->len == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu); /* 깨어나도 조건을 다시 본다(가짜 깨어남) */
    }
    if (q->closed) {
        rc = -1; /* 남은 fd는 wq_close가 이미 가져갔다 */
    } else {
        *fd = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->len--;
        rc = 0;
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

size_t wq_close(WorkQueue *q, int *rest) {
    size_t n = 0;

    pthread_mutex_lock(&q->mu);
    if (!q->closed) {
        q->closed = 1;
        while (q->len > 0) {
            rest[n++] = q->items[q->head];
            q->head = (q->head + 1) % q->cap;
            q->len--;
        }
        pthread_cond_broadcast(&q->not_empty); /* 잠든 워커를 모두 깨워 끝내게 한다 */
    }
    pthread_mutex_unlock(&q->mu);
    return n;
}
