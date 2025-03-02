/*   This file is part of Motion.
 *
 *   Motion is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   Motion is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Motion.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 *    video_v4l2.c
 *
 *    V4L2 interface with basically JPEG decompression support and even more ...
 *    Copyright 2006 Krzysztof Blaszkowski (kb@sysmikro.com.pl)
 *              2007 Angel Carpintero (motiondevelop@gmail.com)
 *    Refactor/rewrite code:  2018 MrDave
 *
*/
#include "translate.h"
#include "motion.h"
#include "util.h"
#include "logger.h"
#include "rotate.h"
#include "video_common.h"
#include "video_v4l2.h"
#include <sys/mman.h>


#ifdef HAVE_V4L2

#if defined(HAVE_LINUX_VIDEODEV2_H)
    #include <linux/videodev2.h>
#else
    #include <sys/videoio.h>
#endif

#define u8 unsigned char
#define u16 unsigned short
#define u32 unsigned int
#define s32 signed int

#define MMAP_BUFFERS            4
#define MIN_MMAP_BUFFERS        2
#define V4L2_PALETTE_COUNT_MAX 21

#define MAX2(x, y) ((x) > (y) ? (x) : (y))
#define MIN2(x, y) ((x) < (y) ? (x) : (y))

static pthread_mutex_t   v4l2_mutex;
static struct video_dev *video_devices = NULL;

typedef struct video_image_buff {
    unsigned char *ptr;
    int content_length;
    size_t size;                    /* total allocated size */
    size_t used;                    /* bytes already used */
    struct timeval image_time;      /* time this image was received */
} video_buff;

typedef struct {
    int fd_device;
    u32 fps;

    struct v4l2_capability cap;
    struct v4l2_format src_fmt;
    struct v4l2_format dst_fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    video_buff *buffers;

    s32 pframe;

    u32 ctrl_flags;
    volatile unsigned int *finish;      /* End the thread */

} src_v4l2_t;

typedef struct palette_item_struct{
    u32      v4l2id;
    char     fourcc[5];
} palette_item;

static void v4l2_palette_init(palette_item *palette_array)
{

    int indx;

    /* When adding here, update the max defined as V4L2_PALETTE_COUNT_MAX above */
    palette_array[0].v4l2id = V4L2_PIX_FMT_SN9C10X;
    palette_array[1].v4l2id = V4L2_PIX_FMT_SBGGR16;
    palette_array[2].v4l2id = V4L2_PIX_FMT_SBGGR8;
    palette_array[3].v4l2id = V4L2_PIX_FMT_SPCA561;
    palette_array[4].v4l2id = V4L2_PIX_FMT_SGBRG8;
    palette_array[5].v4l2id = V4L2_PIX_FMT_SGRBG8;
    palette_array[6].v4l2id = V4L2_PIX_FMT_PAC207;
    palette_array[7].v4l2id = V4L2_PIX_FMT_PJPG;
    palette_array[8].v4l2id = V4L2_PIX_FMT_MJPEG;
    palette_array[9].v4l2id = V4L2_PIX_FMT_JPEG;
    palette_array[10].v4l2id = V4L2_PIX_FMT_RGB24;
    palette_array[11].v4l2id = V4L2_PIX_FMT_SPCA501;
    palette_array[12].v4l2id = V4L2_PIX_FMT_SPCA505;
    palette_array[13].v4l2id = V4L2_PIX_FMT_SPCA508;
    palette_array[14].v4l2id = V4L2_PIX_FMT_UYVY;
    palette_array[15].v4l2id = V4L2_PIX_FMT_YUYV;
    palette_array[16].v4l2id = V4L2_PIX_FMT_YUV422P;
    palette_array[17].v4l2id = V4L2_PIX_FMT_YUV420; /* most efficient for motion */
    palette_array[18].v4l2id = V4L2_PIX_FMT_Y10;
    palette_array[19].v4l2id = V4L2_PIX_FMT_Y12;
    palette_array[20].v4l2id = V4L2_PIX_FMT_GREY;
    palette_array[21].v4l2id = V4L2_PIX_FMT_H264;

    for (indx=0; indx <=V4L2_PALETTE_COUNT_MAX; indx++ ) {
        sprintf(palette_array[indx].fourcc ,"%c%c%c%c"
                ,palette_array[indx].v4l2id >> 0
                ,palette_array[indx].v4l2id >> 8
                ,palette_array[indx].v4l2id >> 16
                ,palette_array[indx].v4l2id >> 24);
    }

}

#if defined (BSD)
static int xioctl(src_v4l2_t *vid_source, unsigned long request, void *arg)
#else
static int xioctl(src_v4l2_t *vid_source, int request, void *arg)
#endif
{
    int ret;

    do
        ret = ioctl(vid_source->fd_device, request, arg);
    while (-1 == ret && EINTR == errno && !vid_source->finish);

    return ret;
}

static void v4l2_vdev_free(struct context *cnt)
{

    /* free the information we collected regarding the controls */
    if (cnt->vdev != NULL) {
        util_parms_free(cnt->vdev);

        free(cnt->vdev);
        cnt->vdev = NULL;
    }
}

static void v4l2_vdev_init(struct context *cnt)
{

    /* Create the v4l2 context within the main thread context  */
    cnt->vdev = mymalloc(sizeof(struct params_context));
    memset(cnt->vdev, 0, sizeof(struct params_context));
    cnt->vdev->params_array = NULL;
    cnt->vdev->params_count = 0;
    cnt->vdev->update_params = TRUE;     /*Set trigger that we have updated user parameters */

    return;

}

static int v4l2_ctrls_count(struct video_dev *curdev)
{

    /* Get the count of how many controls and menu items the device supports */
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_queryctrl       vid_ctrl;
    struct v4l2_querymenu       vid_menu;
    int indx;

    curdev->devctrl_count = 0;

    memset(&vid_ctrl, 0, sizeof(struct v4l2_queryctrl));
    vid_ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (xioctl (vid_source, VIDIOC_QUERYCTRL, &vid_ctrl) == 0) {
        if (vid_ctrl.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
            vid_ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
            continue;
        }
        curdev->devctrl_count++;
        if (vid_ctrl.type == V4L2_CTRL_TYPE_MENU) {
            for (indx = vid_ctrl.minimum; indx<=vid_ctrl.maximum; indx++) {
                memset(&vid_menu, 0, sizeof(struct v4l2_querymenu));
                vid_menu.id = vid_ctrl.id;
                vid_menu.index = indx;
                if (xioctl(vid_source, VIDIOC_QUERYMENU, &vid_menu) == 0) {
                    curdev->devctrl_count++;
                }
            }
        }
        vid_ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    return 0;

}

static int v4l2_ctrls_list(struct video_dev *curdev)
{

    /* Get the names of the controls and menu items the device supports */
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_queryctrl       vid_ctrl;
    struct v4l2_querymenu       vid_menu;
    struct v4l2_control         vid_curr;
    int indx, indx_ctrl;

    curdev->devctrl_array = NULL;
    if (curdev->devctrl_count == 0 ) {
        MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, _("No Controls found for device"));
        return 0;
    }

    curdev->devctrl_array = malloc(curdev->devctrl_count * sizeof(struct vid_devctrl_ctx));

    memset(&vid_ctrl, 0, sizeof(struct v4l2_queryctrl));
    vid_ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    indx_ctrl = 0;
    while (xioctl (vid_source, VIDIOC_QUERYCTRL, &vid_ctrl) == 0) {
        if (vid_ctrl.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
            vid_ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
            continue;
        }

        curdev->devctrl_array[indx_ctrl].ctrl_id = vid_ctrl.id;
        curdev->devctrl_array[indx_ctrl].ctrl_type = vid_ctrl.type;
        curdev->devctrl_array[indx_ctrl].ctrl_default = vid_ctrl.default_value;
        curdev->devctrl_array[indx_ctrl].ctrl_currval = vid_ctrl.default_value;
        curdev->devctrl_array[indx_ctrl].ctrl_newval = vid_ctrl.default_value;
        curdev->devctrl_array[indx_ctrl].ctrl_menuitem = FALSE;

        curdev->devctrl_array[indx_ctrl].ctrl_name = malloc(32);
        sprintf(curdev->devctrl_array[indx_ctrl].ctrl_name,"%s",vid_ctrl.name);

        curdev->devctrl_array[indx_ctrl].ctrl_iddesc = malloc(15);
        sprintf(curdev->devctrl_array[indx_ctrl].ctrl_iddesc,"ID%08d",vid_ctrl.id);

        curdev->devctrl_array[indx_ctrl].ctrl_minimum = vid_ctrl.minimum;
        curdev->devctrl_array[indx_ctrl].ctrl_maximum = vid_ctrl.maximum;

        memset(&vid_curr, 0, sizeof (struct v4l2_control));
        vid_curr.id = vid_ctrl.id;
        if (xioctl(vid_source, VIDIOC_G_CTRL, &vid_curr) == 0) {
            curdev->devctrl_array[indx_ctrl].ctrl_currval = vid_curr.value;
            curdev->devctrl_array[indx_ctrl].ctrl_newval = vid_curr.value;
        }

        if (vid_ctrl.type == V4L2_CTRL_TYPE_MENU) {
            for (indx = vid_ctrl.minimum; indx<=vid_ctrl.maximum; indx++) {
                memset(&vid_menu, 0, sizeof(struct v4l2_querymenu));
                vid_menu.id = vid_ctrl.id;
                vid_menu.index = indx;
                if (xioctl(vid_source, VIDIOC_QUERYMENU, &vid_menu) == 0) {

                    indx_ctrl++;
                    curdev->devctrl_array[indx_ctrl].ctrl_id = vid_ctrl.id;
                    curdev->devctrl_array[indx_ctrl].ctrl_type = 0;
                    curdev->devctrl_array[indx_ctrl].ctrl_menuitem = TRUE;

                    curdev->devctrl_array[indx_ctrl].ctrl_name = malloc(32);
                    sprintf(curdev->devctrl_array[indx_ctrl].ctrl_name,"%s",vid_menu.name);

                    curdev->devctrl_array[indx_ctrl].ctrl_iddesc = malloc(40);
                    sprintf(curdev->devctrl_array[indx_ctrl].ctrl_iddesc,"menu item: Value %d",indx);

                    curdev->devctrl_array[indx_ctrl].ctrl_minimum = 0;
                    curdev->devctrl_array[indx_ctrl].ctrl_maximum = 0;
                }
            }
        }
        indx_ctrl++;
        vid_ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    if (curdev->devctrl_count != 0 ) {
        MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, _("---------Controls---------"));
        MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, _("  V4L2 ID   Name and Range"));
        for (indx=0; indx < curdev->devctrl_count; indx++) {
            if (curdev->devctrl_array[indx].ctrl_menuitem) {
                MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, "  %s %s"
                           ,curdev->devctrl_array[indx].ctrl_iddesc
                           ,curdev->devctrl_array[indx].ctrl_name);
            } else {
                MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, "%s %s, %d to %d"
                           ,curdev->devctrl_array[indx].ctrl_iddesc
                           ,curdev->devctrl_array[indx].ctrl_name
                           ,curdev->devctrl_array[indx].ctrl_minimum
                           ,curdev->devctrl_array[indx].ctrl_maximum);
            }
        }
        MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, "--------------------------");
    }

    return 0;

}

static void v4l2_parm_reset(struct context *cnt
    , struct video_dev *curdev, int indx_dev)
{
    struct vid_devctrl_ctx *devitem;
    struct params_item_ctx *usritem;
    int indx_user, retcd;

    devitem=&curdev->devctrl_array[indx_dev];
    for (indx_user=0; indx_user<cnt->vdev->params_count; indx_user++) {
        usritem=&cnt->vdev->params_array[indx_user];
        if ((mystrceq(devitem->ctrl_iddesc,usritem->param_name)) ||
            (mystrceq(devitem->ctrl_name  ,usritem->param_name))) {
            free(usritem->param_value);
            /* 32 is a randomly selected long enough length*/
            usritem->param_value = mymalloc(32);
            retcd = snprintf(usritem->param_value, 32, "%d", devitem->ctrl_currval);
            if ((retcd < 0) || (retcd > 31)){
                MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                    ,_("Error resetting user value"));
                return;
            }
        }
    }
    devitem->ctrl_newval = devitem->ctrl_currval;

    MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
        ,_("Leaving control %s \"%s\" set to %d")
        ,devitem->ctrl_iddesc, devitem->ctrl_name
        ,devitem->ctrl_currval);
    return;
}

static int v4l2_ctrls_set(struct context *cnt, struct video_dev *curdev)
{
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct vid_devctrl_ctx *devitem;
    struct v4l2_control     vid_ctrl;
    int indx_dev, retcd, failed;

    if (vid_source == NULL) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO,_("Device not ready"));
        return -1;
    }

    failed = FALSE;
    for (indx_dev= 0;indx_dev<curdev->devctrl_count;indx_dev++) {
        devitem=&curdev->devctrl_array[indx_dev];
        if (devitem->ctrl_menuitem == FALSE) {
            if (devitem->ctrl_currval != devitem->ctrl_newval) {
                memset(&vid_ctrl, 0, sizeof (struct v4l2_control));
                vid_ctrl.id = devitem->ctrl_id;
                vid_ctrl.value = devitem->ctrl_newval;
                retcd = xioctl(vid_source, VIDIOC_S_CTRL, &vid_ctrl);
                if (retcd < 0) {
                    failed = TRUE;
                } else {
                    if (curdev->starting) {
                        MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO
                            ,_("Set control \"%s\" to value %d")
                            ,devitem->ctrl_name, devitem->ctrl_newval);
                    }
                   devitem->ctrl_currval = devitem->ctrl_newval;
                }
            }
        }
    }

    /* We loop again to retry to set the values.  It is possible that
     * something needed to be "enabled" before the device would accept
     * values for a specific control. So the first loop would in theory
     * enable the control to accept different values, then this loop would
     * be able to "set" the value.  (Users may specify them in random order)
     */
    if (failed == TRUE) {
        for (indx_dev= 0;indx_dev<curdev->devctrl_count;indx_dev++) {
            devitem=&curdev->devctrl_array[indx_dev];
            if (devitem->ctrl_menuitem == FALSE) {
                if (devitem->ctrl_currval != devitem->ctrl_newval) {
                    memset(&vid_ctrl, 0, sizeof (struct v4l2_control));
                    vid_ctrl.id = devitem->ctrl_id;
                    vid_ctrl.value = devitem->ctrl_newval;
                    retcd = xioctl(vid_source, VIDIOC_S_CTRL, &vid_ctrl);
                    if (retcd < 0) {
                        MOTION_LOG(WRN, TYPE_VIDEO, SHOW_ERRNO
                            ,_("setting control %s \"%s\" to %d failed. ")
                            ,devitem->ctrl_iddesc, devitem->ctrl_name
                            ,devitem->ctrl_newval);
                        v4l2_parm_reset(cnt, curdev, indx_dev);
                    } else {
                        if (curdev->starting) {
                            MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO
                                , _("Set control \"%s\" to value %d")
                                , devitem->ctrl_name, devitem->ctrl_newval);
                        }
                        devitem->ctrl_currval = devitem->ctrl_newval;
                    }
                }
            }
        }
    }

    return 0;
}

static int v4l2_parms_set(struct context *cnt, struct video_dev *curdev)
{

    struct vid_devctrl_ctx  *devitem;
    struct params_item_ctx *usritem;
    int indx_dev, indx_user;

    if (cnt->conf.roundrobin_skip < 0) {
        cnt->conf.roundrobin_skip = 1;
    }

    if (curdev->devctrl_count == 0) {
        cnt->vdev->update_params = FALSE;
        return 0;
    }

    for (indx_dev=0; indx_dev<curdev->devctrl_count; indx_dev++ ) {
        devitem=&curdev->devctrl_array[indx_dev];
        for (indx_user=0; indx_user<cnt->vdev->params_count; indx_user++) {
            usritem=&cnt->vdev->params_array[indx_user];
            if ((mystrceq(devitem->ctrl_iddesc,usritem->param_name)) ||
                (mystrceq(devitem->ctrl_name  ,usritem->param_name))) {
                switch (devitem->ctrl_type) {
                case V4L2_CTRL_TYPE_MENU:
                    /*FALLTHROUGH*/
                case V4L2_CTRL_TYPE_INTEGER:
                    if (atoi(usritem->param_value) < devitem->ctrl_minimum) {
                        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                            ,_("%s control option value %s is below minimum.  Using minimum %d")
                            ,devitem->ctrl_name, usritem->param_value, devitem->ctrl_minimum);
                        devitem->ctrl_newval = devitem->ctrl_minimum;
                    } else if (atoi(usritem->param_value) > devitem->ctrl_maximum) {
                        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                            ,_("%s control option value %s is above maximum.  Using maximum %d")
                            ,devitem->ctrl_name, usritem->param_value, devitem->ctrl_maximum);
                        devitem->ctrl_newval = devitem->ctrl_maximum;
                    } else {
                        devitem->ctrl_newval = atoi(usritem->param_value);
                    }
                    break;
                case V4L2_CTRL_TYPE_BOOLEAN:
                    devitem->ctrl_newval = atoi(usritem->param_value) ? 1 : 0;
                    break;
                default:
                    MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                        ,_("control type not supported yet"));
                }
            }
        }
    }

    return 0;

}

static int v4l2_autobright(struct context *cnt, struct video_dev *curdev, int method)
{

    struct vid_devctrl_ctx  *devitem;
    struct params_item_ctx  *usritem;
    unsigned char           *image;
    int                      window_high;
    int                      window_low;
    int                      target;
    int indx, device_value, make_change;
    int pixel_count, avg, step;
    int parm_hysteresis, parm_damper, parm_max, parm_min;
    char cid_exp[15],cid_expabs[15],cid_bright[15];


    if ((method == 0) || (method > 3)) {
        return 0;
    }

    /* Set the values for the control variables */
    parm_hysteresis = 20;
    parm_damper = 20;
    parm_max = 255;
    parm_min = 0;

    target = -1;

    sprintf(cid_bright,"ID%08d",V4L2_CID_BRIGHTNESS);
    sprintf(cid_exp,"ID%08d",V4L2_CID_EXPOSURE);
    sprintf(cid_expabs,"ID%08d",V4L2_CID_EXPOSURE_ABSOLUTE);

    for (indx = 0;indx < cnt->vdev->params_count; indx++) {
        usritem=&cnt->vdev->params_array[indx];
        if ((method == 1) &&
            ((mystrceq(usritem->param_name,"brightness")) ||
             (mystrceq(usritem->param_name,cid_bright)))) {
               target = atoi(usritem->param_value);
        } else if ((method == 2) &&
            ((mystrceq(usritem->param_name,"exposure")) ||
             (mystrceq(usritem->param_name,cid_exp)))) {
               target = atoi(usritem->param_value);
        } else if ((method == 3) &&
            ((mystrceq(usritem->param_name,"exposure (absolute)")) ||
             (mystrceq(usritem->param_name,cid_expabs)))) {
               target = atoi(usritem->param_value);
        }
    }

    device_value = -1;
    for (indx = 0;indx < curdev->devctrl_count; indx++) {
        devitem=&curdev->devctrl_array[indx];
        if ((method == 1) && (devitem->ctrl_id == V4L2_CID_BRIGHTNESS)) {
            device_value = devitem->ctrl_currval;
            parm_max = devitem->ctrl_maximum;
            parm_min = devitem->ctrl_minimum;
            if (target == -1) {
                target = (int) ((devitem->ctrl_maximum - devitem->ctrl_minimum)/2);
            }
        } else if ((method == 2) && (devitem->ctrl_id == V4L2_CID_EXPOSURE)) {
            device_value = devitem->ctrl_currval;
            parm_max = devitem->ctrl_maximum;
            parm_min = devitem->ctrl_minimum;
            if (target == -1) {
                target = (int) ((devitem->ctrl_maximum - devitem->ctrl_minimum)/2);
            }
        } else if ((method == 3) && (devitem->ctrl_id == V4L2_CID_EXPOSURE_ABSOLUTE)) {
            device_value = devitem->ctrl_currval;
            parm_max = devitem->ctrl_maximum;
            parm_min = devitem->ctrl_minimum;
            if (target == -1) {
                target = (int) ((devitem->ctrl_maximum - devitem->ctrl_minimum)/2);
            }
        }
    }
    /* If we can not find control just give up */
    if (device_value == -1) {
        return 0;
    }

    avg = 0;
    pixel_count = 0;
    image = cnt->imgs.image_vprvcy.image_norm;
    for (indx = 0; indx < cnt->imgs.motionsize; indx += 10) {
        avg += image[indx];
        pixel_count++;
    }
    /* The compiler seems to mandate this be done in separate steps */
    /* Must be an integer math thing..must read up on this...*/
    avg = (avg / pixel_count);
    avg = avg * (parm_max - parm_min);
    avg = avg / 255;

    make_change = FALSE;
    step = 0;
    window_high = MIN2(target + parm_hysteresis, parm_max);
    window_low  = MAX2(target - parm_hysteresis, parm_min);

    /* Average is above window - turn down exposure - go for the target. */
    if (avg > window_high) {
        step = MIN2((avg - target) / parm_damper + 1, device_value - parm_min);
        if (device_value > step + 1 - parm_min) {
            device_value -= step;
            make_change = TRUE;
        } else {
            device_value = parm_min;
            make_change = TRUE;
        }
        //MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, "Down Avg %d step: %d device:%d",avg,step,device_value);
    } else if (avg < window_low) {
        /* Average is below window - turn up exposure - go for the target. */
        step = MIN2((target - avg) / parm_damper + 1, parm_max - device_value);
        if (device_value < parm_max - step) {
            device_value += step;
            make_change = TRUE;
        } else {
            device_value = parm_max;
            make_change = TRUE;
        }
        //MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO, "Up Avg %d step: %d device:%d",avg,step,device_value);
    }

    if (make_change) {
        for (indx = 0;indx < curdev->devctrl_count; indx++) {
            devitem=&curdev->devctrl_array[indx];
            if ((method == 1) && (devitem->ctrl_id == V4L2_CID_BRIGHTNESS)) {
                devitem->ctrl_newval = device_value;
            } else if ((method == 2) && (devitem->ctrl_id == V4L2_CID_EXPOSURE)) {
                devitem->ctrl_newval = device_value;
            } else if ((method == 3) && (devitem->ctrl_id == V4L2_CID_EXPOSURE_ABSOLUTE)) {
                devitem->ctrl_newval = device_value;
            }
        }
    }

    return 0;
}

static int v4l2_input_select(struct context *cnt, struct video_dev *curdev)
{

    /* Set the input number for the device if applicable */
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_input    input;

    if ((cnt->param_input == curdev->input) && (!curdev->starting)) {
        return 0;
    }

    memset(&input, 0, sizeof (struct v4l2_input));
    if (cnt->param_input == -1 ) {
        input.index = 0;
    } else {
        input.index = cnt->param_input;
    }

    if (xioctl(vid_source, VIDIOC_ENUMINPUT, &input) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Unable to query input %d."
            " VIDIOC_ENUMINPUT, if you use a WEBCAM change input value in conf by -1")
            ,input.index);
        return -1;
    }

    if (curdev->starting) {
        MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
            ,_("Name = \"%s\", type 0x%08X, status %08x")
            ,input.name, input.type, input.status);
    }

    if ((input.type & V4L2_INPUT_TYPE_TUNER) && (curdev->starting)) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
            ,_("Name = \"%s\",- TUNER"),input.name);
    }

    if ((input.type & V4L2_INPUT_TYPE_CAMERA) && (curdev->starting)) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO,_("Name = \"%s\"- CAMERA"),input.name);
    }

    if (xioctl(vid_source, VIDIOC_S_INPUT, &input.index) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            , _("Error selecting input %d VIDIOC_S_INPUT"), input.index);
        return -1;
    }

    curdev->input        = cnt->param_input;
    curdev->device_type  = input.type;
    curdev->device_tuner = input.tuner;

    return 0;
}

static int v4l2_norm_select(struct context *cnt, struct video_dev *curdev)
{

    /* Set the video standard (norm) for the device NTSC/PAL/etc*/
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_standard standard;
    v4l2_std_id std_id;

    if ((cnt->param_norm == curdev->norm) && (!curdev->starting)) {
        return 0;
    }

    if (xioctl(vid_source, VIDIOC_G_STD, &std_id) == -1) {
        if (curdev->starting) {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
                ,_("Device does not support specifying PAL/NTSC norm"));
        }
        cnt->param_norm = std_id = 0;    // V4L2_STD_UNKNOWN = 0
    }

    if (std_id) {
        memset(&standard, 0, sizeof(struct v4l2_standard));
        standard.index = 0;

        while (xioctl(vid_source, VIDIOC_ENUMSTD, &standard) == 0) {
            if ((standard.id & std_id) && (curdev->starting)) {
                MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
                    ,_("- video standard %s"), standard.name);
            }
            standard.index++;
        }

        switch (cnt->param_norm) {
        case 1:
            std_id = V4L2_STD_NTSC;
            break;
        case 2:
            std_id = V4L2_STD_SECAM;
            break;
        default:
            std_id = V4L2_STD_PAL;
        }

        if (xioctl(vid_source, VIDIOC_S_STD, &std_id) == -1) {
            MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
                ,_("Error selecting standard method %d VIDIOC_S_STD")
                ,(int)std_id);
        }

        if (curdev->starting) {
            if (std_id == V4L2_STD_NTSC) {
                MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Video standard set to NTSC"));
            } else if (std_id == V4L2_STD_SECAM) {
                MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Video standard set to SECAM"));
            } else {
                MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Video standard set to PAL"));
            }
        }
    }

    curdev->norm = cnt->param_norm;

    return 0;
}

static int v4l2_frequency_select(struct context *cnt, struct video_dev *curdev)
{

    /* Set the frequency for the tuner */
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_tuner     tuner;
    struct v4l2_frequency freq;

    if ((cnt->param_freq == curdev->frequency) && (!curdev->starting)) {
        return 0;
    }

    /* If this input is attached to a tuner, set the frequency. */
    if (curdev->device_type & V4L2_INPUT_TYPE_TUNER) {
        /* Query the tuners capabilities. */
        memset(&tuner, 0, sizeof(struct v4l2_tuner));
        tuner.index = curdev->device_tuner;

        if (xioctl(vid_source, VIDIOC_G_TUNER, &tuner) == -1) {
            MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("tuner %d VIDIOC_G_TUNER"), tuner.index);
            return 0;
        }

        if (curdev->starting) {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Set tuner %d"), tuner.index);
        }

        /* Set the frequency. */
        memset(&freq, 0, sizeof(struct v4l2_frequency));
        freq.tuner = curdev->device_tuner;
        freq.type = V4L2_TUNER_ANALOG_TV;
        freq.frequency = (cnt->param_freq / 1000) * 16;

        if (xioctl(vid_source, VIDIOC_S_FREQUENCY, &freq) == -1) {
            MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("freq %ul VIDIOC_S_FREQUENCY"), freq.frequency);
            return 0;
        }

        if (curdev->starting) {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Set Frequency to %ul"), freq.frequency);
        }
    }

    curdev->frequency = cnt->param_freq;

    return 0;
}

static int v4l2_pixfmt_try(struct context *cnt, struct video_dev *curdev
    , u32 pixformat)
{
    int retcd;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_format *fmt = &vid_source->dst_fmt;

    memset(fmt, 0, sizeof(struct v4l2_format));
    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt->fmt.pix.width = cnt->conf.width;
    fmt->fmt.pix.height = cnt->conf.height;
    fmt->fmt.pix.pixelformat = pixformat;
    fmt->fmt.pix.field = V4L2_FIELD_ANY;

    retcd = xioctl(vid_source, VIDIOC_TRY_FMT, fmt);
    if ((retcd == -1) || (fmt->fmt.pix.pixelformat != pixformat)) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
            , _("Unable to use %c%c%c%c (%dx%d)")
            , pixformat >> 0, pixformat >> 8
            , pixformat >> 16, pixformat >> 24
            , cnt->conf.width, cnt->conf.height);
        return -1;
    }

    MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
        , _("Testing palette %c%c%c%c (%dx%d)")
        , pixformat >> 0, pixformat >> 8
        , pixformat >> 16, pixformat >> 24
        , cnt->conf.width, cnt->conf.height);


    return 0;

}

static int v4l2_pixfmt_stride(struct video_dev *curdev)
{
    int wd, bpl, wps;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;

    curdev->width = (int)vid_source->dst_fmt.fmt.pix.width;
    curdev->height = (int)vid_source->dst_fmt.fmt.pix.height;

    bpl = (int)vid_source->dst_fmt.fmt.pix.bytesperline;
    wd = curdev->width;

    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
        , _("Checking image size %dx%d with stride %d")
        , curdev->width, curdev->height, bpl);

    if (bpl == 0) {
        MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
            , _("No stride value provided from device."));
        return 0;
    }

    /* Documents indicate that stride is equal to width + padding
     * As such, the stride(bpl) will never be less than width
    */
    if (wd > bpl) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
            , _("Width(%d) must be less than stride(%d)"), wd, bpl);
        return -1;
    }

    /* For perfect multiples of width and stride, no adjustment needed */
    if ((wd == bpl) || ((bpl % wd) == 0)) {
        return 0;
    }

    MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
        , _("The image width(%d) is not multiple of the stride(%d)")
        , wd, bpl);

    /* Width per stride */
    wps = bpl / wd;
    if (wps < 1) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            , _("Impossible condition: Width(%d), Stride(%d), Per stride(%d)")
            , wd, bpl, wps);
    }

    MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
        , _("Image width will be padded %d bytes"), ((bpl % wd)/wps));

    curdev->width = (int)wd + ((bpl % wd)/wps);

    return 0;

}

/* Adjust requested resolution if needed*/
static int v4l2_pixfmt_adj(struct context *cnt, struct video_dev *curdev)
{

    if ((curdev->width != cnt->conf.width) ||
        (curdev->height != cnt->conf.height)) {

        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            ,_("Adjusting resolution from %ix%i to %ix%i.")
            ,cnt->conf.width, cnt->conf.height
            ,curdev->width, curdev->height);

        if ((curdev->width % 8) || (curdev->height % 8)) {
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
                ,_("Adjusted resolution not modulo 8."));
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
                ,_("Specify different palette or width/height in config file."));
            return -1;
        }

        cnt->conf.width = curdev->width;
        cnt->conf.height = curdev->height;

    }
    return 0;

}

static int v4l2_pixfmt_set(struct context *cnt, struct video_dev *curdev, u32 pixformat)
{
    int retcd;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_format *fmt = &vid_source->dst_fmt;

    retcd = v4l2_pixfmt_try(cnt, curdev, pixformat);
    if (retcd == -1) {
        return -1;
    }

    retcd = v4l2_pixfmt_stride(curdev);
    if (retcd == -1) {
        return -1;
    }

    retcd = v4l2_pixfmt_adj(cnt, curdev);
    if (retcd == -1) {
        return -1;
    }

    retcd = xioctl(vid_source, VIDIOC_S_FMT, fmt);
    if (retcd == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Error setting pixel format."));
        return -1;
    }

    curdev->pixfmt_src = pixformat;

    if (curdev->starting) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
            ,_("Using palette %c%c%c%c (%dx%d)")
            ,pixformat >> 0 , pixformat >> 8
            ,pixformat >> 16, pixformat >> 24
            ,cnt->conf.width, cnt->conf.height);
    }

    return 0;

}

static int v4l2_pixfmt_select(struct context *cnt, struct video_dev *curdev)
{
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_fmtdesc fmtd;
    int v4l2_pal, indx_palette, indx, retcd;
    palette_item *palette_array;

    palette_array = malloc(sizeof(palette_item) * (V4L2_PALETTE_COUNT_MAX+1));

    v4l2_palette_init(palette_array);

    if (cnt->conf.width % 8) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
            ,_("config image width (%d) is not modulo 8"), cnt->conf.width);
        cnt->conf.width = cnt->conf.width - (cnt->conf.width % 8) + 8;
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            , _("Adjusting to width (%d)"), cnt->conf.width);
    }

    if (cnt->conf.height % 8) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
            ,_("config image height (%d) is not modulo 8"), cnt->conf.height);
        cnt->conf.height = cnt->conf.height - (cnt->conf.height % 8) + 8;
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            ,_("Adjusting to height (%d)"), cnt->conf.height);
    }

    indx_palette = 17;
    for (indx = 0; indx < cnt->vdev->params_count; indx++) {
        if ( mystreq(cnt->vdev->params_array[indx].param_name, "palette")) {
            indx_palette = atoi(cnt->vdev->params_array[indx].param_value);
        };
    }

    if (indx_palette == 21) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
            ,_("H264(21) format not supported via videodevice.  Changing to default palette"));
        indx_palette = 17;
    }

    /* First we try setting the config file value */
    if ((indx_palette >= 0) && (indx_palette <= V4L2_PALETTE_COUNT_MAX)) {
        retcd = v4l2_pixfmt_set(cnt, curdev,palette_array[indx_palette].v4l2id);
        if (retcd >= 0) {
            free(palette_array);
            return 0;
        }
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
            ,_("Configuration palette index %d (%s) for %dx%d doesn't work.")
            , indx_palette, palette_array[indx_palette].fourcc
            ,cnt->conf.width, cnt->conf.height);
    }

    memset(&fmtd, 0, sizeof(struct v4l2_fmtdesc));
    fmtd.index = v4l2_pal = 0;
    fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    indx_palette = -1; /* -1 says not yet chosen */
    MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, _("Supported palettes:"));

    while (xioctl(vid_source, VIDIOC_ENUM_FMT, &fmtd) != -1) {
        if (curdev->starting) {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "(%i) %c%c%c%c (%s)",
                       v4l2_pal, fmtd.pixelformat >> 0,
                       fmtd.pixelformat >> 8, fmtd.pixelformat >> 16,
                       fmtd.pixelformat >> 24, fmtd.description);
            MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
                ,_("%d - %s (compressed : %d) (%#x)")
                ,fmtd.index, fmtd.description, fmtd.flags, fmtd.pixelformat);
        }
         /* Adjust indx_palette if larger value found */
         /* Prevent the selection of H264 since this module does not support it */
        for (indx = 0; indx <= V4L2_PALETTE_COUNT_MAX; indx++) {
            if ((palette_array[indx].v4l2id == fmtd.pixelformat) &&
                (palette_array[indx].v4l2id != V4L2_PIX_FMT_H264)) {
                indx_palette = indx;
            }
        }

        memset(&fmtd, 0, sizeof(struct v4l2_fmtdesc));
        fmtd.index = ++v4l2_pal;
        fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }

    if (indx_palette >= 0) {
        retcd = v4l2_pixfmt_set(cnt, curdev, palette_array[indx_palette].v4l2id);
        if (retcd >= 0) {
            if (curdev->starting) {
                MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
                    ,_("Selected palette %s")
                    ,palette_array[indx_palette].fourcc);
            }
            free(palette_array);
            return 0;
        }
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
            ,_("Palette selection failed for format %s")
            , palette_array[indx_palette].fourcc);
    }

    MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
        ,_("Unable to find a compatible palette format."));

    free(palette_array);

    return -1;


}

static int v4l2_mmap_request(struct video_dev *curdev)
{
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;

    memset(&vid_source->req, 0, sizeof(struct v4l2_requestbuffers));

    vid_source->req.count = MMAP_BUFFERS;
    vid_source->req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vid_source->req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(vid_source, VIDIOC_REQBUFS, &vid_source->req) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
                   ,_("Error requesting buffers %d for memory map. VIDIOC_REQBUFS")
                   ,vid_source->req.count);
        return -1;
    }
    curdev->buffer_count = vid_source->req.count;

    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
        ,_("mmap information: frames=%d"), curdev->buffer_count);

    if (curdev->buffer_count < MIN_MMAP_BUFFERS) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Insufficient buffer memory %d < MIN_MMAP_BUFFERS.")
            ,curdev->buffer_count);
        return -1;
    }

    vid_source->buffers = calloc(curdev->buffer_count, sizeof(video_buff));
    if (!vid_source->buffers) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO, _("Out of memory."));
        vid_source->buffers = NULL;
        return -1;
    }

    return 0;
}

static int v4l2_mmap_query(struct video_dev *curdev, int indx)
{
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buf, 0, sizeof(struct v4l2_buffer));
    memset(planes, 0, sizeof planes);

    buf.index = indx;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = VIDEO_MAX_PLANES;
    buf.m.planes = planes;

    if (xioctl(vid_source, VIDIOC_QUERYBUF, &buf) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Error querying buffer %i"), indx);
        free(vid_source->buffers);
        vid_source->buffers = NULL;
        return -1;
    }

    vid_source->buffers[indx].size = buf.length;

    vid_source->buffers[indx].ptr = mmap(NULL, buf.length
        , PROT_READ | PROT_WRITE, MAP_SHARED
        , vid_source->fd_device, buf.m.offset);

    if (vid_source->buffers[indx].ptr == MAP_FAILED) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Error mapping buffer %i mmap"), indx);
        free(vid_source->buffers);
        vid_source->buffers = NULL;
        return -1;
    }

    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
        , _("%i length=%d Address (%x) offset %d")
        , indx, buf.length, vid_source->buffers[indx].ptr
        , buf.m.offset);

    return 0;
}

static int v4l2_mmap_queue(struct video_dev *curdev, int indx)
{
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&vid_source->buf, 0, sizeof(struct v4l2_buffer));
    memset(planes, 0, sizeof planes);

    vid_source->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vid_source->buf.memory = V4L2_MEMORY_MMAP;
    vid_source->buf.index = indx;
    vid_source->buf.length = VIDEO_MAX_PLANES;
    vid_source->buf.m.planes = planes;

    if (xioctl(vid_source, VIDIOC_QBUF, &vid_source->buf) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO, "VIDIOC_QBUF");
        return -1;
    }
    return 0;

}

static int v4l2_mmap_set(struct video_dev *curdev)
{
    int retcd, indx;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    enum v4l2_buf_type type;

    if (!(vid_source->cap.capabilities & V4L2_CAP_STREAMING)) {
        return -1;
    }

    retcd = v4l2_mmap_request(curdev);
    if (retcd != 0) {
        return retcd;
    }

    for (indx = 0; indx < curdev->buffer_count; indx++) {
        retcd = v4l2_mmap_query(curdev, indx);
        if (retcd != 0) {
            return retcd;
        }
        retcd = v4l2_mmap_queue(curdev,indx);
        if (retcd != 0) {
            return retcd;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(vid_source, VIDIOC_STREAMON, &type) == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO
            ,_("Error starting stream. VIDIOC_STREAMON"));
        return -1;
    }

    return 0;
}

static int v4l2_imgs_set(struct context *cnt, struct video_dev *curdev)
{
    cnt->imgs.width = curdev->width;
    cnt->imgs.height = curdev->height;
    cnt->imgs.motionsize = cnt->imgs.width * cnt->imgs.height;
    cnt->imgs.size_norm = (cnt->imgs.motionsize * 3) / 2;
    cnt->conf.width = curdev->width;
    cnt->conf.height = curdev->height;

    return 0;

}

static int v4l2_pix_change(struct context *cnt, struct video_dev *curdev
    , uint8_t *dest)
{
    int retcd, width, height;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    video_buff *src;

    width = cnt->imgs.width;
    height = cnt->imgs.height;
    src = &vid_source->buffers[vid_source->buf.index];

    /*The FALLTHROUGH is a special comment required by compiler.*/
    switch (curdev->pixfmt_src) {
    case V4L2_PIX_FMT_RGB24:
        vid_rgb24toyuv420p(dest, src->ptr, width, height);
        return 0;
    case V4L2_PIX_FMT_UYVY:
        vid_uyvyto420p(dest, src->ptr, width, height);
        return 0;
    case V4L2_PIX_FMT_YUYV:
        vid_yuv422to420p(dest, src->ptr, width, height);
        return 0;
    case V4L2_PIX_FMT_YUV422P:
        vid_yuv422pto420p(dest, src->ptr, width, height);
        return 0;
    case V4L2_PIX_FMT_YUV420:
        memcpy(dest, src->ptr, cnt->imgs.size_norm);
        return 0;

    case V4L2_PIX_FMT_PJPG:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_JPEG:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_MJPEG:
        retcd = vid_mjpegtoyuv420p(dest
            , src->ptr, width, height, src->content_length);
        return retcd;

    case V4L2_PIX_FMT_SBGGR16:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_SGBRG8:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_SGRBG8:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_SBGGR8:    /* bayer */
        vid_bayer2rgb24(cnt->imgs.common_buffer, src->ptr, width, height);
        vid_rgb24toyuv420p(dest, cnt->imgs.common_buffer, width, height);
        return 0;

    case V4L2_PIX_FMT_SPCA561:
        /*FALLTHROUGH*/
    case V4L2_PIX_FMT_SN9C10X:
        vid_sonix_decompress(dest, src->ptr, width, height);
        vid_bayer2rgb24(cnt->imgs.common_buffer, dest, width, height);
        vid_rgb24toyuv420p(dest, cnt->imgs.common_buffer, width, height);
        return 0;

    case V4L2_PIX_FMT_Y10:
        vid_y10torgb24(cnt->imgs.common_buffer, src->ptr, width, height, 2);
        vid_rgb24toyuv420p(dest, cnt->imgs.common_buffer, width, height);
        return 0;
    case V4L2_PIX_FMT_Y12:
        vid_y10torgb24(cnt->imgs.common_buffer, src->ptr, width, height, 4);
        vid_rgb24toyuv420p(dest, cnt->imgs.common_buffer, width, height);
        return 0;
    case V4L2_PIX_FMT_GREY:
        vid_greytoyuv420p(dest, src->ptr, width, height);
        return 0;
    default:
        retcd = -1;

    }

    return -1;
}

static int v4l2_capture(struct video_dev *curdev)
{
    int retcd;
    sigset_t set, old;
    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];

    /* Block signals during IOCTL */
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    if (vid_source->pframe >= 0) {
        retcd = xioctl(vid_source, VIDIOC_QBUF, &vid_source->buf);
        if (retcd == -1) {
            MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO, "VIDIOC_QBUF");
            pthread_sigmask(SIG_UNBLOCK, &old, NULL);
            return retcd;
        }
    }

    memset(&vid_source->buf, 0, sizeof(struct v4l2_buffer));
    memset(planes, 0, sizeof planes);
    vid_source->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vid_source->buf.memory = V4L2_MEMORY_MMAP;
    vid_source->buf.bytesused = 0;
    vid_source->buf.length = VIDEO_MAX_PLANES;
    vid_source->buf.m.planes = planes;

    retcd = xioctl(vid_source, VIDIOC_DQBUF, &vid_source->buf);
    if (retcd == -1) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO, "VIDIOC_DQBUF");
        pthread_sigmask(SIG_UNBLOCK, &old, NULL);
        return retcd;
    }

    vid_source->pframe = vid_source->buf.index;
    vid_source->buffers[vid_source->buf.index].used = vid_source->buf.bytesused;
    vid_source->buffers[vid_source->buf.index].content_length = vid_source->buf.bytesused;
    pthread_sigmask(SIG_UNBLOCK, &old, NULL);

    return 0;
}

static int v4l2_device_init(struct context *cnt, struct video_dev *curdev)
{

    src_v4l2_t *vid_source;

    /* Allocate memory for the state structure. */
    if (!(vid_source = calloc(sizeof(src_v4l2_t), 1))) {
        MOTION_LOG(ERR, TYPE_VIDEO, SHOW_ERRNO, _("Out of memory."));
        vid_source = NULL;
        return -1;
    }

    pthread_mutexattr_init(&curdev->attr);
    pthread_mutex_init(&curdev->mutex, &curdev->attr);

    curdev->usage_count = 1;
    curdev->input = cnt->param_input;
    curdev->norm = cnt->param_norm;
    curdev->frequency = cnt->param_freq;
    curdev->height = cnt->conf.height;
    curdev->width = cnt->conf.width;

    curdev->devctrl_array = NULL;
    curdev->devctrl_count = 0;
    curdev->owner = -1;
    curdev->fps = 0;
    curdev->buffer_count= 0;

    curdev->v4l2_private = vid_source;
    vid_source->fd_device = curdev->fd_device;
    vid_source->fps = cnt->conf.framerate;
    vid_source->pframe = -1;
    vid_source->finish = &cnt->finish;
    vid_source->buffers = NULL;

    return 0;
}

static void v4l2_device_select(struct context *cnt, struct video_dev *curdev)
{
    int indx, retcd, newvals;

    if (curdev->v4l2_private == NULL) {
        MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO,_("Device not ready"));
        return;
    }

    vid_parms_parse(cnt);

    if ((cnt->param_input != curdev->input) ||
        (cnt->param_norm != curdev->norm) ||
        (cnt->param_freq != curdev->frequency)) {
        newvals = TRUE;
    } else {
        newvals = FALSE;
    }

    if (newvals) {
        retcd = v4l2_input_select(cnt, curdev);
        if (retcd == 0) {
            retcd = v4l2_norm_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_frequency_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_parms_set(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_autobright(cnt, curdev, cnt->conf.auto_brightness);
        }
        if (retcd == 0) {
            retcd = v4l2_ctrls_set(cnt, curdev);
        }
        if (retcd <  0) {
            MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                ,_("Errors occurred during device select"));
        }

        /* Clear the buffers from previous "robin" pictures*/
        for (indx =0; indx < curdev->buffer_count; indx++) {
            v4l2_capture(curdev);
        }

        /* Skip the requested round robin frame count */
        for (indx = 1; indx < cnt->conf.roundrobin_skip; indx++) {
            v4l2_capture(curdev);
        }

    } else {
        /* No round robin - we only adjust picture controls */
        retcd = v4l2_parms_set(cnt, curdev);
        if (retcd == 0) {
            retcd = v4l2_autobright(cnt, curdev, cnt->conf.auto_brightness);
        }
        if (retcd == 0) {
            retcd = v4l2_ctrls_set(cnt, curdev);
        }
        if (retcd < 0 ) {
            MOTION_LOG(WRN, TYPE_VIDEO, NO_ERRNO
                ,_("Errors occurred during device select"));
        }
    }
}

static int v4l2_device_open(struct context *cnt, struct video_dev *curdev)
{

    int fd_device;
    /* Open the video device */

    MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
        ,_("Using videodevice %s and input %d")
        ,cnt->conf.video_device, cnt->param_input);

    /* Give the watchdog more time for this open function */
    cnt->watchdog = (cnt->conf.watchdog_tmo * 2);

    curdev->video_device = cnt->conf.video_device;
    curdev->fd_device = -1;
    fd_device = -1;

    fd_device = open(curdev->video_device, O_RDWR|O_CLOEXEC);
    if (fd_device > 0) {
        curdev->fd_device = fd_device;
        src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
        vid_source->fd_device = fd_device;
        return 0;
    }

    MOTION_LOG(ALR, TYPE_VIDEO, SHOW_ERRNO
        ,_("Failed to open video device %s")
        ,cnt->conf.video_device);
    return -1;

}

static void v4l2_device_close(struct video_dev *curdev)
{

    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (vid_source != NULL) {
        xioctl(vid_source, VIDIOC_STREAMOFF, &type);
    }

    if (vid_source->fd_device != -1) {
        close(vid_source->fd_device);
        vid_source->fd_device = -1;
    }
}

static void v4l2_device_cleanup(struct video_dev *curdev)
{

    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;

    unsigned int indx;
    int indx2;

    if (vid_source->buffers != NULL) {
        for (indx = 0; indx < vid_source->req.count; indx++) {
            munmap(vid_source->buffers[indx].ptr, vid_source->buffers[indx].size);
        }
        free(vid_source->buffers);
        vid_source->buffers = NULL;
    }

    if (vid_source != NULL) {
        free(vid_source);
        curdev->v4l2_private = NULL;
    }

    if (curdev->devctrl_count != 0) {
        for (indx2=0; indx2 < curdev->devctrl_count; indx2++) {
            free(curdev->devctrl_array[indx2].ctrl_iddesc);
            free(curdev->devctrl_array[indx2].ctrl_name);
            curdev->devctrl_array[indx2].ctrl_iddesc = NULL;
            curdev->devctrl_array[indx2].ctrl_name = NULL;
        }
        free(curdev->devctrl_array);
        curdev->devctrl_array = NULL;
    }
    curdev->devctrl_count=0;

}

static int v4l2_device_capability(struct video_dev *curdev)
{

    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;

    if (xioctl(vid_source, VIDIOC_QUERYCAP, &vid_source->cap) < 0) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, _("Not a V4L2 device?"));
        return -1;
    }

    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "------------------------");
    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "cap.driver: \"%s\"",vid_source->cap.driver);
    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "cap.card: \"%s\"",vid_source->cap.card);
    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "cap.bus_info: \"%s\"",vid_source->cap.bus_info);
    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "cap.capabilities=0x%08X",vid_source->cap.capabilities);
    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, "------------------------");

    if (vid_source->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- VIDEO_CAPTURE");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- VIDEO_OUTPUT");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_VIDEO_OVERLAY) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- VIDEO_OVERLAY");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_VBI_CAPTURE) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- VBI_CAPTURE");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_VBI_OUTPUT) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- VBI_OUTPUT");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_RDS_CAPTURE) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- RDS_CAPTURE");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_TUNER) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- TUNER");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_AUDIO) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- AUDIO");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_READWRITE) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- READWRITE");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_ASYNCIO) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- ASYNCIO");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_STREAMING) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- STREAMING");
    }
    if (vid_source->cap.capabilities & V4L2_CAP_TIMEPERFRAME) {
        MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "- TIMEPERFRAME");
    }

    if (!(vid_source->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, _("Device does not support capturing."));
        return -1;
    }

    return 0;
}

static int v4l2_fps_set(struct context *cnt, struct video_dev *curdev)
{

    src_v4l2_t *vid_source = (src_v4l2_t *) curdev->v4l2_private;
    struct v4l2_streamparm setfps;
    int retcd;

    memset(&setfps, 0, sizeof(struct v4l2_streamparm));
    setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setfps.parm.capture.timeperframe.numerator = 1;
    setfps.parm.capture.timeperframe.denominator = cnt->conf.framerate;

    MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO
        , _("Trying to set fps to %d")
        , setfps.parm.capture.timeperframe.denominator);

    retcd = xioctl(vid_source, VIDIOC_S_PARM, &setfps);
    if (retcd != 0) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO
            ,_("Error setting fps. Return code %d"), retcd);
    }

    MOTION_LOG(INF, TYPE_VIDEO, NO_ERRNO
        , _("Device set fps to %d")
        , setfps.parm.capture.timeperframe.denominator);

    return 0;
}

#endif /* HAVE_V4L2 */

void v4l2_mutex_init(void)
{
    #ifdef HAVE_V4L2
        pthread_mutex_init(&v4l2_mutex, NULL);
    #else
        MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, _("V4L2 is not enabled"));
    #endif // HAVE_V4L2
}

void v4l2_mutex_destroy(void)
{
    #ifdef HAVE_V4L2
        pthread_mutex_destroy(&v4l2_mutex);
    #else
        return;
    #endif // HAVE_V4L2
}

int v4l2_start(struct context *cnt)
{
    #ifdef HAVE_V4L2

        int retcd;
        struct video_dev *curdev;

        pthread_mutex_lock(&v4l2_mutex);

        /* If device is already open and initialized use it*/
        curdev = video_devices;
        while (curdev) {
            if (mystreq(cnt->conf.video_device, curdev->video_device)) {
                v4l2_vdev_init(cnt);

                vid_parms_parse(cnt);

                retcd = v4l2_imgs_set(cnt, curdev);

                if (retcd == 0) {
                    curdev->usage_count++;
                    retcd = curdev->fd_device;
                }
                pthread_mutex_unlock(&v4l2_mutex);
                return retcd;
            }
            curdev = curdev->next;
        }

        curdev = mymalloc(sizeof(struct video_dev));

        curdev->starting = TRUE;

        v4l2_vdev_init(cnt);

        vid_parms_parse(cnt);

        retcd = v4l2_device_init(cnt, curdev);
        if (retcd == 0) {
            retcd = v4l2_device_open(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_device_capability(curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_input_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_norm_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_frequency_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_pixfmt_select(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_fps_set(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_ctrls_count(curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_ctrls_list(curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_parms_set(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_ctrls_set(cnt, curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_mmap_set(curdev);
        }
        if (retcd == 0) {
            retcd = v4l2_imgs_set(cnt, curdev);
        }
        if (retcd < 0) {
            /* These may need more work to consider all the fail scenarios*/
            if (curdev->v4l2_private != NULL) {
                free(curdev->v4l2_private);
                curdev->v4l2_private = NULL;
            }
            pthread_mutexattr_destroy(&curdev->attr);
            pthread_mutex_destroy(&curdev->mutex);
            v4l2_vdev_free(cnt);
            if (curdev->fd_device != -1) {
                close(curdev->fd_device);
            }
            free(curdev);
            pthread_mutex_unlock(&v4l2_mutex);
            return retcd;
        }

        curdev->starting = FALSE;

        /* Insert into linked list. */
        curdev->next = video_devices;
        video_devices = curdev;

        pthread_mutex_unlock(&v4l2_mutex);

        return curdev->fd_device;
    #else
        (void)cnt;
        MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO, _("V4L2 is not enabled."));
        return -1;
    #endif // HAVE_V4l2
}

void v4l2_cleanup(struct context *cnt)
{
    #ifdef HAVE_V4L2

        struct video_dev *dev = video_devices;
        struct video_dev *prev = NULL;

        /* Cleanup the v4l2 part */
        pthread_mutex_lock(&v4l2_mutex);
        while (dev) {
            if (dev->fd_device == cnt->video_dev) {
                break;
            }
            prev = dev;
            dev = dev->next;
        }
        pthread_mutex_unlock(&v4l2_mutex);

        /* Set it as closed in thread context. */
        cnt->video_dev = -1;

        v4l2_vdev_free(cnt);

        if (dev == NULL) {
            MOTION_LOG(CRT, TYPE_VIDEO, NO_ERRNO, _("Unable to find video device"));
            return;
        }

        if (--dev->usage_count == 0) {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
                ,_("Closing video device %s"), dev->video_device);

            v4l2_device_close(dev);
            v4l2_device_cleanup(dev);

            dev->fd_device = -1;

            /* Remove from list */
            if (prev == NULL) {
                video_devices = dev->next;
            } else {
                prev->next = dev->next;
            }

            pthread_mutexattr_destroy(&dev->attr);
            pthread_mutex_destroy(&dev->mutex);
            free(dev);

        } else {
            MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO
                ,_("Still %d users of video device %s, so we don't close it now")
                ,dev->usage_count, dev->video_device);
            /*
            * There is still at least one thread using this device
            * If we own it, release it.
            */
            if (dev->owner == cnt->threadnr) {
                dev->frames = 0;
                dev->owner = -1;
                pthread_mutex_unlock(&dev->mutex);
            }
        }


    #else
        (void)cnt;
    #endif // HAVE_V4L2
}

int v4l2_next(struct context *cnt, struct image_data *img_data)
{
    #ifdef HAVE_V4L2
        int retcd = -2;
        struct config *conf = &cnt->conf;
        struct video_dev *dev;

        pthread_mutex_lock(&v4l2_mutex);
        dev = video_devices;
        while (dev) {
            if (dev->fd_device == cnt->video_dev) {
                break;
            }
            dev = dev->next;
        }
        pthread_mutex_unlock(&v4l2_mutex);

        if (dev == NULL) {
            return -1;
        }

        if (dev->owner != cnt->threadnr) {
            pthread_mutex_lock(&dev->mutex);
            dev->owner = cnt->threadnr;
            dev->frames = conf->roundrobin_frames;
        }

        v4l2_device_select(cnt, dev);

        retcd = v4l2_capture(dev);

        if (retcd == 0) {
            retcd = v4l2_pix_change(cnt, dev, img_data->image_norm);
        }

        if (--dev->frames <= 0) {
            dev->owner = -1;
            dev->frames = 0;
            pthread_mutex_unlock(&dev->mutex);
        }
        if (retcd == 0) {
            rotate_map(cnt, img_data);
        }

        return retcd;
    #else
        (void)cnt;
        (void)img_data;
        return -1;
    #endif // HAVE_V4L2
}

int v4l2_palette_valid(char *video_device, int v4l2_palette)
{
    #ifdef HAVE_V4L2

        /* This function is a boolean that returns true(1) if the palette selected in the
        * configuration file is valid for the device and false(0) if the palette is not valid
        */

        palette_item *palette_array;
        struct v4l2_fmtdesc fmtd;

        int device_palette;
        int retcd;
        src_v4l2_t *vid_source;

        palette_array = malloc(sizeof(palette_item) * (V4L2_PALETTE_COUNT_MAX+1));

        v4l2_palette_init(palette_array);

        vid_source = calloc(sizeof(src_v4l2_t), 1);
        vid_source->fd_device = open(video_device, O_RDWR|O_CLOEXEC);
        if (vid_source->fd_device < 0) {
            MOTION_LOG(ALR, TYPE_VIDEO, SHOW_ERRNO
                ,_("Failed to open video device %s"),video_device);
            free(vid_source);
            free(palette_array);
            return 0;
        }

        memset(&fmtd, 0, sizeof(struct v4l2_fmtdesc));
        fmtd.index = device_palette = 0;
        fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        retcd = 0;
        while (xioctl(vid_source, VIDIOC_ENUM_FMT, &fmtd) != -1) {
            if (palette_array[v4l2_palette].v4l2id == fmtd.pixelformat) {
                retcd = 1;
            }
            memset(&fmtd, 0, sizeof(struct v4l2_fmtdesc));
            fmtd.index = ++device_palette;
            fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        }

        close(vid_source->fd_device);

        free(vid_source);

        free(palette_array);

        return retcd;
    #else
        (void)video_device;
        (void)v4l2_palette;
        return 0;
    #endif // HAVE_V4L2
}

void v4l2_palette_fourcc(int v4l2_palette, char *fourcc)
{
    #ifdef HAVE_V4L2

        /* This function populates the provided fourcc pointer with the fourcc code for the
        * requested palette id code.  If the palette is not one of the ones that Motion supports
        * it returns the string as "NULL"
        */

        palette_item *palette_array;

        palette_array = malloc(sizeof(palette_item) * (V4L2_PALETTE_COUNT_MAX+1));

        v4l2_palette_init(palette_array);

        if ((v4l2_palette > V4L2_PALETTE_COUNT_MAX) || (v4l2_palette < 0)) {
            sprintf(fourcc,"%s","NULL");
        } else {
            sprintf(fourcc,"%s",palette_array[v4l2_palette].fourcc);
        }

        free(palette_array);

        return;
    #else
        sprintf(fourcc,"%s","NULL");
        (void)v4l2_palette;
        return;
    #endif // HAVE_V4L2
}

int v4l2_parms_valid(char *video_device, int v4l2_palette, int v4l2_fps, int v4l2_width, int v4l2_height)
{
    #ifdef HAVE_V4L2

        /* This function is a boolean that returns true(1) if the parms selected in the
        * configuration file are valid for the device and false(0) if not valid
        */
        palette_item *palette_array;
        struct v4l2_fmtdesc         dev_format;
        struct v4l2_frmsizeenum     dev_sizes;
        struct v4l2_frmivalenum     dev_frameint;

        int retcd;
        int indx_format, indx_sizes, indx_frameint;

        src_v4l2_t *vid_source;

        palette_array = malloc(sizeof(palette_item) * (V4L2_PALETTE_COUNT_MAX+1));

        v4l2_palette_init(palette_array);

        vid_source = calloc(sizeof(src_v4l2_t), 1);
        vid_source->fd_device = open(video_device, O_RDWR|O_CLOEXEC);
        if (vid_source->fd_device < 0) {
            MOTION_LOG(ALR, TYPE_VIDEO, SHOW_ERRNO
                ,_("Failed to open video device %s"),video_device);
            free(vid_source);
            free(palette_array);
            return 0;
        }

        retcd = 0;
        memset(&dev_format, 0, sizeof(struct v4l2_fmtdesc));
        dev_format.index = indx_format = 0;
        dev_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        while (xioctl(vid_source, VIDIOC_ENUM_FMT, &dev_format) != -1) {
            MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
                ,_("Testing palette %s (%c%c%c%c)")
                ,dev_format.description
                ,dev_format.pixelformat >> 0
                ,dev_format.pixelformat >> 8
                ,dev_format.pixelformat >> 16
                ,dev_format.pixelformat >> 24);

            memset(&dev_sizes, 0, sizeof(struct v4l2_frmsizeenum));
            dev_sizes.index = indx_sizes = 0;
            dev_sizes.pixel_format = dev_format.pixelformat;
            while (xioctl(vid_source, VIDIOC_ENUM_FRAMESIZES, &dev_sizes) != -1) {
                MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
                    ,_("  Width: %d, Height %d")
                    ,dev_sizes.discrete.width
                    ,dev_sizes.discrete.height);

                memset(&dev_frameint, 0, sizeof(struct v4l2_frmivalenum));
                dev_frameint.index = indx_frameint = 0;
                dev_frameint.pixel_format = dev_format.pixelformat;
                dev_frameint.width = dev_sizes.discrete.width;
                dev_frameint.height = dev_sizes.discrete.height;
                while (xioctl(vid_source, VIDIOC_ENUM_FRAMEINTERVALS, &dev_frameint) != -1) {
                    MOTION_LOG(DBG, TYPE_VIDEO, NO_ERRNO
                        ,_("    Framerate %d/%d")
                        ,dev_frameint.discrete.numerator
                        ,dev_frameint.discrete.denominator);
                    if ((palette_array[v4l2_palette].v4l2id == dev_format.pixelformat) &&
                        ((int)dev_sizes.discrete.width == v4l2_width) &&
                        ((int)dev_sizes.discrete.height == v4l2_height) &&
                        ((int)dev_frameint.discrete.numerator == 1) &&
                        ((int)dev_frameint.discrete.denominator == v4l2_fps)) {
                        retcd = 1;
                    }
                    memset(&dev_frameint, 0, sizeof(struct v4l2_frmivalenum));
                    dev_frameint.index = ++indx_frameint;
                    dev_frameint.pixel_format = dev_format.pixelformat;
                    dev_frameint.width = dev_sizes.discrete.width;
                    dev_frameint.height = dev_sizes.discrete.height;
                }
                memset(&dev_sizes, 0, sizeof(struct v4l2_frmsizeenum));
                dev_sizes.index = ++indx_sizes;
                dev_sizes.pixel_format = dev_format.pixelformat;
            }
            memset(&dev_format, 0, sizeof(struct v4l2_fmtdesc));
            dev_format.index = ++indx_format;
            dev_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        }

        close(vid_source->fd_device);

        free(vid_source);

        free(palette_array);

        return retcd;
    #else
        (void)video_device;
        (void)v4l2_fps;
        (void)v4l2_palette;
        (void)v4l2_width;
        (void)v4l2_height;
        return 0;
    #endif // HAVE_V4L2
}

