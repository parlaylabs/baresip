/**
 * @file gst_v4l2_sink/display.c  Video codecs using Gstreamer video pipeline
 *
 * Copyright (C) 2010 - 2013 Creytiv.com
 * Copyright (C) 2014 Fadeev Alexander
 */
#define _DEFAULT_SOURCE 1
#define __USE_POSIX199309
#define _BSD_SOURCE 1
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
	GstElement *pipeline, *source, *sink;
	GstBus *bus;
	gulong need_data_handler;
	gulong enough_data_handler;
	gulong new_buffer_handler;
	bool gst_inited;

	/* Main loop thread. */
	int run;
	pthread_t tid;

	/* Thread synchronization. */
	pthread_mutex_t mutex;
	pthread_cond_t wait;
	int bwait;
};


static void gst_v4l2_sink_close(struct vidsink_state *st);


static void internal_bus_watch_handler(struct vidsink_state *st)
{
	GError *err;
	gchar *d;
	GstMessage *msg = gst_bus_pop(st->bus);

	if (!msg) {
		/* take a nap (300ms) */
		usleep(300 * 1000);
		return;
	}

	switch (GST_MESSAGE_TYPE(msg)) {

	case GST_MESSAGE_EOS:

		/* XXX decrementing repeat count? */

		/* Re-start stream */
		gst_element_set_state(st->pipeline, GST_STATE_NULL);
		gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
		break;

	case GST_MESSAGE_ERROR:
		gst_message_parse_error(msg, &err, &d);

		warning("gst_v4l2_sink: Error: %d(%m) message=%s\n", err->code,
			err->code, err->message);
		warning("gst_v4l2_sink: Debug: %s\n", d);

		g_free(d);
		g_error_free(err);

		st->run = FALSE;
		break;

	default:
		break;
	}

	gst_message_unref(msg);
}


static void *internal_thread(void *arg)
{
	struct vidsink_state *st = arg;

	/* Now set to playing and iterate. */
	debug("gst_v4l2_sink: Setting pipeline to PLAYING\n");

	gst_element_set_state(st->pipeline, GST_STATE_PLAYING);

	while (st->run) {
		internal_bus_watch_handler(st);
	}

	debug("gst_v4l2_sink: Pipeline thread was stopped.\n");

	return NULL;
}


static void internal_appsrc_start_feed(GstElement * pipeline, guint size,
				       struct vidsink_state *st)
{
	(void)pipeline;
	(void)size;

	if (!st)
		return;

	pthread_mutex_lock(&st->mutex);
	st->bwait = FALSE;
	pthread_cond_signal(&st->wait);
	pthread_mutex_unlock(&st->mutex);
}


static void internal_appsrc_stop_feed(GstElement * pipeline,
				      struct vidsink_state *st)
{
	(void)pipeline;

	if (!st)
		return;

	pthread_mutex_lock(&st->mutex);
	st->bwait = TRUE;
	pthread_mutex_unlock(&st->mutex);
}

/**
 * Set up the Gstreamer pipeline. Appsrc gets raw frames, and pushes
 * them into the V4L2 sink.
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
static int gst_v4l2_sink_init(struct vidsink_state *st, int width, int height)
{
	GError* gerror = NULL;
	char pipeline[1024];
	int err = 0;

	gst_v4l2_sink_close(st);

    const char* dev = (st->dev == NULL) ? "/dev/video0" : st->dev;
	snprintf(pipeline, sizeof(pipeline),
	 "appsrc name=source is-live=TRUE block=TRUE do-timestamp=TRUE ! "
	 "capsfilter caps=\"video/x-raw-yuv,width=%d,height=%d,format=(fourcc)I420,framerate=30/1\" ! "
	 "v4l2sink name=sink async=FALSE sync=FALSE device=%s",
	 width, height, dev);

	debug("gst_v4l2_sink: format: yu12 = yuv420p = i420\n");

	/* Initialize pipeline. */
	st->pipeline = gst_parse_launch(pipeline, &gerror);
	if (gerror) {
		warning("gst_v4l2_sink: launch error: %s: %s\n",
			gerror->message, pipeline);
		err = gerror->code;
		g_error_free(gerror);
		goto out;
	}

	st->source = gst_bin_get_by_name(GST_BIN(st->pipeline), "source");
	if (!st->source) {
		warning("gst_v4l2_sink: failed to get source element\n");
		err = ENOMEM;
		goto out;
	}

	/* Configure appsource */
	st->need_data_handler = g_signal_connect(st->source, "need-data",
				 G_CALLBACK(internal_appsrc_start_feed), st);
	st->enough_data_handler = g_signal_connect(st->source, "enough-data",
				   G_CALLBACK(internal_appsrc_stop_feed), st);

	/********************* Misc **************************/

	/* Bus watch */
	st->bus = gst_pipeline_get_bus(GST_PIPELINE(st->pipeline));

	/********************* Thread **************************/

	st->bwait = FALSE;

	err = gst_element_set_state(st->pipeline, GST_STATE_PLAYING);
	if (GST_STATE_CHANGE_FAILURE == err) {
		g_warning("set state returned GST_STATE_CHANGE_FAILUER\n");
	}

	/* Launch thread with gstreamer loop. */
	st->run = true;
	err = pthread_create(&st->tid, NULL, internal_thread, st);
	if (err) {
		st->run = false;
		goto out;
	}

	st->gst_inited = true;

 out:
	return err;
}


static int gst_v4l2_sink_push(struct vidsink_state *st, const uint8_t *src,
			  size_t size)
{
	GstBuffer *buffer;
	int ret = 0;

	if (!st) {
		return EINVAL;
	}

	if (!size) {
		warning("gst_v4l2_sink: push: eos returned %d at %d\n",
			ret, __LINE__);
		gst_app_src_end_of_stream((GstAppSrc *)st->source);
		return ret;
	}

	/* Wait "start feed". */
	pthread_mutex_lock(&st->mutex);
	if (st->bwait) {
#define WAIT_TIME_SECONDS 5
		struct timespec ts;
		struct timeval tp;
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec;
		ts.tv_nsec = tp.tv_usec * 1000;
		ts.tv_sec += WAIT_TIME_SECONDS;
		/* Wait. */
		ret = pthread_cond_timedwait(&st->wait, &st->mutex, &ts);
		if (ETIMEDOUT == ret) {
			warning("gst_v4l2_sink: Raw frame is lost"
				" because of timeout\n");
			return ret;
		}
	}
	pthread_mutex_unlock(&st->mutex);

	/* Create a new empty buffer */
	buffer = gst_buffer_new();
	GST_BUFFER_MALLOCDATA(buffer) = (guint8 *)src;
	GST_BUFFER_SIZE(buffer) = (guint)size;
	GST_BUFFER_DATA(buffer) = GST_BUFFER_MALLOCDATA(buffer);

	ret = gst_app_src_push_buffer((GstAppSrc *)st->source, buffer);

	if (ret != GST_FLOW_OK) {
		warning("gst_v4l2_sink: push buffer returned"
			" %d for %d bytes \n", ret, size);
		return ret;
	}

	return ret;
}


static void gst_v4l2_sink_close(struct vidsink_state *st)
{
	if (!st)
		return;

	st->gst_inited = false;

	/* Remove asynchronous callbacks to prevent using gst_v4l2_sink_t
	   context ("st") after releasing. */
	if (st->source) {
		g_signal_handler_disconnect(st->source,
					    st->need_data_handler);
		g_signal_handler_disconnect(st->source,
					    st->enough_data_handler);
	}

	/* Stop thread. */
	if (st->run) {
		st->run = false;
		pthread_join(st->tid, NULL);
	}

	if (st->source) {
		gst_object_unref(GST_OBJECT(st->source));
		st->source = NULL;
	}
	if (st->bus) {
		gst_object_unref(GST_OBJECT(st->bus));
		st->bus = NULL;
	}

	if (st->pipeline) {
		gst_element_set_state(st->pipeline, GST_STATE_NULL);
		gst_object_unref(GST_OBJECT(st->pipeline));
		st->pipeline = NULL;
	}
}


static void destruct_resources(void *arg)
{
	struct vidsink_state *st = arg;

	gst_v4l2_sink_close(st);

	/* destroy lock */
	pthread_mutex_destroy(&st->mutex);
	pthread_cond_destroy(&st->wait);
}

int gst_v4l2_sink_alloc(struct vidsink_state **stp, const char* dev)
{
	struct vidsink_state *st;

	st = mem_zalloc(sizeof(*st), destruct_resources);
	if (!st)
		return ENOMEM;

	*stp = st;

    st->dev = dev;

	/* Synchronization primitives. */
	pthread_mutex_init(&st->mutex, NULL);
	pthread_cond_init(&st->wait, NULL);

	return 0;
}

int gst_v4l2_sink_display(struct vidsink_state *st, const struct vidframe *frame)
{
	uint8_t *data;
	size_t size;
	int height;
	int err;

	if (!st || !frame || frame->fmt != VID_FMT_YUV420P)
		return EINVAL;

	if (!st->gst_inited || !vidsz_cmp(&st->size, &frame->size)) {

		err = gst_v4l2_sink_init(st, frame->size.w, frame->size.h);

		if (err) {
			warning("gst_v4l2_sink codec: gst_v4l2_sink_alloc failed\n");
			return err;
		}

		/* To detect if requested size was changed. */
		st->size = frame->size;
	}

	height = frame->size.h;

	/* NOTE: I420 (YUV420P): hardcoded. */
	size = frame->linesize[0] * height
		+ frame->linesize[1] * height * 0.5
		+ frame->linesize[2] * height * 0.5;

	data = malloc(size);    /* XXX: memory-leak ? */
	if (!data)
		return ENOMEM;

	size = 0;

	/* XXX: avoid memcpy here ? */
	memcpy(&data[size], frame->data[0], frame->linesize[0] * height);
	size += frame->linesize[0] * height;
	memcpy(&data[size], frame->data[1], frame->linesize[1] * height * 0.5);
	size += frame->linesize[1] * height * 0.5;
	memcpy(&data[size], frame->data[2], frame->linesize[2] * height * 0.5);
	size += frame->linesize[2] * height * 0.5;

	return gst_v4l2_sink_push(st, data, size);
}
