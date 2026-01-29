/**
 * @file log.h
 * @brief 日志模块头文件
 * 
 * 提供简单的日志打印功能，支持三个级别：INFO、WARN、ERROR。
 * 日志格式：[级别 时间戳] 消息内容
 * 
 * 使用示例：
 *   LOGI("Server started on port %d", port);
 *   LOGW("Connection timeout after %d seconds", timeout);
 *   LOGE("Failed to open file: %s", filename);
 */
#pragma once

#include <stdio.h>
#include <time.h>

/**
 * @brief 获取当前时间戳字符串
 * 
 * 格式：HH:MM:SS.mmm（时:分:秒.毫秒）
 * 使用 CLOCK_REALTIME 获取实时时间。
 * 
 * 注意：返回静态缓冲区，非线程安全（但对于日志通常可接受）。
 * 
 * @return const char* 时间戳字符串
 */
static inline const char *log_timestamp(void)
{
    static char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_now;
    localtime_r(&ts.tv_sec, &tm_now);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (int)(ts.tv_nsec / 1000000));
    return buf;
}

/**
 * @brief 日志级别枚举
 */
enum {
    LOG_LEVEL_INFO  = 1,  /**< 信息级别（正常运行信息） */
    LOG_LEVEL_WARN  = 2,  /**< 警告级别（潜在问题） */
    LOG_LEVEL_ERROR = 3,  /**< 错误级别（操作失败） */
};

/**
 * @brief 日志打印函数
 * 
 * 实际的打印实现在 log.c 中。
 * 
 * @param level 日志级别
 * @param fmt   格式化字符串
 * @param ...   可变参数
 */
void log_print(int level, const char *fmt, ...);

/** 
 * @brief 信息级别日志宏
 * 
 * 用法：LOGI("message %s %d", str, num);
 */
#define LOGI(fmt, ...) log_print(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)

/** 
 * @brief 警告级别日志宏
 * 
 * 用法：LOGW("warning: %s", msg);
 */
#define LOGW(fmt, ...) log_print(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)

/** 
 * @brief 错误级别日志宏
 * 
 * 用法：LOGE("error: %s failed", func_name);
 */
#define LOGE(fmt, ...) log_print(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
