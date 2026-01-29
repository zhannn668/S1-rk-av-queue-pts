/**
 * @file audio_capture.h
 * @brief ALSA 音频采集模块头文件
 * 
 * 封装 ALSA（Advanced Linux Sound Architecture）PCM 采集接口，
 * 提供简单的打开、读取、关闭操作。
 * 
 * 特性：
 * - 支持编译时检测 ALSA 可用性（使用 __has_include）
 * - 当 ALSA 不可用时，提供占位实现并输出错误信息
 * - 固定使用 S16_LE（16位小端）采样格式
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * ALSA 可用性检测
 * 
 * 使用 C 标准的 __has_include 特性检测 ALSA 头文件是否存在。
 * 如果不存在，定义占位类型并设置 RK_ALSA_AVAILABLE = 0。
 * ============================================================================ */
#if defined(__has_include)
#  if __has_include(<alsa/asoundlib.h>)
#    include <alsa/asoundlib.h>
#    define RK_ALSA_AVAILABLE 1
#  else
#    define RK_ALSA_AVAILABLE 0
#  endif
#else
#  include <alsa/asoundlib.h>
#  define RK_ALSA_AVAILABLE 1
#endif

/* 当 ALSA 不可用时的占位类型定义 */
#if !RK_ALSA_AVAILABLE
typedef struct _snd_pcm snd_pcm_t;        /**< PCM 句柄占位类型 */
typedef int snd_pcm_format_t;             /**< PCM 格式占位类型 */
typedef unsigned long snd_pcm_uframes_t;  /**< 帧数占位类型 */
typedef long snd_pcm_sframes_t;           /**< 有符号帧数占位类型 */
#endif

/**
 * @brief 音频采集上下文结构体
 * 
 * 保存 ALSA 采集的状态信息，包括设备句柄、采样参数等。
 */
typedef struct {
    snd_pcm_t          *handle;           /**< ALSA PCM 设备句柄 */
    unsigned int        sample_rate;      /**< 实际采样率（驱动可能会调整） */
    int                 channels;         /**< 声道数 */
    snd_pcm_format_t    format;           /**< 采样格式（当前固定 S16_LE） */
    snd_pcm_uframes_t   frames_per_period;/**< 每个 period 的帧数 */
    size_t              bytes_per_frame;  /**< 每帧字节数 = (位宽/8) × 声道数 */
} AudioCapture;

/**
 * @brief 打开 ALSA 采集设备
 * 
 * 初始化 PCM 采集，配置硬件参数（采样率、声道、格式等）。
 * 
 * @param ac          输出：采集上下文
 * @param device      ALSA 设备名，例如 "hw:0,0"、"default"
 * @param sample_rate 期望采样率（驱动可能会近似调整）
 * @param channels    声道数
 * @return int        0 成功，-1 失败
 */
int audio_capture_open(AudioCapture *ac,
                       const char *device,
                       unsigned int sample_rate,
                       int channels);

/**
 * @brief 从设备读取 PCM 数据（阻塞）
 * 
 * @param ac    采集上下文
 * @param buf   输出缓冲区
 * @param bytes 期望读取的字节数（建议是 frames_per_period × bytes_per_frame 的整数倍）
 * @return ssize_t 实际读取的字节数，<0 表示出错
 */
ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes);

/**
 * @brief 关闭设备并释放资源
 * 
 * @param ac 采集上下文
 */
void audio_capture_close(AudioCapture *ac);
