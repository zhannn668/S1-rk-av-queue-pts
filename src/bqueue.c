/**
 * @file bqueue.c
 * @brief 阻塞队列（Blocking Queue）实现
 * 
 * 提供一个线程安全的有界队列，支持：
 * - 阻塞式 push/pop：当队列满/空时阻塞等待
 * - 非阻塞式 try_push：队列满时立即返回
 * - close 操作：关闭队列并唤醒所有等待线程
 * 
 * 典型用途：生产者-消费者模式的线程间通信。
 * 
 * 实现原理：
 * - 使用环形缓冲区（循环数组）存储元素
 * - 使用 pthread_mutex 保护临界区
 * - 使用 pthread_cond 实现阻塞等待
 */
#include "rkav/bqueue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 初始化阻塞队列
 * 
 * 分配内存并初始化互斥锁和条件变量。
 * 
 * @param q        队列指针
 * @param capacity 队列容量（最大元素个数）
 * @return int     0 成功，-1 失败
 */
int bq_init(BQueue *q, size_t capacity)
{
    if (!q || capacity == 0)
        return -1;

    memset(q, 0, sizeof(*q));

    /* 分配存储 void* 指针的数组 */
    q->items = (void **)calloc(capacity, sizeof(void *));
    if (!q->items)
        return -1;

    q->capacity = capacity;
    q->size = 0;
    q->head = 0;      /* 出队位置 */
    q->tail = 0;      /* 入队位置 */
    q->closed = 0;

    /* 初始化同步原语 */
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);  /* 队列非空条件 */
    pthread_cond_init(&q->not_full, NULL);   /* 队列非满条件 */

    return 0;
}

/**
 * @brief 关闭队列
 * 
 * 设置关闭标志并唤醒所有等待的线程。
 * 关闭后：
 * - push 会立即返回失败
 * - pop 会在队列清空后返回 0（表示结束）
 * 
 * @param q 队列指针
 */
void bq_close(BQueue *q)
{
    if (!q) return;
    
    pthread_mutex_lock(&q->mtx);
    q->closed = 1;
    /* 广播唤醒所有等待的线程 */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
}

/**
 * @brief 销毁队列并释放资源
 * 
 * 注意：不会 free 队列中残留的元素，调用者应在 close 后先 drain 队列。
 * 
 * @param q 队列指针
 */
void bq_destroy(BQueue *q)
{
    if (!q) return;

    /* 先获取锁，取出 items 指针后释放 */
    pthread_mutex_lock(&q->mtx);
    void **items = q->items;
    q->items = NULL;
    pthread_mutex_unlock(&q->mtx);

    if (items) free(items);

    /* 销毁同步原语 */
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    memset(q, 0, sizeof(*q));
}

/**
 * @brief 阻塞式入队
 * 
 * 当队列满时阻塞等待，直到有空位或队列被关闭。
 * 
 * @param q    队列指针
 * @param item 要入队的元素（void* 指针）
 * @return int 0 成功，-1 失败（队列已关闭）
 */
int bq_push(BQueue *q, void *item)
{
    if (!q) return -1;
    
    pthread_mutex_lock(&q->mtx);

    /* 等待队列非满 */
    while (!q->closed && q->size == q->capacity) {
        pthread_cond_wait(&q->not_full, &q->mtx);
    }

    /* 检查是否因关闭而退出等待 */
    if (q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }

    /* 入队：写入 tail 位置，tail 前进 */
    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;  /* 环形递增 */
    q->size++;

    /* 通知等待出队的线程 */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

/**
 * @brief 非阻塞式入队
 * 
 * 尝试入队，队列满时立即返回而不阻塞。
 * 
 * @param q    队列指针
 * @param item 要入队的元素
 * @return int 0 成功，1 队列满（需要重试或丢弃），-1 失败（队列已关闭）
 */
int bq_try_push(BQueue *q, void *item)
{
    if (!q) return -1;
    
    pthread_mutex_lock(&q->mtx);

    if (q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    if (q->size == q->capacity) {
        /* 队列已满，立即返回 */
        pthread_mutex_unlock(&q->mtx);
        return 1;
    }

    /* 入队 */
    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

/**
 * @brief 阻塞式出队
 * 
 * 当队列空时阻塞等待，直到有元素或队列被关闭且清空。
 * 
 * @param q   队列指针
 * @param out 输出：取出的元素
 * @return int 1 成功取出元素，0 队列已关闭且为空，-1 失败
 */
int bq_pop(BQueue *q, void **out)
{
    if (!q || !out) return -1;
    
    pthread_mutex_lock(&q->mtx);

    /* 等待队列非空 */
    while (!q->closed && q->size == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }

    /* 检查是否因关闭且为空而退出 */
    if (q->size == 0 && q->closed) {
        pthread_mutex_unlock(&q->mtx);
        return 0;  /* 正常结束标志 */
    }

    /* 出队：读取 head 位置，head 前进 */
    void *item = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;  /* 环形递增 */
    q->size--;

    /* 通知等待入队的线程 */
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);

    *out = item;
    return 1;
}

/**
 * @brief 获取队列当前元素个数
 * 
 * @param q 队列指针
 * @return size_t 当前元素个数
 */
size_t bq_size(BQueue *q)
{
    if (!q) return 0;
    
    pthread_mutex_lock(&q->mtx);
    size_t s = q->size;
    pthread_mutex_unlock(&q->mtx);
    
    return s;
}

/**
 * @brief 获取队列容量
 * 
 * @param q 队列指针
 * @return size_t 队列最大容量
 */
size_t bq_capacity(BQueue *q)
{
    if (!q) return 0;
    return q->capacity;
}
