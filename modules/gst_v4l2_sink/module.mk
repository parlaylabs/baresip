#
# module.mk
#
# Copyright (C) 2017 Highfive, Inc.
#

MOD		:= gst_v4l2_sink
$(MOD)_SRCS	+= gst_v4l2_sink.c display.c
$(MOD)_LFLAGS	+= $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
$(MOD)_CFLAGS   += $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
$(MOD)_CFLAGS	+= -Wno-cast-align

include mk/mod.mk
