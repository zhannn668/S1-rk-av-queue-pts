/**
 * @file time.c
 * @brief 时间工具模块实现
 * 
 * 提供高精度单调时钟相关的时间函数。
 * 使用 CLOCK_MONOTONIC 保证时间只会单调递增，不受系统时间调整影响。
 */
#include "rkav/time.h"

#include <time.h>

/**
 * @brief 获取当前单调时钟时间（微秒）
 * 
 * 使用 CLOCK_MONOTONIC 时钟源：
 * - 单调递增，不会因系统时间调整而跳变
 * - 适合用于计算时间间隔、PTS 时间戳等
 * 
 * 返回值从系统启动开始计时，但具体起点不重要，
 * 重要的是相邻调用之间的差值。
 * 
 * @return uint64_t 当前时间（微秒）
 */
uint64_t rkav_now_monotonic_us(void)
{
    struct timespec ts;
    
    /* 获取单调时钟时间 */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    
    /* 转换为微秒：秒 × 1000000 + 纳秒 / 1000 */
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}
