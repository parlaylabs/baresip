/**
 * @file gst_v4l2_sink.c Fake video source and video display
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#include <unistd.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <gst/gst.h>
#include "gst_v4l2_sink.h"


/**
 * @defgroup gst_v4l2_sink gst_v4l2_sink
 *
 * V4L2 sink display module
 *
 * This module can be used to "display" data to a V4L2 sink.
 * It makes use of the gstreamer v4l2sink plugin.
 *
 * Example config:
 \verbatim
  video_display   gst_v4l2_sink,/dev/video1
 \endverbatim
 */

static int gst_argcount = 4;
static char* gst_args[] = {"dummy", "--gst-debug-level=2", "--gst-plugin-spew"};
static char** gst_args_array = gst_args;

struct vidisp_st {
    const struct vidisp *vd;  /* inheritance */
    struct vidsink_state* state;
};

static struct vidisp *vidisp;

static void destructor(void *arg)
{
    struct vidisp_st *st = arg;
    if (st->state)
        mem_deref(st->state);
}

static int alloc(struct vidisp_st **stp, const struct vidisp *vd,
              struct vidisp_prm *prm, const char *dev,
              vidisp_resize_h *resizeh, void *arg)
{
    struct vidisp_st *st;
    (void)prm;
    (void)resizeh;
    (void)arg;

    if (!stp || !vd)
        return EINVAL;

    st = mem_zalloc(sizeof(*st), destructor);
    if (!st)
        return ENOMEM;

    st->vd = vd;
    gst_v4l2_sink_alloc(&st->state, dev);

    *stp = st;

    return 0;
}


static int display(struct vidisp_st *st, const char *title,
           const struct vidframe *frame)
{
    (void)title;
    gst_v4l2_sink_display(st->state, frame);

    return 0;
}


static int module_init(void)
{
    int err = 0;
	gst_init(&gst_argcount, &gst_args_array);
    err |= vidisp_register(&vidisp, baresip_vidispl(),
                   "gst_v4l2_sink", alloc, NULL,
                   display, NULL);
    return err;
}


static int module_close(void)
{
    vidisp = mem_deref(vidisp);
    gst_deinit();
    return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(gst_v4l2_sink) = {
    "gst_v4l2_sink",
    "gst_v4l2_sink",
    module_init,
    module_close
};
