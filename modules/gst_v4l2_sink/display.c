/**
 * @file gst_v4l2_sink/display.c  Render to V4L2 sink using Gstreamer video pipeline
 *
 * Copyright (C) 2017 Highfive, Inc.
 */

#define __USE_POSIX199309
#define _DEFAULT_SOURCE 1
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsrc.h>
#include "gst_v4l2_sink.h"


struct vidsink_state {
	struct vidsz size;
    const char* dev;
	/* Gstreamer */
	struct {
		bool valid;

		GstElement *pipeline;
		GstAppSrc *source;

		GstAppSrcCallbacks appsrcCallbacks;

		/* Thread synchronization. */
		struct {
			pthread_mutex_t mutex;
			pthread_cond_t cond;
			/* 0: no-wait, 1: wait, -1: pipeline destroyed */
			int flag;
		} wait;
	} streamer;
};


static void appsrc_need_data_cb(GstAppSrc *src, guint size, gpointer user_data)
{
	struct vidsink_state *st = user_data;
	(void)src;
	(void)size;

	pthread_mutex_lock(&st->streamer.wait.mutex);
	if (st->streamer.wait.flag == 1){
		st->streamer.wait.flag = 0;
		pthread_cond_signal(&st->streamer.wait.cond);
	}
	pthread_mutex_unlock(&st->streamer.wait.mutex);
}


static void appsrc_enough_data_cb(GstAppSrc *src, gpointer user_data)
{
	struct vidsink_state *st = user_data;
	(void)src;

	pthread_mutex_lock(&st->streamer.wait.mutex);
	if (st->streamer.wait.flag == 0)
		st->streamer.wait.flag = 1;
	pthread_mutex_unlock(&st->streamer.wait.mutex);
}


static void appsrc_destroy_notify_cb(struct vidsink_state *st)
{
	pthread_mutex_lock(&st->streamer.wait.mutex);
	st->streamer.wait.flag = -1;
	pthread_cond_signal(&st->streamer.wait.cond);
	pthread_mutex_unlock(&st->streamer.wait.mutex);
}

static GstBusSyncReply bus_sync_handler_cb(GstBus *bus, GstMessage *msg,
					   struct vidsink_state *st)
{
	(void)bus;

	if ((GST_MESSAGE_TYPE (msg)) == GST_MESSAGE_ERROR) {
		GError *err = NULL;
		gchar *dbg_info = NULL;
		gst_message_parse_error (msg, &err, &dbg_info);
		warning("gst_video: Error: %d(%m) message=%s\n",
			err->code, err->code, err->message);
		warning("gst_video: Debug: %s\n", dbg_info);
		g_error_free (err);
		g_free (dbg_info);

		/* mark pipeline as broked */
		st->streamer.valid = false;
	}

	gst_message_unref(msg);
	return GST_BUS_DROP;
}


static void bus_destroy_notify_cb(struct vidsink_state *st)
{
	(void)st;
}


/**
 * Set up the Gstreamer pipeline. Appsrc gets raw frames, and we feed that
 * into the V4L2 sink element.
 *
 * The pipeline looks like this:
 *
 * <pre>
 *  .--------.   .----------.
 *  | appsrc |   | v4l2sink |
 *  |   .----|   |----.     |
 *  |   |src |-->|sink|     |
 *  |   '----|   |----'     |
 *  '--------'   '----------'
 * </pre>
 */
static int pipeline_init(struct vidsink_state *st, const struct vidsz *size)
{
	GstAppSrc *source;
	GstBus *bus;
	GError* gerror = NULL;
	char pipeline[1024];
	GstStateChangeReturn ret;
	int err = 0;

	if (!st || !size)
		return EINVAL;

    const char* dev = (st->dev == NULL) ? "/dev/video0" : st->dev;
	snprintf(pipeline, sizeof(pipeline),
	 "appsrc name=source is-live=TRUE block=TRUE "
	 "do-timestamp=TRUE max-bytes=6000000 ! "
	 "capsfilter caps=\"video/x-raw,width=%d,height=%d,format=I420,framerate=30/1,interlace-mode=progressive\" ! "
	 "v4l2sink name=sink async=FALSE sync=FALSE device=%s",
	 size->w, size->h, dev);

	/* Initialize pipeline. */
	st->streamer.pipeline = gst_parse_launch(pipeline, &gerror);

	if (gerror) {
		warning("gst_video: launch error: %d: %s: %s\n",
			gerror->code, gerror->message, pipeline);
		err = gerror->code;
		g_error_free(gerror);
		return err;
	}

	/* Configure appsource */
	source = GST_APP_SRC(gst_bin_get_by_name(
				 GST_BIN(st->streamer.pipeline), "source"));
	gst_app_src_set_callbacks(source, &(st->streamer.appsrcCallbacks),
			  st, (GDestroyNotify)appsrc_destroy_notify_cb);

	/* Bus watch */
	bus = gst_pipeline_get_bus(GST_PIPELINE(st->streamer.pipeline));
	gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler_cb,
				 st, (GDestroyNotify)bus_destroy_notify_cb);
	gst_object_unref(GST_OBJECT(bus));

	/* Set start values of locks */
	pthread_mutex_lock(&st->streamer.wait.mutex);
	st->streamer.wait.flag = 0;
	pthread_mutex_unlock(&st->streamer.wait.mutex);

	/* Start pipeline */
	ret = gst_element_set_state(st->streamer.pipeline, GST_STATE_PLAYING);
	if (GST_STATE_CHANGE_FAILURE == ret) {
		g_warning("set state returned GST_STATE_CHANGE_FAILURE\n");
		err = EPROTO;
		goto out;
	}

	st->streamer.source = source;

	/* Mark pipeline as working */
	st->streamer.valid = true;

 out:
	return err;
}


static void pipeline_close(struct vidsink_state *st)
{
	if (!st)
		return;

	st->streamer.valid = false;

	if (st->streamer.source) {
		gst_object_unref(GST_OBJECT(st->streamer.source));
		st->streamer.source = NULL;
	}

	if (st->streamer.pipeline) {
		gst_element_set_state(st->streamer.pipeline, GST_STATE_NULL);

		/* pipeline */
		gst_object_unref(GST_OBJECT(st->streamer.pipeline));
		st->streamer.pipeline = NULL;
	}
}


static void destruct_resources(void *data)
{
	struct vidsink_state *st = data;

	/* close pipeline */
	pipeline_close(st);

	/* destroy lock */
	pthread_mutex_destroy(&st->streamer.wait.mutex);
	pthread_cond_destroy(&st->streamer.wait.cond);
}


int gst_v4l2_sink_alloc(struct vidsink_state **stp, const char* dev)
{
	struct vidsink_state *st;

	st = mem_zalloc(sizeof(*st), destruct_resources);
	if (!st)
		return ENOMEM;

	*stp = st;

    st->dev = dev;

	/* initialize lock */
	pthread_mutex_init(&st->streamer.wait.mutex, NULL);
	pthread_cond_init(&st->streamer.wait.cond, NULL);


	/* Set appsource callbacks. */
	st->streamer.appsrcCallbacks.need_data = &appsrc_need_data_cb;
	st->streamer.appsrcCallbacks.enough_data = &appsrc_enough_data_cb;

	return 0;
}


/*
 * couple gstreamer tightly by lock-stepping
 */
static int pipeline_push(struct vidsink_state *st, const struct vidframe *frame)
{
	GstBuffer *buffer;
	uint8_t *data;
	size_t size;
	GstFlowReturn ret;
	int err = 0;

#if 1
	/* XXX: should not block the function here */

	/*
	 * Wait "start feed".
	 */
	pthread_mutex_lock(&st->streamer.wait.mutex);
	if (st->streamer.wait.flag == 1) {
		pthread_cond_wait(&st->streamer.wait.cond,
				  &st->streamer.wait.mutex);
	}
	pthread_mutex_unlock(&st->streamer.wait.mutex);
	if (err)
		return err;
#endif

	/*
	 * Copy frame into buffer for gstreamer
	 */

	/* NOTE: I420 (YUV420P): hardcoded. */
	size = frame->linesize[0] * frame->size.h
		+ frame->linesize[1] * frame->size.h * 0.5
		+ frame->linesize[2] * frame->size.h * 0.5;

	/* allocate memory; memory is freed within callback of
	   gst_memory_new_wrapped of gst_video_push */
	data = g_try_malloc(size);
	if (!data)
		return ENOMEM;

	/* copy content of frame */
	size = 0;
	memcpy(&data[size], frame->data[0],
	       frame->linesize[0] * frame->size.h);
	size += frame->linesize[0] * frame->size.h;
	memcpy(&data[size], frame->data[1],
	       frame->linesize[1] * frame->size.h * 0.5);
	size += frame->linesize[1] * frame->size.h * 0.5;
	memcpy(&data[size], frame->data[2],
	       frame->linesize[2] * frame->size.h * 0.5);
	size += frame->linesize[2] * frame->size.h * 0.5;

	/* Wrap memory in a gstreamer buffer */
	buffer = gst_buffer_new();
	gst_buffer_insert_memory(buffer, -1,
				 gst_memory_new_wrapped (0, data, size, 0,
							 size, data, g_free));

	/*
	 * Push data into gstreamer.
	 */

	ret = gst_app_src_push_buffer(st->streamer.source, buffer);
	if (ret != GST_FLOW_OK) {
		warning("gst_video: pushing buffer failed\n");
		err = EPROTO;
		goto out;
	}

 out:
	return err;
}


int gst_v4l2_sink_display(struct vidsink_state *st, const struct vidframe *frame)
{
	int err;

	if (!st || !frame || frame->fmt != VID_FMT_YUV420P)
		return EINVAL;

	if (!st->streamer.valid || !vidsz_cmp(&st->size, &frame->size)) {

		pipeline_close(st);

		err = pipeline_init(st, &frame->size);
		if (err) {
			warning("gst_video: pipeline initialization failed\n");
			return err;
		}

		st->size = frame->size;
	}

	/*
	 * Push frame into pipeline.
	 * Function call will return once frame has been processed completely.
	 */
	err = pipeline_push(st, frame);

	return err;
}
