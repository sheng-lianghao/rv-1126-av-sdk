#include "safe_queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void safe_queue_init(SafeQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void safe_queue_destroy(SafeQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    /* 释放队列中残留的条目，防止内存泄漏 */
    while (q->count > 0) {
        QueueItem *item = &q->items[q->head];
        free(item->data);
        item->data = NULL;
        q->head = (q->head + 1) % SAFE_QUEUE_MAX;
        q->count--;
    }
    pthread_mutex_unlock(&q->mutex);

    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->mutex);
}

int safe_queue_push(SafeQueue *q, void *data, size_t size, int64_t pts, int64_t dts)
{
    pthread_mutex_lock(&q->mutex);

    /* 若队列已关闭，拒绝新条目 */
    while (q->count >= SAFE_QUEUE_MAX && !q->eof)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (q->eof) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    QueueItem *slot = &q->items[q->tail];
    slot->data = data;
    slot->size = size;
    slot->pts  = pts;
    slot->dts  = dts;
    q->tail = (q->tail + 1) % SAFE_QUEUE_MAX;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int safe_queue_pop(SafeQueue *q, QueueItem *out)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->eof)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0 && q->eof) {
        pthread_mutex_unlock(&q->mutex);
        return 1;  /* eof + 空 */
    }

    *out = q->items[q->head];
    q->head = (q->head + 1) % SAFE_QUEUE_MAX;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int safe_queue_peek(SafeQueue *q, int64_t *pts_out)
{
    pthread_mutex_lock(&q->mutex);

    if (q->count == 0 && q->eof) {
        *pts_out = INT64_MAX;
        pthread_mutex_unlock(&q->mutex);
        return 1;
    }

    /* 若还没有数据但生产者未结束，等一段时间 */
    if (q->count == 0) {
        /* 非阻塞 peek：直接返回 INT64_MAX 让调用方稍后重试 */
        *pts_out = INT64_MAX;
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    *pts_out = q->items[q->head].pts;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void safe_queue_signal_eof(SafeQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->eof = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}
