#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

typedef struct {
    AVCodecContext    *codec_ctx;
    AVFrame           *frame;
    struct SwsContext *sws_ctx;   /* 仅当编码器不支持 NV12 时使用 */
    enum AVPixelFormat enc_fmt;   /* 编码器实际使用的像素格式 */
    int                width;
    int                height;
    int                fps_num;
    int                fps_den;
} VideoEncoder;

int  video_encoder_open(VideoEncoder *ve, int width, int height,
                        int fps_num, int fps_den, int bitrate,
                        int oformat_flags);

/* nv12_data：摄像头原始 NV12 帧数据 */
int  video_encoder_encode(VideoEncoder *ve, const void *nv12_data,
                          size_t data_size, int64_t pts_ns,
                          AVPacket **pkt);

int  video_encoder_flush(VideoEncoder *ve, AVPacket **pkt);

void video_encoder_close(VideoEncoder *ve);

#endif /* VIDEO_ENCODER_H */
