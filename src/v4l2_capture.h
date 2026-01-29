/**
 * @file v4l2_capture.h
 * @brief V4L2 视频采集模块头文件
 * 
 * 封装 Linux V4L2（Video for Linux 2）API，提供视频采集功能。
 * 
 * 特性：
 * - 使用 MMAP 方式映射内核缓冲区，减少内存拷贝
 * - 支持多平面格式（NV12M），自动合成为连续 NV12
 * - 非阻塞模式采集，适合实时处理场景
 * 
 * 典型使用流程：
 * 1. v4l2_capture_open()   - 打开设备并分配缓冲区
 * 2. v4l2_capture_start()  - 启动视频流
 * 3. 循环: v4l2_capture_dqbuf() -> 处理帧 -> v4l2_capture_qbuf()
 * 4. v4l2_capture_close()  - 关闭设备并释放资源
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** 最大缓冲区数量 */
#define V4L2_MAX_BUFS    8

/** 最大平面数（NV12M 使用 2 个平面：Y 和 UV） */
#define V4L2_MAX_PLANES  2

/**
 * @brief 单个 V4L2 缓冲区结构
 * 
 * 存储一个采集缓冲区各个平面的映射信息。
 */
typedef struct {
    void  *planes[V4L2_MAX_PLANES];   /**< 各平面的 mmap 起始地址 */
    size_t lengths[V4L2_MAX_PLANES];  /**< 各平面的 mmap 长度 */
} V4L2Buf;

/**
 * @brief V4L2 采集上下文结构
 * 
 * 保存设备状态、缓冲区信息和帧数据。
 */
typedef struct {
    int           fd;                  /**< 设备文件描述符 */

    unsigned int  width;               /**< 采集宽度（像素） */
    unsigned int  height;              /**< 采集高度（像素） */

    unsigned int  buf_count;           /**< 实际分配的缓冲区数量 */
    int           last_index;          /**< 最近一次 DQBUF 的缓冲区索引 */

    V4L2Buf       bufs[V4L2_MAX_BUFS]; /**< 缓冲区数组 */

    uint8_t      *nv12_frame;          /**< 合成后的连续 NV12 帧缓冲 */
    size_t        frame_size;          /**< 帧大小（字节） = width × height × 3 / 2 */

    uint32_t      last_sequence;       /**< 最近一次 DQBUF 的 sequence（用于丢帧检测） */
} V4L2Capture;

/**
 * @brief 打开 V4L2 采集设备
 * 
 * 打开设备节点、设置格式、分配缓冲区并入队。
 * 
 * @param cap    输出：采集上下文
 * @param dev    设备路径，例如 "/dev/video0"
 * @param width  期望宽度
 * @param height 期望高度
 * @return int   0 成功，-1 失败
 */
int  v4l2_capture_open (V4L2Capture *cap, const char *dev,
                        unsigned int width, unsigned int height);

/**
 * @brief 启动视频流
 * 
 * 调用 VIDIOC_STREAMON 开始采集。
 * 
 * @param cap 采集上下文
 * @return int 0 成功，-1 失败
 */
int  v4l2_capture_start(V4L2Capture *cap);

/**
 * @brief 出队一帧
 * 
 * 从驱动获取一帧已填充的数据，并合成为连续 NV12。
 * 使用后需调用 v4l2_capture_qbuf() 归还缓冲区。
 * 
 * @param cap    采集上下文
 * @param index  输出：缓冲区索引
 * @param data   输出：帧数据指针（指向内部缓冲，无需 free）
 * @param length 输出：帧数据长度
 * @return int   0 成功，1 暂无数据（EAGAIN），-1 失败
 */
int  v4l2_capture_dqbuf(V4L2Capture *cap, int *index,
                        void **data, size_t *length);

/**
 * @brief 将缓冲区归还给驱动
 * 
 * @param cap   采集上下文
 * @param index 缓冲区索引
 * @return int  0 成功，-1 失败
 */
int  v4l2_capture_qbuf (V4L2Capture *cap, int index);

/**
 * @brief 打印当前设备格式信息
 * 
 * 调用 VIDIOC_G_FMT 并打印实际生效的格式参数（便于调试）。
 * 
 * @param cap 采集上下文
 */
void v4l2_capture_dump_format(V4L2Capture *cap);

/**
 * @brief 关闭采集设备
 * 
 * 停止视频流、解除 mmap 映射、关闭文件描述符。
 * 
 * @param cap 采集上下文
 */
void v4l2_capture_close(V4L2Capture *cap);
