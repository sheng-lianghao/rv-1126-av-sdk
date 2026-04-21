#ifndef SAFE_QUEUE_H
#define SAFE_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define SAFE_QUEUE_MAX 64

typedef struct {
    void   *data;   /* 堆分配的载荷指针，队列拥有所有权 */
    size_t  size;   /* data 的字节长度（AVPacket 队列中为 0，data 即 AVPacket*） */
    int64_t pts;    /* 展示时间戳（原始帧队列：纳秒；packet 队列：编码器 timebase 单位） */
    int64_t dts;    /* 解码时间戳 */
} QueueItem;

typedef struct {
    QueueItem       items[SAFE_QUEUE_MAX];
    int             head;       /* 下一个读取位置 */
    int             tail;       /* 下一个写入位置 */
    int             count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             eof;        /* 生产者设为 1，通知消费者无更多数据 */
} SafeQueue;

void safe_queue_init(SafeQueue *q);
void safe_queue_destroy(SafeQueue *q);

/* 推入一个条目（取得 data 所有权）。返回 0 成功，-1 队列已关闭 */
int  safe_queue_push(SafeQueue *q, void *data, size_t size, int64_t pts, int64_t dts);

/* 阻塞弹出。返回 0 成功，1 表示 eof 且队列已空 */
int  safe_queue_pop(SafeQueue *q, QueueItem *out);

/* 非破坏性查看队首的 pts；若 eof 且空则写入 INT64_MAX。返回 0 有数据，1 eof+空 */
int  safe_queue_peek(SafeQueue *q, int64_t *pts_out);

/* 生产者调用：标记结束，唤醒所有阻塞的消费者 */
void safe_queue_signal_eof(SafeQueue *q);

#endif /* SAFE_QUEUE_H */