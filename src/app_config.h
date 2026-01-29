/**
 * @file app_config.h
 * @brief 应用配置模块头文件
 * 
 * 定义应用程序的配置结构体和配置解析接口。
 * 支持从命令行参数加载配置，也提供默认配置值。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用配置结构体
 * 
 * 包含视频采集、音频采集、编码参数和输出配置等所有运行时参数。
 * 程序启动时由 app_config_load_default() 初始化默认值，
 * 然后由 app_config_parse_args() 解析命令行参数覆盖。
 */
typedef struct {
    /* ============ 视频相关配置 ============ */
    
    const char *video_device;   /**< V4L2 视频设备节点路径，例如 "/dev/video0" */
    int         width;          /**< 采集宽度（像素） */
    int         height;         /**< 采集高度（像素） */
    int         fps;            /**< 目标帧率 */
    int         bitrate;        /**< H.264 编码目标码率（bps），例如 2000000 表示 2Mbps */
    uint32_t    v4l2_fourcc;    /**< V4L2 像素格式（FOURCC），0=自动选择（预留） */

    /* ============ 音频相关配置 ============ */
    
    const char *audio_device;   /**< ALSA 音频采集设备名，例如 "hw:0,0" */
    unsigned int sample_rate;   /**< 采样率（Hz），例如 48000 */
    unsigned int channels;      /**< 声道数，例如 2（立体声） */
    unsigned int audio_chunk_ms;/**< 音频块时长（毫秒），用于统计和调试 */

    /* ============ 输出相关配置 ============ */
    
    const char *sink_type;      /**< 输出类型："file" 或 "pipe"（预留） */
    const char *output_path_h264;/**< H.264 输出文件路径，例如 "out.h264" */
    const char *output_path_pcm; /**< PCM 音频输出文件路径，例如 "out.pcm" */
    unsigned int duration_sec;   /**< 录制时长（秒），0 表示无限制 */
} AppConfig;

/**
 * @brief 加载默认配置值
 * 
 * @param cfg 配置结构体指针
 * @return int 0 成功，-1 失败
 */
int  app_config_load_default(AppConfig *cfg);

/**
 * @brief 解析命令行参数
 * 
 * 使用 getopt_long 解析长短选项，覆盖 cfg 中的对应字段。
 * 调用前应先调用 app_config_load_default() 初始化默认值。
 * 
 * @param cfg  配置结构体指针（输入/输出）
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return int 0 成功，-1 失败
 */
int  app_config_parse_args(AppConfig *cfg, int argc, char **argv);

/**
 * @brief 打印当前配置摘要
 * 
 * 用于启动时确认最终生效的配置参数。
 * 
 * @param cfg 配置结构体指针（只读）
 */
void app_config_print_summary(const AppConfig *cfg);

/**
 * @brief 打印命令行帮助信息
 * 
 * @param prog 程序名（通常传 argv[0]）
 */
void app_config_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif
