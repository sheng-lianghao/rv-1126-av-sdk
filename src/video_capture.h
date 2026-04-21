#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define V4L2_BUF_COUNT 4

typedef struct {
    void  *start;
    size_t length;
} MmapBuffer;

typedef struct {
    int          fd;
    char         device[64];
    unsigned int width;
    unsigned int height;
    unsigned int fps_num;
    unsigned int fps_den;
    MmapBuffer   buffers[V4L2_BUF_COUNT];
    int          n_buffers;
    int          is_mplane;          /* 1 = MPLANE API（RV1126 ISP），0 = 单平面 */
    int64_t      start_time_ns;
} VideoCapture;

/* 打开设备、配置格式、申请并映射 mmap 缓冲区 */
int  video_capture_open(VideoCapture *vc, const char *device,
                        unsigned int w, unsigned int h,
                        unsigned int fps_num, unsigned int fps_den);

/* 启动视频流（VIDIOC_STREAMON） */
int  video_capture_start(VideoCapture *vc);

/* 阻塞等待一帧（select + DQBUF），将数据拷贝到堆上后立即 QBUF。
   调用者负责 free(*frame_data)。pts_ns 为相对于 start 的纳秒偏移。*/
int  video_capture_grab(VideoCapture *vc, void **frame_data,
                        size_t *frame_size, int64_t *pts_ns);

/* 停止视频流（VIDIOC_STREAMOFF） */
int  video_capture_stop(VideoCapture *vc);

/* munmap 所有缓冲区并关闭 fd */
void video_capture_close(VideoCapture *vc);

#endif /* VIDEO_CAPTURE_H */