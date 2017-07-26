/**
 * @file gst_v4l2_sink/gst_v4l2_sink.h  Gstreamer video sink pipeline -- internal API
 *
 * Copyright (C) 2017 Highfive, Inc.
 */


/* Display */
struct vidsink_state;

int gst_v4l2_sink_alloc(struct vidsink_state **stp, const char* dev);
int gst_v4l2_sink_display(struct vidsink_state *st, const struct vidframe *frame);

