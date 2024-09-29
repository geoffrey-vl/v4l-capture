/**
 * @file capture.c
 * @author Geoffrey Van Landeghem
 * @brief 
 * @version 0.1
 * @date 2024-05-09
 * 
 * @copyright Copyright (c) 2024
 * 
 * Simple application that captures a bunch of camera frames
 * using memory mapping, and saves the 5th frame to disc.
 * The output file is called frame.jpg.
 * The camera input device is /dev/video0.
 * 
 * Compile:
 * $ gcc -o capture capture.c
 * 
 * Run:
 * ./capture
 * 
 * Based upon https://www.kernel.org/doc/html/v4.14/media/uapi/v4l/capture.c.html
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <unistd.h>
#include <sys/mman.h>

#define VIDEO_DEV "/dev/video0"
#define OUTPUT_IMG "frame.jpg"
#define STORE_AFTER_X_FRAMES 5

static int _fd = 0;
static void* _buffer = NULL;
static unsigned int _len_buff = 0;
static int frames_received = 0;

static int open_device(void)
{
    fprintf(stdout, "Opening video device '" VIDEO_DEV "'\n");
    _fd = open(VIDEO_DEV, O_RDWR | O_NONBLOCK, 0);
    if (_fd < 0) {
        perror("Failed to open device");
        return errno;
    }
    return 0;
}

static int init_device(void)
{
    fprintf(stdout, "Querying capabilities device\n");
    struct v4l2_capability cap;
    if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("Failed to get device capabilities");
        return errno;
    }
    fprintf(stderr, "- DRIVER: %s\n", cap.driver);
    fprintf(stderr, "- BUS INFO: %s\n", cap.bus_info);
    fprintf(stderr, "- CARD: %s\n", cap.card);
    fprintf(stderr, "- VERSION: %d\n", cap.version);
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "The device does not support video capture.\n");
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "The device does not support video streaming.\n");
        return -1;
    }

    fprintf(stdout, "Setting image format\n");
    struct v4l2_format format;
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = 640;
    format.fmt.pix.height = 480;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_INTERLACED;
    if (ioctl(_fd, VIDIOC_S_FMT, &format) < 0) {
        perror("Failed to set format");
        return errno;
    }
    return 0;
}

static int init_mmap(void)
{
    fprintf(stdout, "Requesting buffers\n");
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("Failed to request buffers");
        return errno;
    }

    fprintf(stdout, "Memory mapping\n");
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        perror("Failed to query buffer");
        return errno;
    }
    fprintf(stdout, "Buffer length: %u\n", buf.length);
    _len_buff = buf.length;
    _buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
    if (_buffer == MAP_FAILED) {
        perror("Failed to mmap");
        return errno;
    }
    return 0;
}

static int start_capturing(void)
{
    fprintf(stdout, "Capturing frame (queue buffer)\n");
    struct v4l2_buffer buf;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
        perror("Failed to queue buffer");
        return errno;
    }
    fprintf(stdout, "Capturing frame (start stream)\n");
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("Failed to start capture");
        return errno;
    }
    return 0;
}

static int process_image(const void *data, int size)
{
    fprintf(stdout, "Saving frame to " OUTPUT_IMG "\n");
    FILE* file = fopen("frame.jpg", "wb");
    if (file == NULL) {
        perror("Failed to save frame");
        return -1;
    }
    size_t objects_written = fwrite(data, size, 1, file);
    fclose(file);
    fprintf(stdout, "Stored %lu object(s)\n", objects_written);
    return 0;
}

static int read_frame(void)
{
    struct v4l2_buffer buf;
    unsigned int i;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    fprintf(stdout, "Capturing frame (dequeue buffer)\n");
    if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return 0;
        perror("Failed to dequeue buffer");
        return errno;
    }

    frames_received++;
    fprintf(stdout, "Frame[%d] Buffer index: %d, bytes used: %d\n", frames_received, buf.index, buf.bytesused);

    if (frames_received == STORE_AFTER_X_FRAMES) {
        process_image(_buffer, buf.bytesused);
        return 1;
    }

    if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
        perror("Failed to queue buffer");
        return errno;
    }
    return 0;
}

static int main_loop(void)
{
    unsigned int count = 70;
    while (count-- > 0) {
        for (;;) {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(_fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(_fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;
                perror("Failed to select");
                return errno;
            }

            if (0 == r) {
                perror("Select timed out");
                return errno;
            }

            if (read_frame())
                break;
            /* EAGAIN - continue select loop. */
        }
        return 0;
    }
}

static int stop_capturing(void)
{
    fprintf(stdout, "Stop capturing\n");
    enum v4l2_buf_type type;
    if (ioctl(_fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("Failed to stop capture");
        return -1;
    }
    return 0;
}

static int uninit_mmap(void)
{
    fprintf(stdout, "Memory unmapping\n");
    if (-1 == munmap(_buffer, _len_buff)) {
        perror("Failed to unmap");
        return -1;
    }
    _buffer = NULL;
    return 0;
}

static int close_device(void)
{
    fprintf(stdout, "Closing video device\n");
    if (-1 == close(_fd)) {
        perror("Failed to close device");
        return -1;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (open_device() != 0) return -1;

    if (init_device() != 0) {
        close_device();
        return -1;
    }

    if (init_mmap() != 0) {
        close_device();
        return -1;
    }

    if (start_capturing()) {
        uninit_mmap();
        close_device();
        return -1;
    }

    if (main_loop()) {
        uninit_mmap();
        close_device();
        return -1;
    }

    if (stop_capturing()) {
        uninit_mmap();
        close_device();
        return -1;
    }

    if (uninit_mmap()) {
        close_device();
        return -1;
    }
    if (close_device()) return -1;

    return 0;
}