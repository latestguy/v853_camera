#ifndef V853_CAM_INTF_H
#define V853_CAM_INTF_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>
#include <linux/videodev2.h>

struct buffer {
    void *start[3];
    size_t length[3];
};

typedef struct camera_info {
    int cam_fd;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t buf_cnt;
    int nplanes;
    int driver_type;
    int pixel_fmt;
    struct buffer *buffers;
} camera_handle;

int camera_init(camera_handle *camera);
int camera_start(camera_handle *camera);
int camera_stop(camera_handle *camera);
int camera_uninit(camera_handle *camera);
int camera_cap_image(camera_handle *camera, uint8_t *img_buf, int *img_size, int timeout);
int loop_process(camera_handle *camera);

void show_capabilities(struct v4l2_capability *cap);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* V853_CAM_INTF_H */