/**
 * @file av_stats.h
 * @brief 音视频统计模块头文件
 * 
 * 提供多线程安全的统计计数器，用于实时监控系统运行状态。
 * 所有计数器使用 C11 原子类型，支持多线程并发更新。
 * 
 * 使用方法：
 * 1. 调用 av_stats_init() 初始化
 * 2. 在各工作线程中调用 av_stats_inc_* / av_stats_add_* 累加计数
 * 3. 在统计线程中每秒调用 av_stats_tick_print() 打印并重置计数器
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音视频统计结构体
 * 
 * 所有字段都是原子类型，保证多线程安全。
 * 设计为"每秒窗口"模式：累加一秒后由 tick_print 读取并清零。
 */
typedef struct {
    atomic_uint_fast64_t video_frames;  /**< 过去 1 秒编码成功的视频帧数 */
    atomic_uint_fast64_t enc_bytes;     /**< 过去 1 秒编码输出的字节数 */
    atomic_uint_fast64_t audio_chunks;  /**< 过去 1 秒写入的音频块数 */
    atomic_uint_fast64_t drop_count;    /**< 过去 1 秒检测到的丢帧/异常次数 */
} AvStats;

/**
 * @brief 初始化统计结构体
 * 
 * 将所有计数器清零。
 * 
 * @param s 统计对象指针
 */
void av_stats_init(AvStats *s);

/**
 * @brief 打印并重置统计信息（每秒调用一次）
 * 
 * 读取各计数器的值并清零，打印帧率、码率等指标。
 * 假设调用周期为 1 秒。
 * 
 * @param s 统计对象指针
 */
void av_stats_tick_print(AvStats *s);

/**
 * @brief 视频帧计数 +1
 * 
 * 编码成功一帧后调用。
 * 
 * @param s 统计对象指针
 */
static inline void av_stats_inc_video_frame(AvStats *s) {
    atomic_fetch_add_explicit(&s->video_frames, 1, memory_order_relaxed);
}

/**
 * @brief 累加编码输出字节数
 * 
 * @param s     统计对象指针
 * @param bytes 本次编码输出的字节数
 */
static inline void av_stats_add_enc_bytes(AvStats *s, uint64_t bytes) {
    atomic_fetch_add_explicit(&s->enc_bytes, bytes, memory_order_relaxed);
}

/**
 * @brief 音频块计数 +1
 * 
 * 写入一个音频块后调用。
 * 
 * @param s 统计对象指针
 */
static inline void av_stats_inc_audio_chunk(AvStats *s) {
    atomic_fetch_add_explicit(&s->audio_chunks, 1, memory_order_relaxed);
}

/**
 * @brief 累加丢帧/异常计数
 * 
 * @param s 统计对象指针
 * @param n 丢帧/异常次数
 */
static inline void av_stats_add_drop(AvStats *s, uint64_t n) {
    atomic_fetch_add_explicit(&s->drop_count, n, memory_order_relaxed);
}

#ifdef __cplusplus
}
#endif
