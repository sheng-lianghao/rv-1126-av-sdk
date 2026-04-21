#include "video_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <time.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

int video_capture_open(VideoCapture *vc, const char *device,
                       unsigned int w, unsigned int h,
                       unsigned int fps_num, unsigned int fps_den)
{
    memset(vc, 0, sizeof(*vc));
    snprintf(vc->device, sizeof(vc->device), "%s", device);
    vc->width   = w;
    vc->height  = h;
    vc->fps_num = fps_num;
    vc->fps_den = fps_den;

    vc->fd = open(device, O_RDWR | O_NONBLOCK);
    if (vc->fd < 0) {
        perror("open video device");
        return -1;
    }

    /* 查询能力 */
    struct v4l2_capability cap;
    if (xioctl(vc->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto err_close;
    }
    /* 若驱动设置了 V4L2_CAP_DEVICE_CAPS，真正的能力在 device_caps 里 */
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? cap.device_caps : cap.capabilities;
    int is_mplane = !!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    enum v4l2_buf_type buf_type = is_mplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vc->is_mplane = is_mplane;

    if (!(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) ||
        !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming capture\n");
        goto err_close;
    }

    /* 设置格式：NV12（RV1126 ISP 输出）*/
    struct v4l2_format fmt = {0};
    fmt.type = buf_type;
    if (is_mplane) {
        fmt.fmt.pix_mp.width       = w;
        fmt.fmt.pix_mp.height      = h;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
        fmt.fmt.pix_mp.num_planes  = 1;
    } else {
        fmt.fmt.pix.width       = w;
        fmt.fmt.pix.height      = h;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    }
    if (xioctl(vc->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto err_close;
    }

    /* 设置帧率 */
    struct v4l2_streamparm parm = {0};
    parm.type = buf_type;
    parm.parm.capture.timeperframe.numerator   = fps_den;
    parm.parm.capture.timeperframe.denominator = fps_num;
    xioctl(vc->fd, VIDIOC_S_PARM, &parm);  /* 非致命错误，忽略返回值 */

    /* 申请 mmap 缓冲区 */
    struct v4l2_requestbuffers req = {0};
    req.count  = V4L2_BUF_COUNT;
    req.type   = buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vc->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto err_close;
    }
    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer count: %u\n", req.count);
        goto err_close;
    }
    vc->n_buffers = (int)req.count;

    /* 映射每个缓冲区 */
    for (int i = 0; i < vc->n_buffers; i++) {
        struct v4l2_plane planes[1] = {{0}};
        struct v4l2_buffer buf = {0};
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (is_mplane) {
            buf.m.planes = planes;
            buf.length   = 1;
        }
        if (xioctl(vc->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto err_unmap;
        }
        size_t len    = is_mplane ? planes[0].length   : buf.length;
        off_t  offset = is_mplane ? planes[0].m.mem_offset : buf.m.offset;
        vc->buffers[i].length = len;
        vc->buffers[i].start  = mmap(NULL, len,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, vc->fd, offset);
        if (vc->buffers[i].start == MAP_FAILED) {
            perror("mmap");
            vc->n_buffers = i;
            goto err_unmap;
        }
    }
    return 0;

err_unmap:
    for (int i = 0; i < vc->n_buffers; i++)
        if (vc->buffers[i].start && vc->buffers[i].start != MAP_FAILED)
            munmap(vc->buffers[i].start, vc->buffers[i].length);
err_close:
    close(vc->fd);
    vc->fd = -1;
    return -1;
}

int video_capture_start(VideoCapture *vc)
{
    enum v4l2_buf_type buf_type = vc->is_mplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* 将所有缓冲区入队 */
    for (int i = 0; i < vc->n_buffers; i++) {
        struct v4l2_plane planes[1] = {{0}};
        struct v4l2_buffer buf = {0};
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (vc->is_mplane) {
            buf.m.planes = planes;
            buf.length   = 1;
        }
        if (xioctl(vc->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF (start)");
            return -1;
        }
    }

    if (xioctl(vc->fd, VIDIOC_STREAMON, &buf_type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    /* 记录 CLOCK_MONOTONIC 基准时间 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    vc->start_time_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 0;
}

int video_capture_grab(VideoCapture *vc, void **frame_data,
                       size_t *frame_size, int64_t *pts_ns)
{
    enum v4l2_buf_type buf_type = vc->is_mplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* select 等待帧就绪，超时 2 秒 */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(vc->fd, &fds);
    struct timeval tv = {2, 0};
    int r = select(vc->fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0)
        return (r == 0) ? -EAGAIN : -errno;

    struct v4l2_plane planes[1] = {{0}};
    struct v4l2_buffer buf = {0};
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (vc->is_mplane) {
        buf.m.planes = planes;
        buf.length   = 1;
    }
    if (xioctl(vc->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return -EAGAIN;
        perror("VIDIOC_DQBUF");
        return -1;
    }

    /* 计算 PTS（纳秒） */
    int64_t ts_ns;
    if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        ts_ns = (int64_t)buf.timestamp.tv_sec * 1000000000LL
              + (int64_t)buf.timestamp.tv_usec * 1000LL;
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ts_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    }
    *pts_ns = ts_ns - vc->start_time_ns;
    if (*pts_ns < 0) *pts_ns = 0;

    size_t used = vc->is_mplane ? planes[0].bytesused : buf.bytesused;
    *frame_size = used;
    *frame_data = malloc(used);
    if (!*frame_data) {
        xioctl(vc->fd, VIDIOC_QBUF, &buf);
        return -1;
    }
    memcpy(*frame_data, vc->buffers[buf.index].start, used);

    if (xioctl(vc->fd, VIDIOC_QBUF, &buf) < 0)
        perror("VIDIOC_QBUF (re-queue)");

    return 0;
}

int video_capture_stop(VideoCapture *vc)
{
    enum v4l2_buf_type buf_type = vc->is_mplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vc->fd, VIDIOC_STREAMOFF, &buf_type) < 0) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}

void video_capture_close(VideoCapture *vc)
{
    for (int i = 0; i < vc->n_buffers; i++)
        munmap(vc->buffers[i].start, vc->buffers[i].length);
    if (vc->fd >= 0)
        close(vc->fd);
    vc->fd = -1;
}
