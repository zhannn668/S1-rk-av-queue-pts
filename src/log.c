/**
 * @file log.c
 * @brief 日志模块实现
 * 
 * 实现日志打印函数，输出到 stderr。
 * 格式：[级别 时间戳] 消息内容
 */
#include "log.h"
#include <stdarg.h>

/**
 * @brief 日志打印函数
 * 
 * 根据日志级别添加前缀标识，并附带时间戳。
 * 输出到 stderr 以便与正常输出分离。
 * 
 * @param level 日志级别（LOG_LEVEL_INFO/WARN/ERROR）
 * @param fmt   printf 风格的格式化字符串
 * @param ...   可变参数
 */
void log_print(int level, const char *fmt, ...)
{
    /* 根据级别确定前缀字符 */
    const char *lv = "I";  /* 默认 INFO */
    if (level == LOG_LEVEL_WARN)  lv = "W";
    if (level == LOG_LEVEL_ERROR) lv = "E";

    /* 打印级别和时间戳 */
    fprintf(stderr, "[%s %s] ", lv, log_timestamp());

    /* 打印用户消息 */
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    /* 换行 */
    fprintf(stderr, "\n");
}
