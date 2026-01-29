/**
 * @file main.c
 * @brief S1: 音视频采集、编码与队列管理主程序
 *
 * 本文件是整个应用的入口，实现了一个多线程的音视频采集系统：
 * 
 * 架构概览：
 * ┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌──────────────┐
 * │ V4L2 采集   │────>│ Raw视频队列  │────>│ MPP 编码    │────>│ H264输出队列 │────> 文件
 * │ (摄像头)    │     │ (g_raw_vq)   │     │ (H.264)     │     │ (g_h264_q)   │
 * └─────────────┘     └──────────────┘     └─────────────┘     └──────────────┘
 *
 * ┌─────────────┐     ┌──────────────┐
 * │ ALSA 采集   │────>│ 音频队列     │────> 文件
 * │ (麦克风)    │     │ (g_aud_q)    │
 * └─────────────┘     └──────────────┘
 *
 * 线程模型：
 * - signal_thread:        捕获 SIGINT/SIGTERM 实现优雅退出
 * - timer_thread:         定时器线程，到达指定时长后触发停止
 * - stats_thread:         每秒打印统计信息（帧率、码率、队列深度等）
 * - video_capture_thread: V4L2 视频采集，打时间戳后推入 raw 队列
 * - video_encode_thread:  从 raw 队列取帧，MPP 硬编码后推入 H264 队列
 * - audio_capture_thread: ALSA 音频采集，打时间戳后推入音频队列
 * - h264_sink_thread:     从 H264 队列取数据，写入文件
 * - pcm_sink_thread:      从音频队列取数据，写入 PCM 文件
 *
 * PTS（Presentation Time Stamp）策略：
 * - 视频：每帧在采集点使用 CLOCK_MONOTONIC 打时间戳
 * - 音频：起始时刻用 CLOCK_MONOTONIC，后续按采样帧数累加推算
 */

#include "app_config.h"
#include "log.h"
#include "v4l2_capture.h"
#include "audio_capture.h"
#include "encoder_mpp.h"
#include "av_stats.h"

#include "rkav/bqueue.h"
#include "rkav/types.h"
#include "rkav/time.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ============================================================================
 * 全局变量定义
 * ============================================================================ */

/**
 * @brief 全局停止标志（原子变量）
 * 
 * 使用原子操作保证多线程安全：
 * - 0: 正常运行
 * - 1: 请求停止
 */
static atomic_int g_stop = 0;

/**
 * @brief 音视频统计信息结构体
 * 
 * 用于记录每秒的帧数、字节数、丢帧数等指标
 */
static AvStats g_stats;

/**
 * @brief 原始视频帧队列
 * 
 * 存储 VideoFrame* 指针，连接采集线程与编码线程
 * 容量较小（8），满时采集端会丢帧以保证实时性
 */
static BQueue g_raw_vq;

/**
 * @brief H.264 编码后数据队列
 * 
 * 存储 EncodedPacket* 指针，连接编码线程与写入线程
 * 容量较大（64），给写入端更多缓冲空间
 */
static BQueue g_h264_q;

/**
 * @brief 音频数据队列
 * 
 * 存储 AudioChunk* 指针，连接音频采集线程与写入线程
 * 容量最大（256），音频数据量相对较小但频繁
 */
static BQueue g_aud_q;

/**
 * @brief 视频帧间 PTS 差值（微秒）
 * 
 * 用于监控视频帧间隔是否稳定
 */
static atomic_uint_fast64_t g_video_pts_delta_us;

/**
 * @brief 音频块间 PTS 差值（微秒）
 * 
 * 用于监控音频块间隔是否稳定
 */
static atomic_uint_fast64_t g_audio_pts_delta_us;

/**
 * @brief 请求停止所有线程
 * 
 * 该函数执行以下操作：
 * 1. 原子地将 g_stop 设为 1
 * 2. 关闭所有阻塞队列，唤醒等待中的线程
 * 
 * 只有第一次调用会真正执行关闭队列操作，后续调用无效（幂等）
 */
static void request_stop(void)
{
    /* 原子交换：返回旧值，同时设置新值为 1 */
    int prev = atomic_exchange(&g_stop, 1);
    if (prev == 0) {
        /* 首次调用：关闭所有队列，让阻塞的 pop/push 立即返回 */
        bq_close(&g_raw_vq);
        bq_close(&g_h264_q);
        bq_close(&g_aud_q);
    }
}

/**
 * @brief 检查是否应该停止运行
 * 
 * @return int 非零表示应该停止，0 表示继续运行
 */
static int should_stop(void)
{
    return atomic_load(&g_stop) != 0;
}

/* ============================================================================
 * 内存释放辅助函数
 * ============================================================================ */

/**
 * @brief 释放视频帧结构体及其数据
 * 
 * @param vf 视频帧指针，可为 NULL（安全）
 */
static void free_video_frame(VideoFrame *vf)
{
    if (!vf) return;
    if (vf->data) free(vf->data);
    free(vf);
}

/**
 * @brief 释放音频块结构体及其数据
 * 
 * @param ac 音频块指针，可为 NULL（安全）
 */
static void free_audio_chunk(AudioChunk *ac)
{
    if (!ac) return;
    if (ac->data) free(ac->data);
    free(ac);
}

/**
 * @brief 释放编码后数据包结构体及其数据
 * 
 * @param p 数据包指针，可为 NULL（安全）
 */
static void free_encoded_packet(EncodedPacket *p)
{
    if (!p) return;
    if (p->data) free(p->data);
    free(p);
}

/* ============================================================================
 * 线程相关类型定义
 * ============================================================================ */

/**
 * @brief 线程参数结构体
 * 
 * 用于向工作线程传递配置信息
 */
typedef struct {
    const AppConfig *cfg;  /**< 应用配置指针（只读） */
} ThreadArgs;

/**
 * @brief 信号处理线程函数
 * 
 * 使用 sigwait 阻塞等待 SIGINT（Ctrl+C）或 SIGTERM（kill）信号。
 * 收到信号后调用 request_stop() 触发优雅退出。
 * 
 * 为什么用 sigwait 而不是 signal handler：
 * - signal handler 是异步调用的，在 handler 中做复杂操作不安全
 * - sigwait 可以在专门的线程中同步处理信号，更加可控
 * 
 * @param arg 未使用
 * @return void* 始终返回 NULL
 */
static void *signal_thread(void *arg)
{
    (void)arg;  /* 抑制未使用参数警告 */
    
    /* 构建要等待的信号集 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);   /* Ctrl+C */
    sigaddset(&set, SIGTERM);  /* kill 命令 */

    int sig = 0;
    /* 阻塞等待信号，成功时 sig 被设为收到的信号编号 */
    if (sigwait(&set, &sig) == 0) {
        LOGW("[signal] caught signal=%d, stopping...", sig);
        request_stop();
    }
    return NULL;
}

/**
 * @brief 定时器线程参数
 */
typedef struct {
    unsigned int sec;  /**< 定时时长（秒），0 表示不启用定时器 */
} TimerArgs;

/**
 * @brief 定时器线程函数
 * 
 * 倒计时指定秒数后自动触发停止。
 * 每秒检查一次 should_stop()，以便在外部停止时及时退出。
 * 
 * @param arg 指向 TimerArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *timer_thread(void *arg)
{
    TimerArgs *t = (TimerArgs *)arg;
    if (!t || t->sec == 0) return NULL;

    unsigned int left = t->sec;
    
    /* 每秒递减，同时检查是否被外部停止 */
    while (!should_stop() && left > 0) {
        sleep(1);
        left--;
    }

    /* 如果是自然倒计时结束（非外部停止），则触发停止 */
    if (!should_stop()) {
        LOGI("[timer] reached %u sec, stopping...", t->sec);
        request_stop();
    }
    return NULL;
}

/**
 * @brief 统计信息线程函数
 * 
 * 每秒执行一次：
 * 1. 调用 av_stats_tick_print() 打印帧率、码率、丢帧等指标
 * 2. 打印三个队列的当前深度/容量
 * 3. 打印视频/音频的 PTS 间隔（用于监控稳定性）
 * 
 * @param arg 未使用
 * @return void* 始终返回 NULL
 */
static void *stats_thread(void *arg)
{
    (void)arg;
    
    while (!should_stop()) {
        sleep(1);
        
        /* 打印帧率、码率等核心指标 */
        av_stats_tick_print(&g_stats);

        /* 获取各队列当前深度 */
        size_t vq = bq_size(&g_raw_vq);
        size_t hq = bq_size(&g_h264_q);
        size_t aq = bq_size(&g_aud_q);
        LOGI("[Q] raw=%zu/%zu h264=%zu/%zu audio=%zu/%zu",
             vq, bq_capacity(&g_raw_vq),
             hq, bq_capacity(&g_h264_q),
             aq, bq_capacity(&g_aud_q));

        /* 打印 PTS 间隔，用于检测帧间隔是否稳定 */
        uint64_t vdu = atomic_load(&g_video_pts_delta_us);
        uint64_t adu = atomic_load(&g_audio_pts_delta_us);
        if (vdu) {
            LOGI("[PTS] video_delta=%.3fms", (double)vdu / 1000.0);
        } else {
            LOGI("[PTS] video_delta=n/a");
        }
        if (adu) {
            LOGI("[PTS] audio_delta=%.3fms", (double)adu / 1000.0);
        } else {
            LOGI("[PTS] audio_delta=n/a");
        }
    }
    return NULL;
}

/**
 * @brief 视频采集线程函数
 * 
 * 工作流程：
 * 1. 打开 V4L2 设备并启动采集
 * 2. 循环：出队帧 -> 打时间戳 -> 检测丢帧 -> 拷贝数据 -> 推入队列 -> 归还 buffer
 * 3. 退出时关闭设备
 * 
 * PTS 策略：
 * 每帧在 DQBUF 成功后立即调用 rkav_now_monotonic_us() 获取单调时钟时间戳。
 * 
 * 丢帧检测：
 * 通过 V4L2 buffer 的 sequence 字段检测驱动层丢帧（sequence 跳变）。
 * 
 * 队列满策略：
 * 使用 bq_try_push（非阻塞），队列满时直接丢帧，保证采集实时性。
 * 
 * @param arg 指向 ThreadArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *video_capture_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    /* 初始化 V4L2 采集 */
    V4L2Capture cap;
    if (v4l2_capture_open(&cap, cfg->video_device, cfg->width, cfg->height) != 0) {
        LOGE("[video_cap] open failed");
        request_stop();
        return NULL;
    }
    if (v4l2_capture_start(&cap) != 0) {
        LOGE("[video_cap] start failed");
        v4l2_capture_close(&cap);
        request_stop();
        return NULL;
    }

    uint64_t frame_id = 0;  /* 采集帧计数器 */

    /* 丢帧检测：通过 v4l2 sequence 检测丢帧（序号跳变） */
    int has_seq = 0;        /* 是否已记录过首帧 sequence */
    uint32_t last_seq = 0;  /* 上一帧的 sequence */

    while (!should_stop()) {
        int index = -1;
        void *data = NULL;
        size_t len = 0;

        /* 尝试出队一帧（非阻塞模式，返回 1 表示暂无数据） */
        int ret = v4l2_capture_dqbuf(&cap, &index, &data, &len);
        if (ret == 1) {
            /* EAGAIN：暂时没有帧可取，短暂休眠后重试 */
            usleep(1000);
            continue;
        }
        if (ret != 0) {
            /* 其他错误 */
            LOGE("[video_cap] dqbuf failed");
            av_stats_add_drop(&g_stats, 1);
            usleep(1000);
            continue;
        }

        /* 丢帧检测：sequence 应该连续递增 */
        if (!has_seq) {
            last_seq = cap.last_sequence;
            has_seq = 1;
        } else {
            uint32_t cur = cap.last_sequence;
            if (cur > last_seq + 1) {
                /* 检测到驱动层丢帧 */
                av_stats_add_drop(&g_stats, (uint64_t)(cur - last_seq - 1));
            }
            last_seq = cur;
        }

        /* 关键：在采集点打上 monotonic 时间戳 */
        uint64_t pts_us = rkav_now_monotonic_us();

        /* 分配视频帧结构体 */
        VideoFrame *vf = (VideoFrame *)calloc(1, sizeof(VideoFrame));
        if (!vf) {
            av_stats_add_drop(&g_stats, 1);
            v4l2_capture_qbuf(&cap, index);
            continue;
        }

        vf->data = (uint8_t *)malloc(len);
        if (!vf->data) {
            free(vf);
            av_stats_add_drop(&g_stats, 1);
            v4l2_capture_qbuf(&cap, index);
            continue;
        }
        memcpy(vf->data, data, len);
        
        /* 填充视频帧元数据 */
        vf->size = len;
        vf->w = cfg->width;
        vf->h = cfg->height;
        vf->stride = cfg->width;  /* 简化处理，实际可从 VIDIOC_G_FMT 获取准确 stride */
        vf->pts_us = pts_us;
        vf->frame_id = frame_id++;

        /* 非阻塞推入 raw 队列：满就丢帧，保证采集实时性 */
        int pr = bq_try_push(&g_raw_vq, vf);
        if (pr == 1) {
            /* 队列满：丢弃当前帧 */
            av_stats_add_drop(&g_stats, 1);
            free_video_frame(vf);
        } else if (pr < 0) {
            /* 队列已关闭：退出循环 */
            free_video_frame(vf);
            v4l2_capture_qbuf(&cap, index);
            break;
        }

        /* 将 buffer 归还给驱动，以便继续采集 */
        v4l2_capture_qbuf(&cap, index);
    }

    v4l2_capture_close(&cap);
    return NULL;
}

/**
 * @brief 视频编码线程函数
 * 
 * 工作流程：
 * 1. 初始化 MPP 硬编码器（H.264）
 * 2. 循环：从 raw 队列取帧 -> 编码 -> 封装成 EncodedPacket -> 推入 H264 队列
 * 3. 退出时释放编码器资源
 * 
 * 编码结果：
 * - out_data: 编码后的 H.264 NAL 数据（需要 free）
 * - out_keyframe: 是否为关键帧（I 帧）
 * 
 * @param arg 指向 ThreadArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *video_encode_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    /* 初始化 MPP 硬编码器 */
    EncoderMPP enc;
    if (encoder_mpp_init(&enc, cfg->width, cfg->height, cfg->fps,
                         cfg->bitrate, MPP_VIDEO_CodingAVC) != 0) {
        LOGE("[video_enc] encoder init failed");
        request_stop();
        return NULL;
    }

    while (!should_stop()) {
        void *item = NULL;
        
        /* 阻塞等待取帧 */
        int r = bq_pop(&g_raw_vq, &item);
        if (r == 0) break;  /* 队列关闭且为空 */
        if (r < 0) {
            av_stats_add_drop(&g_stats, 1);
            continue;
        }

        VideoFrame *vf = (VideoFrame *)item;

        /* 调用 MPP 编码，输出为独立的内存块 */
        uint8_t *pkt_data = NULL;
        size_t pkt_size = 0;
        bool key = false;

        int er = encoder_mpp_encode_packet(&enc, vf->data, vf->size,
                                           &pkt_data, &pkt_size, &key);
        if (er != 0) {
            av_stats_add_drop(&g_stats, 1);
            free_video_frame(vf);
            continue;
        }

        /* 编码成功且有输出数据 */
        if (pkt_data && pkt_size > 0) {
            /* 封装成 EncodedPacket */
            EncodedPacket *ep = (EncodedPacket *)calloc(1, sizeof(EncodedPacket));
            if (!ep) {
                free(pkt_data);
                av_stats_add_drop(&g_stats, 1);
            } else {
                ep->data = pkt_data;
                ep->size = pkt_size;
                ep->pts_us = vf->pts_us;      /* 继承原始帧的时间戳 */
                ep->is_keyframe = key;

                /* 阻塞推入 H264 队列 */
                int pr = bq_push(&g_h264_q, ep);
                if (pr != 0) {
                    free_encoded_packet(ep);
                    break;
                }

                /* 更新统计 */
                av_stats_inc_video_frame(&g_stats);
                av_stats_add_enc_bytes(&g_stats, (uint64_t)pkt_size);
            }
        }

        free_video_frame(vf);
    }

    encoder_mpp_deinit(&enc);
    return NULL;
}

/**
 * @brief 音频采集线程函数
 * 
 * 工作流程：
 * 1. 打开 ALSA 采集设备
 * 2. 循环：读取 PCM 数据 -> 打时间戳 -> 封装成 AudioChunk -> 推入队列
 * 3. 退出时关闭设备
 * 
 * PTS 策略：
 * - 起始时刻使用 CLOCK_MONOTONIC 获取
 * - 后续每块的 PTS 按采样帧数累加推算：pts += frames * 1000000 / sample_rate
 * - 这样保证 PTS 连续且与实际采样时长一致
 * 
 * @param arg 指向 ThreadArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *audio_capture_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    /* 初始化 ALSA 采集 */
    AudioCapture ac;
    if (audio_capture_open(&ac, cfg->audio_device, cfg->sample_rate, (int)cfg->channels) != 0) {
        LOGE("[audio_cap] open failed");
        request_stop();
        return NULL;
    }

    /* 起始 PTS 用 monotonic 时钟，后续靠采样计数推进 */
    uint64_t pts_us = rkav_now_monotonic_us();

    /* 每次读取的字节数 = period 帧数 × 每帧字节数 */
    size_t chunk_bytes = (size_t)ac.frames_per_period * ac.bytes_per_frame;

    while (!should_stop()) {
        /* 分配缓冲区 */
        uint8_t *buf = (uint8_t *)malloc(chunk_bytes);
        if (!buf) {
            av_stats_add_drop(&g_stats, 1);
            usleep(1000);
            continue;
        }

        /* 从 ALSA 读取 PCM 数据（阻塞） */
        ssize_t n = audio_capture_read(&ac, buf, chunk_bytes);
        if (n <= 0) {
            free(buf);
            if (!should_stop()) usleep(1000);
            continue;
        }

        /* 计算实际读取的采样帧数 */
        uint32_t frames = (uint32_t)(n / ac.bytes_per_frame);

        AudioChunk *chunk = (AudioChunk *)calloc(1, sizeof(AudioChunk));
        if (!chunk) {
            free(buf);
            av_stats_add_drop(&g_stats, 1);
            continue;
        }

        chunk->data = buf;
        chunk->bytes = (size_t)n;
        chunk->sample_rate = (int)ac.sample_rate;
        chunk->channels = ac.channels;
        chunk->bytes_per_sample = 2; // S16LE
        chunk->frames = frames;
        chunk->pts_us = pts_us;

        // 推进 pts：frames 是“每声道帧数”
        pts_us += (uint64_t)frames * 1000000ULL / (uint64_t)ac.sample_rate;

        int pr = bq_push(&g_aud_q, chunk);
        if (pr != 0) {
            free_audio_chunk(chunk);
            break;
        }
    }

    audio_capture_close(&ac);
    return NULL;
}

/**
 * @brief H.264 输出 Sink 线程函数
 * 
 * 工作流程：
 * 1. 打开输出文件（.h264 裸流）
 * 2. 循环：从 H264 队列取出编码包 -> 写入文件
 * 3. 退出时关闭文件
 * 
 * PTS Delta 计算：
 * 记录相邻两帧的 PTS 差值，用于统计线程输出帧间隔。
 * 
 * @param arg 指向 ThreadArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *h264_sink_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    /* 打开 H.264 输出文件 */
    FILE *fp = fopen(cfg->output_path_h264, "wb");
    if (!fp) {
        LOGE("[h264_sink] open file failed: %s", cfg->output_path_h264);
        request_stop();
        return NULL;
    }
    LOGI("[h264_sink] opened: %s", cfg->output_path_h264);

    uint64_t last_pts = 0;  /* 上一帧 PTS，用于计算帧间隔 */

    while (!should_stop()) {
        void *item = NULL;
        
        /* 阻塞等待取出编码包 */
        int r = bq_pop(&g_h264_q, &item);
        if (r == 0) break;  /* 队列关闭且为空 */
        if (r < 0) continue;

        EncodedPacket *ep = (EncodedPacket *)item;
        
        /* 计算并更新 PTS delta（供统计线程输出） */
        if (last_pts && ep->pts_us > last_pts) {
            atomic_store(&g_video_pts_delta_us, ep->pts_us - last_pts);
        }
        last_pts = ep->pts_us;

        /* 写入 H.264 数据 */
        if (ep->data && ep->size) {
            size_t w = fwrite(ep->data, 1, ep->size, fp);
            if (w != ep->size) {
                LOGW("[h264_sink] partial write: %zu/%zu", w, ep->size);
                request_stop();
            }
        }

        free_encoded_packet(ep);
    }

    fclose(fp);
    LOGI("[h264_sink] closed");
    return NULL;
}

/**
 * @brief PCM 输出 Sink 线程函数
 * 
 * 工作流程：
 * 1. 打开输出文件（.pcm 裸 PCM 数据）
 * 2. 循环：从音频队列取出 AudioChunk -> 写入文件
 * 3. 退出时关闭文件
 * 
 * @param arg 指向 ThreadArgs 的指针
 * @return void* 始终返回 NULL
 */
static void *pcm_sink_thread(void *arg)
{
    ThreadArgs *ta = (ThreadArgs *)arg;
    const AppConfig *cfg = ta->cfg;

    /* 打开 PCM 输出文件 */
    FILE *fp = fopen(cfg->output_path_pcm, "wb");
    if (!fp) {
        LOGE("[pcm_sink] open file failed: %s", cfg->output_path_pcm);
        request_stop();
        return NULL;
    }
    LOGI("[pcm_sink] opened: %s", cfg->output_path_pcm);

    uint64_t last_pts = 0;  /* 上一块 PTS，用于计算帧间隔 */

    while (!should_stop()) {
        void *item = NULL;
        
        /* 阻塞等待取出音频块 */
        int r = bq_pop(&g_aud_q, &item);
        if (r == 0) break;  /* 队列关闭且为空 */
        if (r < 0) continue;

        AudioChunk *ac = (AudioChunk *)item;
        
        /* 计算并更新 PTS delta */
        if (last_pts && ac->pts_us > last_pts) {
            atomic_store(&g_audio_pts_delta_us, ac->pts_us - last_pts);
        }
        last_pts = ac->pts_us;

        /* 写入 PCM 数据 */
        if (ac->data && ac->bytes) {
            size_t w = fwrite(ac->data, 1, ac->bytes, fp);
            if (w != ac->bytes) {
                LOGW("[pcm_sink] partial write: %zu/%zu", w, ac->bytes);
                request_stop();
            }
        }

        av_stats_inc_audio_chunk(&g_stats);
        free_audio_chunk(ac);
    }

    fclose(fp);
    LOGI("[pcm_sink] closed");
    return NULL;
}

/* ============================================================================
 *                              主函数
 * ============================================================================ */

/**
 * @brief 程序入口函数
 * 
 * 整体流程：
 * 1. 配置信号处理（阻塞 SIGINT/SIGTERM 以便用 sigwait 同步处理）
 * 2. 解析命令行参数
 * 3. 初始化统计和队列
 * 4. 创建 8 个工作线程
 * 5. 等待线程结束
 * 6. 清理资源
 * 
 * 线程列表：
 * - th_sig:       信号处理线程（等待 SIGINT/SIGTERM）
 * - th_timer:     定时器线程（可选，按时长自动停止）
 * - th_stat:      统计输出线程（每秒打印一次）
 * - th_vcap:      视频采集线程（V4L2）
 * - th_venc:      视频编码线程（MPP H.264）
 * - th_acap:      音频采集线程（ALSA）
 * - th_h264sink:  H.264 输出线程
 * - th_pcmsink:   PCM 输出线程
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return int 0 表示成功，非 0 表示失败
 */
int main(int argc, char **argv)
{
    /* 
     * 信号处理策略：
     * 使用 sigwait() 同步等待信号，而非异步 signal handler。
     * 先将 SIGINT/SIGTERM 加入阻塞集合，由专门线程 sigwait。
     */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* 加载默认配置并解析命令行参数 */
    AppConfig cfg;
    app_config_load_default(&cfg);
    if (app_config_parse_args(&cfg, argc, argv) != 0) {
        app_config_print_usage(argv[0]);
        return -1;
    }

    app_config_print_summary(&cfg);

    /* 初始化全局统计计数器和 PTS delta 变量 */
    av_stats_init(&g_stats);
    atomic_store(&g_video_pts_delta_us, 0);
    atomic_store(&g_audio_pts_delta_us, 0);

    /*
     * 初始化三个阻塞队列：
     * - g_raw_vq:  原始视频帧队列（容量小，保持采集实时性）
     * - g_h264_q:  编码后 H.264 包队列
     * - g_aud_q:   音频块队列（容量大，容纳更多音频数据）
     */
    if (bq_init(&g_raw_vq, 8) != 0 ||
        bq_init(&g_h264_q, 64) != 0 ||
        bq_init(&g_aud_q, 256) != 0) {
        LOGE("[main] queue init failed");
        return -1;
    }

    /* 准备线程参数 */
    ThreadArgs ta = { .cfg = &cfg };
    TimerArgs  targs = { .sec = cfg.duration_sec };

    /* 8 个工作线程句柄 */
    pthread_t th_sig, th_timer, th_stat;
    pthread_t th_vcap, th_venc;
    pthread_t th_acap, th_h264sink, th_pcmsink;

    /* 创建信号处理线程 */
    if (pthread_create(&th_sig, NULL, signal_thread, NULL) != 0) {
        LOGE("[main] pthread_create signal failed");
        return -1;
    }

    /* 如果指定了运行时长，创建定时器线程 */
    if (cfg.duration_sec > 0) {
        if (pthread_create(&th_timer, NULL, timer_thread, &targs) != 0) {
            LOGE("[main] pthread_create timer failed");
            request_stop();
        }
    }

    /* 创建统计输出线程 */
    if (pthread_create(&th_stat, NULL, stats_thread, NULL) != 0) {
        LOGE("[main] pthread_create stats failed");
        request_stop();
    }

    /* 创建视频采集和编码线程 */
    if (pthread_create(&th_vcap, NULL, video_capture_thread, &ta) != 0) {
        LOGE("[main] pthread_create video_cap failed");
        request_stop();
    }
    if (pthread_create(&th_venc, NULL, video_encode_thread, &ta) != 0) {
        LOGE("[main] pthread_create video_enc failed");
        request_stop();
    }

    /* 创建音频采集线程 */
    if (pthread_create(&th_acap, NULL, audio_capture_thread, &ta) != 0) {
        LOGE("[main] pthread_create audio_cap failed");
        request_stop();
    }

    /* 创建输出 Sink 线程 */
    if (pthread_create(&th_h264sink, NULL, h264_sink_thread, &ta) != 0) {
        LOGE("[main] pthread_create h264_sink failed");
        request_stop();
    }
    if (pthread_create(&th_pcmsink, NULL, pcm_sink_thread, &ta) != 0) {
        LOGE("[main] pthread_create pcm_sink failed");
        request_stop();
    }

    /* 
     * 等待采集和处理线程结束
     * 顺序：先等采集线程，再等编码/输出线程
     */
    pthread_join(th_vcap, NULL);
    pthread_join(th_acap, NULL);
    pthread_join(th_venc, NULL);
    pthread_join(th_h264sink, NULL);
    pthread_join(th_pcmsink, NULL);

    /* 通知其他线程停止 */
    request_stop();
    pthread_join(th_stat, NULL);

    /*
     * 信号线程默认阻塞在 sigwait()，这里发送 SIGTERM 唤醒它。
     * 也可以通过 Ctrl+C 或 kill 命令唤醒。
     */
    pthread_kill(th_sig, SIGTERM);
    pthread_join(th_sig, NULL);

    if (cfg.duration_sec > 0) {
        pthread_join(th_timer, NULL);
    }

    /* 销毁队列 */
    bq_destroy(&g_raw_vq);
    bq_destroy(&g_h264_q);
    bq_destroy(&g_aud_q);

    LOGI("[main] done. video=%s audio=%s", cfg.output_path_h264, cfg.output_path_pcm);
    return 0;
}
