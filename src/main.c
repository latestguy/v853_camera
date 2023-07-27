#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <errno.h>
#include <asm-generic/errno-base.h>

#include <v853_cam_intf.h>

#define IOCTL_RETRY 4
#define V4L2_REQ_BUF_COUNT 3

#define PTR_CHECK(pa)                                \
    do                                               \
    {                                                \
        if (!pa)                                     \
        {                                            \
            printf("Invalid parameter is NULL!!\n"); \
            return -1;                               \
        }                                            \
    } while (0)

static int cam_fd;

/*
 * ioctl with a number of retries in the case of I/O failure
 * args:
 *   fd - device descriptor
 *   IOCTL_X - ioctl reference
 *   arg - pointer to ioctl data
 *
 * asserts:
 *   none
 *
 * returns - ioctl result
 */
#if 1
int xioctl(int fd, int IOCTL_X, void *arg)
{
    int ret   = 0;
    int tries = IOCTL_RETRY;
    do
    {
        ret = ioctl(fd, IOCTL_X, arg);
    } while (ret && tries-- && ((errno == EINTR) || (errno == EAGAIN) || (errno == ETIMEDOUT)));

    if (ret && (tries <= 0))
        fprintf(stderr, "V4L2_CORE: ioctl (%i) retried %i times - giving up: %s)\n", IOCTL_X, IOCTL_RETRY, strerror(errno));

    return (ret);
}
#else
static int
xioctl(int fd, int request, void *arg)
{
    int r;
    do
        r = ioctl(fd, request, arg);
    while (-1 == r && EINTR == errno);
    return r;
}
#endif

int main(int argc, char **argv)
{
    camera_handle camera;
    uint8_t img_buf[1920 * 1088 * 3 / 2];
    char file_name[32];
    int img_size;
    int ret;

    printf("hello world!\n");

    camera.pixel_fmt = V4L2_PIX_FMT_MJPEG;
    // camera.pixel_fmt = V4L2_PIX_FMT_YUV420;
    camera.buf_cnt   = V4L2_REQ_BUF_COUNT;

    ret = camera_init(&camera);
    if (ret < 0)
        return ret;

    ret = camera_start(&camera);
    if (ret < 0)
        return ret;

    // loop_process(&camera);
    for (int i = 0; i < 1000; i++)
    {
        FILE *fp;
        memset(file_name, 0, sizeof(file_name));
        sprintf(file_name, "img%02d.jpg", i);

        fp = fopen(file_name, "w");

        ret = camera_cap_image(&camera, img_buf, &img_size, 2);
        if (ret < 0)
        {
            printf("get image failed!\n");
            break;
        }
        printf("img[%02d] size: %d\n", i, img_size);

        fwrite(img_buf, img_size, 1, fp);

        fclose(fp);
    }

    camera_stop(&camera);

    camera_uninit(&camera);

    return 0;
}

int camera_init(camera_handle *camera)
{
    struct v4l2_capability cap;      /* Query device capabilities */
    struct v4l2_fmtdesc fmtdesc;     /* Enumerate image formats */
    struct v4l2_frmsizeenum frmsize; /* Enumerate frame sizes */
    struct v4l2_frmivalenum ival;    /* Enumerate fps */
    struct v4l2_format fmt;          /* try a format */
    struct v4l2_input inp;           /* select the current video input */
    struct v4l2_streamparm parms;    /* set streaming parameters */
    struct v4l2_requestbuffers req;  /* Initiate Memory Mapping or User Pointer I/O */
    struct v4l2_buffer buf;          /* Query the status of a buffer */
    int n_buffers = 0;
    int rc;
    int idx;

    PTR_CHECK(camera);

    /* open dev */
    camera->cam_fd = open("/dev/video0", O_RDWR);
    if (camera->cam_fd < 0)
    {
        perror("open ");
        return -1;
    }
    printf("camera init success!\n");

    /* query capability */
    rc = ioctl(camera->cam_fd, VIDIOC_QUERYCAP, &cap);
    if (rc < 0)
    {
        printf("query camera capabilities failed!\n");
        return -1;
    }
    show_capabilities(&cap);

    if (!(cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)))
    {
        printf("camera not support video capture!\n");
        return -1;
    }
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
        camera->driver_type = V4L2_CAP_VIDEO_CAPTURE;
    else if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        camera->driver_type = V4L2_CAP_VIDEO_CAPTURE_MPLANE;
    else
    {
        printf("This dev is not a capture device.!\n");
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("camera not support streaming!\n");
        return -1;
    }

    memset(&inp, 0, sizeof(inp));
    inp.index = 0;
    inp.type  = V4L2_INPUT_TYPE_CAMERA;
    rc        = ioctl(camera->cam_fd, VIDIOC_S_INPUT, &inp);
    if (rc < 0)
    {
        printf("camera input failed!\n");
        return -1;
    }

    /* enumerate camera support pixel format and resolution */
    printf("prepare query camera pixel fomat!\n");
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.index = 0;
    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (xioctl(camera->cam_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0)
    {
        printf("{ idx: %02d, pixelformat = '%c%c%c%c', description = '%s' }\n",
               fmtdesc.index, fmtdesc.pixelformat & 0xFF,
               (fmtdesc.pixelformat >> 8) & 0xFF, (fmtdesc.pixelformat >> 16) & 0xFF,
               (fmtdesc.pixelformat >> 24) & 0xFF, fmtdesc.description);
        fmtdesc.index++;

        /* camera resolution list*/
        frmsize.index        = 0;
        frmsize.pixel_format = fmtdesc.pixelformat;
        while (ioctl(camera->cam_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0)
        {
            frmsize.index++;
            if (frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
            {
                printf("{ discrete: width = %u, height = %u }\n",
                       frmsize.stepwise.max_width, frmsize.stepwise.max_height);
            }
            else
            {
                printf("{ discrete: width = %u, height = %u }\n",
                       frmsize.discrete.width, frmsize.discrete.height);
            }
        }
    }

    /* default select camera resolution from frmsize idx0 */
    frmsize.index        = 0;
    frmsize.pixel_format = camera->pixel_fmt;
    rc                   = xioctl(camera->cam_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize);
    if (rc < 0)
    {
        printf("camera enum resolution failed!\n");
        return -1;
    }
    printf("framesize type: %d\n", frmsize.type);
    if (frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
    {
        camera->width  = frmsize.stepwise.max_width;
        camera->height = frmsize.stepwise.max_height;
    }
    else
    {
        camera->width  = frmsize.discrete.width;
        camera->height = frmsize.discrete.height;
    }
    printf("width:  %d\n", camera->width);
    printf("height: %d\n", camera->height);

    /* set camera format and resolution */
    memset(&fmt, 0, sizeof(struct v4l2_format));
    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width       = camera->width;
        fmt.fmt.pix_mp.height      = camera->height;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.pixelformat = camera->pixel_fmt;
    }
    else
    {
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = camera->width;
        fmt.fmt.pix.height      = camera->height;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        fmt.fmt.pix.pixelformat = camera->pixel_fmt;
    }
    rc = ioctl(camera->cam_fd, VIDIOC_S_FMT, &fmt);
    if (rc < 0)
    {
        printf("camera set format failed!\n");
        return -1;
    }

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        if (camera->width != fmt.fmt.pix_mp.width || camera->height != fmt.fmt.pix_mp.height)
            printf(" does not support %u * %u\n", camera->width, camera->height);

        camera->width  = fmt.fmt.pix_mp.width;
        camera->height = fmt.fmt.pix_mp.height;
        printf(" VIDIOC_S_FMT succeed\n");
        printf(" fmt.type = %d\n", fmt.type);
        printf(" fmt.fmt.pix_mp.width = %d\n", fmt.fmt.pix_mp.width);
        printf(" fmt.fmt.pix_mp.height = %d\n", fmt.fmt.pix_mp.height);
        // printf(" fmt.fmt.pix_mp.pixelformat = %s\n", get_format_name(fmt.fmt.pix_mp.pixelformat));
        printf(" fmt.fmt.pix_mp.field = %d\n", fmt.fmt.pix_mp.field);

        if (ioctl(camera->cam_fd, VIDIOC_G_FMT, &fmt) < 0)
        {
            printf(" get the data format failed!\n");
            return -1;
        }

        camera->nplanes = fmt.fmt.pix_mp.num_planes;
        printf("camera.nplanes: %d\n", camera->nplanes);
    }
    else
    {
        if (camera->width != fmt.fmt.pix.width || camera->height != fmt.fmt.pix.height)
            printf(" does not support %u * %u\n", camera->width, camera->height);

        camera->width  = fmt.fmt.pix.width;
        camera->height = fmt.fmt.pix.height;
        printf(" VIDIOC_S_FMT succeed\n");
        printf(" fmt.type = %d\n", fmt.type);
        printf(" fmt.fmt.pix.width = %d\n", fmt.fmt.pix.width);
        printf(" fmt.fmt.pix.height = %d\n", fmt.fmt.pix.height);
        // printf(" fmt.fmt.pix.pixelformat = %s\n", get_format_name(fmt.fmt.pix.pixelformat));
        printf(" fmt.fmt.pix.field = %d\n", fmt.fmt.pix.field);
    }

    /* set camera buffer count and mem type */
    memset(&req, 0, sizeof(req));
    req.count = camera->buf_cnt;
    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    rc         = ioctl(camera->cam_fd, VIDIOC_REQBUFS, &req);
    if (rc < 0)
    {
        printf("camera dose not support mmap!\n");
        return -1;
    }

    camera->buffers = calloc(req.count, sizeof(*camera->buffers));
    if (camera->buffers == NULL)
    {
        printf("calloc for req buffers failed!\n");
        return -1;
    }

    for (n_buffers = 0; n_buffers < req.count; n_buffers++)
    {
        memset(&buf, 0, sizeof(buf));
        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        else
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = n_buffers;

        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
            buf.length   = camera->nplanes;
            buf.m.planes = (struct v4l2_plane *)calloc(buf.length, sizeof(struct v4l2_plane));
        }

        rc = ioctl(camera->cam_fd, VIDIOC_QUERYBUF, &buf);
        if (rc < 0)
        {
            printf("ioctl VIDIOC_QUERYBUF failed!\n");
            goto FREE_BUF;
        }

        /* memory map */
        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
            for (idx = 0; idx < camera->nplanes; idx++)
            {
                camera->buffers[n_buffers].length[idx] = buf.m.planes[idx].length;
                camera->buffers[n_buffers].start[idx]  = mmap(NULL, buf.m.planes[idx].length,
                                                              PROT_READ | PROT_WRITE,
                                                              MAP_SHARED, camera->cam_fd,
                                                              buf.m.planes[idx].m.mem_offset);

                if (camera->buffers[n_buffers].start[idx] == MAP_FAILED)
                {
                    printf("mmap failed!\n");
                    goto FREE_BUF;
                }

                printf(" map buffer index: %d, mem: %p, len: %d, offset: %x\n",
                       n_buffers, camera->buffers[n_buffers].start[idx], buf.m.planes[idx].length,
                       buf.m.planes[idx].m.mem_offset);
                free(buf.m.planes);
            }
        }
        else
        {
            printf("buf.length: %d\n", buf.length);
            camera->buffers[n_buffers].length[0] = buf.length;
            camera->buffers[n_buffers].start[0]  = mmap(NULL, buf.length,
                                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                                        camera->cam_fd, buf.m.offset);
            if (camera->buffers[n_buffers].start == MAP_FAILED)
            {
                printf("mmap failed!\n");
                goto FREE_BUF;
            }
            printf(" map buffer index: %d, mem: %p, len: %d, offset: %x\n",
                   n_buffers, camera->buffers[n_buffers].start[0],
                   buf.length, buf.m.offset);
        }
    }

    return 0;

FREE_BUF:
    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        free(buf.m.planes);
    free(camera->buffers);
CLOSE_CAM_FD:
    close(camera->cam_fd);

    return -1;
}

int camera_start(camera_handle *camera)
{
    PTR_CHECK(camera);

    enum v4l2_buf_type type;
    struct v4l2_buffer buf;
    int i;

    for (i = 0; i < camera->buf_cnt; ++i)
    {
        memset(&buf, 0, sizeof(buf));
        buf.index  = i;
        buf.memory = V4L2_MEMORY_MMAP;
        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
            buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.length   = camera->nplanes;
            buf.m.planes = (struct v4l2_plane *)calloc(buf.length, sizeof(struct v4l2_plane));
        }
        else
        {
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        }

        if (ioctl(camera->cam_fd, VIDIOC_QBUF, &buf) < 0)
        {
            printf("ioctl VIDIOC_QBUF failed!\n");
            goto FREE_BUF;
        }
        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            free(buf.m.planes);
    }

    type = buf.type;
    if (ioctl(camera->cam_fd, VIDIOC_STREAMON, &type) < 0)
    {
        printf("ioctl VIDIOC_STREAMON failed!\n");
        goto FREE_BUF;
    }

    return 0;

FREE_BUF:
    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        free(buf.m.planes);
    free(camera->buffers);
CLOSE_CAM_FD:
    close(camera->cam_fd);

    return -1;
}

int camera_stop(camera_handle *camera)
{
    PTR_CHECK(camera);

    int n_buffers;
    int i;
    enum v4l2_buf_type type;

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    else
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(camera->cam_fd, VIDIOC_STREAMOFF, &type) < 0)
    {
        printf("ioctl VIDIOC_STREAMON failed!\n");
    }

    return 0;
}

int camera_uninit(camera_handle *camera)
{
    PTR_CHECK(camera);

    int idx;
    int rc;

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        for (idx = 0; idx < camera->nplanes; idx++)
        {
            rc = munmap(camera->buffers[idx].start[idx], camera->buffers[idx].length[idx]);
            if (rc < 0)
                printf("munmap failed!\n");
        }
    }
    else
    {
        for (idx = 0; idx < camera->buf_cnt; idx++)
        {
            rc = munmap(camera->buffers[idx].start[0], camera->buffers[idx].length[0]);
            if (rc < 0)
                printf("munmap failed!\n");
        }
    }

    free(camera->buffers);
    close(camera->cam_fd);

    return 0;
}

/**
 * @brief get a camera image in stream.
 * 
 * @param camera camera handle point
 * @param img_buf image buffer addr
 * @param img_size current image size
 * @param timeout timeout
 * @return int 
 */
int camera_cap_image(camera_handle *camera, uint8_t *img_buf, int *img_size, int timeout)
{
    PTR_CHECK(camera);
    int rc;
    struct v4l2_buffer buf;

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.length   = camera->nplanes;
        buf.m.planes = (struct v4l2_plane *)calloc(camera->nplanes, sizeof(struct v4l2_plane));
    }
    else
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(camera->cam_fd, &fds);

    /* time out */
    tv.tv_sec  = timeout;
    tv.tv_usec = 0;

    rc = select(camera->cam_fd + 1, &fds, NULL, NULL, &tv);
    if (rc < 0)
    {
        printf("select error!\n");
        return -1;
    }
    else if (rc == 0)
    {
        printf("select time out..\n");
        return -1;
    }

    rc = ioctl(camera->cam_fd, VIDIOC_DQBUF, &buf);
    if (rc < 0)
    {
        printf("ioctl VIDIOC_DQBUF failed!\n");
        return -1;
    }

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        *img_size = camera->buffers[buf.index].length[0];
        memcpy(img_buf, camera->buffers[buf.index].start[0], camera->buffers[buf.index].length[0]);
    }
    else
    {
        *img_size = buf.bytesused;
        memcpy(img_buf, camera->buffers[buf.index].start[0], buf.bytesused);
    }

    rc = ioctl(camera->cam_fd, VIDIOC_QBUF, &buf);
    if (rc < 0)
    {
        printf("ioctl VIDIOC_QBUF failed!\n");
        return -1;
    }

    return 0;
}

int loop_process(camera_handle *camera)
{
    PTR_CHECK(camera);

    int rc;
    char file_name[32];
    FILE *fp = NULL;
    struct v4l2_buffer buf;
    static u_int32_t img_num;
    int last_tm = 0, cur_tm = 0;
    float fps;

    if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.length   = camera->nplanes;
        buf.m.planes = (struct v4l2_plane *)calloc(camera->nplanes, sizeof(struct v4l2_plane));
    }
    else
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    while (1)
    {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(camera->cam_fd, &fds);

        /* time out */
        tv.tv_sec  = 2;
        tv.tv_usec = 0;

        rc = select(camera->cam_fd + 1, &fds, NULL, NULL, &tv);
        if (rc < 0)
        {
            printf("select error!\n");
            return -1;
        }
        else if (rc == 0)
        {
            printf("select time out..\n");
            return -1;
        }

        rc = ioctl(camera->cam_fd, VIDIOC_DQBUF, &buf);
        if (rc < 0)
        {
            printf("ioctl VIDIOC_DQBUF failed!\n");
            return -1;
        }

        if (last_tm == 0)
        {
            last_tm = buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
        }
        else
        {
            cur_tm  = buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
            fps     = 2.0 / ((cur_tm - last_tm) / 1000000.0);
            last_tm = cur_tm;
            // printf("fps = %f\n", fps);
        }

        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
            printf("img_id: %08u, buf idx: %d, len: %zu, timestamp: %ld%ld, fps = %f\n",
                   img_num, buf.index,
                   camera->buffers[buf.index].length[0],
                   buf.timestamp.tv_sec, buf.timestamp.tv_usec, fps);
        }
        else
        {
            printf("img_id: %08u, buf idx: %d, len: %u, timestamp: %ld%ld, fps = %f\n",
                   img_num, buf.index, buf.bytesused,
                   buf.timestamp.tv_sec, buf.timestamp.tv_usec, fps);
        }
        img_num++;

        sprintf(file_name, "img%d_%ld%ld.yuv", img_num, buf.timestamp.tv_sec, buf.timestamp.tv_usec);
        fp = fopen(file_name, "w");
        if (fp == NULL)
        {
            printf("can't open %s\n", file_name);
            return -1;
        }

        if (camera->driver_type == V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            rc = fwrite(camera->buffers[buf.index].start[0], camera->buffers[buf.index].length[0], 1, fp);
        else
            rc = fwrite(camera->buffers[buf.index].start[0], buf.bytesused, 1, fp);
        if (rc != 1)
        {
            printf("fwrite for %s failed!\n", file_name);
            return -1;
        }
        fclose(fp);

        rc = ioctl(camera->cam_fd, VIDIOC_QBUF, &buf);
        if (rc < 0)
        {
            printf("ioctl VIDIOC_QBUF failed!\n");
            return -1;
        }
    }
}

void show_capabilities(struct v4l2_capability *cap)
{
    if (cap == NULL)
        return;
    printf("camera.bus_info:     %s\n", cap->bus_info);
    printf("camera.card:         %s\n", cap->card);
    printf("camera.capabilities: 0x%X\n", cap->capabilities);
    printf("camera.device_caps:  0x%X\n", cap->device_caps);
    printf("camera.driver:       %s\n", cap->driver);
    printf("camera.version:      0x%X\n", cap->version);
}