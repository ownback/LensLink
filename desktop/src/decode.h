/* Lean libavcodec decoder for the desktop daemon: software H.264/HEVC,
 * low-latency settings, one decoded frame per callback. The plugin's
 * h264-decoder.c adds hardware fall-back and OBS frame plumbing this
 * doesn't need. */

#pragma once

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ddecode {
	AVCodecContext *ctx;
	AVPacket *pkt;
	AVFrame *frame;
};

typedef void (*ddecode_frame_fn)(void *ud, const AVFrame *frame);

bool ddecode_open(struct ddecode *d, enum AVCodecID id);
void ddecode_close(struct ddecode *d);

/* Feeds one complete access unit and emits every frame it yields.
 * False on a decode error — the caller should drop the decoder. */
bool ddecode_run(struct ddecode *d, const uint8_t *data, size_t len,
		 uint64_t pts_ns, ddecode_frame_fn cb, void *ud);
