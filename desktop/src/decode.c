#include "decode.h"

#include "log.h"

#include <string.h>

bool ddecode_open(struct ddecode *d, enum AVCodecID id)
{
	memset(d, 0, sizeof(*d));

	const AVCodec *codec = avcodec_find_decoder(id);
	if (!codec) {
		LOGE("decoder for codec %d unavailable", (int)id);
		return false;
	}

	d->ctx = avcodec_alloc_context3(codec);
	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	if (!d->ctx || !d->pkt || !d->frame)
		goto fail;

	d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	/* Same reasoning as the plugin: the wire delivers complete access
	 * units, and frame threading would only buffer latency. */
	d->ctx->thread_count = 1;

	if (avcodec_open2(d->ctx, codec, NULL) < 0) {
		LOGE("failed to open decoder");
		goto fail;
	}
	return true;

fail:
	ddecode_close(d);
	return false;
}

void ddecode_close(struct ddecode *d)
{
	if (d->ctx)
		avcodec_free_context(&d->ctx);
	if (d->pkt)
		av_packet_free(&d->pkt);
	if (d->frame)
		av_frame_free(&d->frame);
	memset(d, 0, sizeof(*d));
}

bool ddecode_run(struct ddecode *d, const uint8_t *data, size_t len,
		 uint64_t pts_ns, ddecode_frame_fn cb, void *ud)
{
	d->pkt->data = (uint8_t *)(uintptr_t)data;
	d->pkt->size = (int)len;
	d->pkt->pts = (int64_t)pts_ns;
	d->pkt->dts = (int64_t)pts_ns;

	int ret = avcodec_send_packet(d->ctx, d->pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		LOGW("avcodec_send_packet: %d", ret);
		return false;
	}

	while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
		if (cb)
			cb(ud, d->frame);
		av_frame_unref(d->frame);
	}
	return true;
}
