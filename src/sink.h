/**
 * @file sink.h
 * @brief 编码输出 Sink 模块头文件
 * 
 * Sink（下游/落地端）负责将编码后的数据写入目标（文件、管道等）。
 * 当前实现支持：
 * - ENC_SINK_FILE: 写入本地文件
 * - ENC_SINK_PIPE_FFMPEG: 预留，后续用于 RTMP 推流等场景
 */
#pragma once

#include <stdio.h>
#include <stdint.h>

/**
 * @brief Sink 类型枚举
 */
typedef enum {
    ENC_SINK_NONE = 0,         /**< 无输出（静默丢弃） */
    ENC_SINK_FILE,             /**< 写入本地文件（当前主要实现） */
    ENC_SINK_PIPE_FFMPEG,      /**< 预留：通过管道传给 FFmpeg 进行推流等 */
} EncSinkType;

/**
 * @brief Sink 上下文结构体
 */
typedef struct {
    EncSinkType type;          /**< Sink 类型 */
    char target[512];          /**< 目标路径/地址 */

    FILE *file_fp;             /**< 文件句柄（ENC_SINK_FILE 时使用） */
    FILE *pipe_fp;             /**< 管道句柄（ENC_SINK_PIPE_FFMPEG 时使用） */

} EncSink;

/**
 * @brief 初始化 Sink
 * 
 * 设置 Sink 类型和目标，不执行实际 I/O。
 * 
 * @param sink   Sink 指针
 * @param type   Sink 类型
 * @param target 目标路径（可为 NULL）
 * @return int   0 成功，-1 失败
 */
int enc_sink_init(EncSink *sink, EncSinkType type, const char *target);

/**
 * @brief 打开 Sink
 * 
 * 执行实际的 I/O 打开操作（如 fopen）。
 * 
 * @param sink Sink 指针
 * @return int 0 成功，-1 失败
 */
int enc_sink_open(EncSink *sink);

/**
 * @brief 写入数据到 Sink
 * 
 * @param sink Sink 指针
 * @param data 数据指针
 * @param size 数据长度（字节）
 * @return int 0 成功，-1 失败
 */
int enc_sink_write(EncSink *sink, const uint8_t *data, size_t size);

/**
 * @brief 关闭 Sink
 * 
 * 关闭文件/管道句柄并释放资源。
 * 
 * @param sink Sink 指针
 */
void enc_sink_close(EncSink *sink);