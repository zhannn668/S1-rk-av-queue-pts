/**
 * @file encoder_mpp.h
 * @brief Rockchip MPP 硬编码模块头文件
 * 
 * 封装 Rockchip MPP（Media Process Platform）硬件编码器接口。
 * 支持 H.264/AVC 编码，利用 RK 芯片的 VPU 进行硬件加速。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MPP 可用性检测 */
#if defined(__has_include)
#  if __has_include("rk_mpi.h")
#    include "rk_mpi.h"
#    define RK_MPP_AVAILABLE 1
#  elif __has_include(<rk_mpi.h>)
#    include <rk_mpi.h>
#    define RK_MPP_AVAILABLE 1
#  else
#    define RK_MPP_AVAILABLE 0
#  endif
#else
#  include "rk_mpi.h"
#  define RK_MPP_AVAILABLE 1
#endif

/* 当 MPP 不可用时的占位类型定义 */
#if !RK_MPP_AVAILABLE
typedef void *MppCtx;            /**< MPP 上下文占位类型 */
typedef struct MppApi MppApi;    /**< MPP API 占位类型 */
typedef void *MppBufferGroup;    /**< 缓冲组占位类型 */
typedef void *MppBuffer;         /**< 缓冲区占位类型 */
typedef int MppCodingType;       /**< 编码类型占位类型 */
enum { MPP_VIDEO_CodingAVC = 7 };/**< H.264/AVC 编码类型常量 */
#endif

#include "sink.h"

/**
 * @brief MPP 编码器上下文结构体
 */
typedef struct {
    MppCtx         ctx;           /**< MPP 上下文 */
    MppApi        *mpi;           /**< MPP API 接口指针 */
    MppBufferGroup buf_grp;       /**< ION 缓冲组 */
    MppBuffer      frm_buf;       /**< 输入帧缓冲 */
    int            width;         /**< 输入宽度 */
    int            height;        /**< 输入高度 */
    int            hor_stride;    /**< 水平步长（16 对齐后） */
    int            ver_stride;    /**< 垂直步长（16 对齐后） */
    size_t         frame_size;    /**< 帧大小 */
    MppCodingType  type;          /**< 编码类型 */
} EncoderMPP;

/** 初始化 MPP 编码器 */
int encoder_mpp_init(EncoderMPP *enc,
                     int width, int height,
                     int fps,
                     int bitrate_bps,
                     MppCodingType type);

/** 编码一帧并写入 Sink */
int encoder_mpp_encode(EncoderMPP *enc,
                       const uint8_t *frame_data,
                       size_t frame_size,
                       EncSink *sink,
                       size_t *out_bytes);

/** 编码一帧并返回数据包（调用者负责 free） */
int encoder_mpp_encode_packet(EncoderMPP *enc,
                              const uint8_t *frame_data,
                              size_t frame_size,
                              uint8_t **out_data,
                              size_t *out_size,
                              bool *out_keyframe);

/** 释放编码器资源 */
void encoder_mpp_deinit(EncoderMPP *enc);
