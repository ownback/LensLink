#include "vmic.h"

#include "log.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * One producer (the session thread pushing wire audio) and one consumer
 * (PipeWire's RT process callback) meet in a mutex-guarded SPSC byte
 * ring. The producer drops the oldest audio when the ring fills — clock
 * drift between phone and host is ppm-level, so that fires roughly
 * never; when the ring runs dry the consumer pads silence instead of
 * stalling.
 */

#define RING_BYTES (192000 * 2) /* 2 s of 48 kHz stereo S16 */

struct spsc_ring {
	uint8_t *buf;
	size_t cap;
	size_t rd, wr, used;
};

static void ring_init(struct spsc_ring *r)
{
	r->buf = malloc(RING_BYTES);
	r->cap = RING_BYTES;
	r->rd = r->wr = r->used = 0;
}

static void ring_push(struct spsc_ring *r, const uint8_t *data, size_t n)
{
	if (n >= r->cap) {
		memcpy(r->buf, data + n - r->cap, r->cap);
		r->wr = 0;
		r->rd = 0;
		r->used = r->cap;
		return;
	}
	if (r->used + n > r->cap) {
		r->rd = (r->rd + (r->used + n - r->cap)) % r->cap;
		r->used = r->cap - n;
	}
	size_t tail = r->cap - r->wr;
	if (n <= tail) {
		memcpy(r->buf + r->wr, data, n);
	} else {
		memcpy(r->buf + r->wr, data, tail);
		memcpy(r->buf, data + tail, n - tail);
	}
	r->wr = (r->wr + n) % r->cap;
	r->used += n;
}

static size_t ring_pop(struct spsc_ring *r, uint8_t *out, size_t n)
{
	if (n > r->used)
		n = r->used;
	size_t head = r->cap - r->rd;
	if (n <= head) {
		memcpy(out, r->buf + r->rd, n);
	} else {
		memcpy(out, r->buf + r->rd, head);
		memcpy(out + head, r->buf, n - head);
	}
	r->rd = (r->rd + n) % r->cap;
	r->used -= n;
	return n;
}

static void on_process(void *ud)
{
	struct vmic *v = ud;
	struct spsc_ring *r = v->ring;

	struct pw_buffer *b = pw_stream_dequeue_buffer(v->stream);
	if (!b)
		return;
	struct spa_buffer *buf = b->buffer;
	int16_t *dst = buf->datas[0].data;
	if (dst) {
		size_t want = buf->datas[0].maxsize / 4; /* bytes→stereo s16 frames */
		if (b->requested > 0 && b->requested < want)
			want = b->requested;
		size_t got;
		pthread_mutex_lock(&v->lock);
		got = ring_pop(r, (uint8_t *)dst, want * 4);
		pthread_mutex_unlock(&v->lock);
		memset((uint8_t *)dst + got, 0, want * 4 - got);
		buf->datas[0].chunk->offset = 0;
		buf->datas[0].chunk->stride = 4;
		buf->datas[0].chunk->size = want * 4;
	}
	pw_stream_queue_buffer(v->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

bool vmic_start(struct vmic *v)
{
	memset(v, 0, sizeof(*v));
	pthread_mutex_init(&v->lock, NULL);

	v->ring = malloc(sizeof(struct spsc_ring));
	if (!v->ring)
		return false;
	ring_init(v->ring);

	pw_init(NULL, NULL);

	v->loop = pw_thread_loop_new("lenslink-vmic", NULL);
	if (!v->loop) {
		LOGE("vmic: thread loop failed");
		return false;
	}

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_MEDIA_ROLE,
		"Communication", PW_KEY_NODE_NAME, "lenslink_virtual_mic",
		PW_KEY_NODE_DESCRIPTION, "LensLink Virtual Microphone",
		PW_KEY_NODE_WANT_DRIVER, "true", NULL);
	/* pw_stream_new takes ownership of `props` — do not free. */
	v->stream = pw_stream_new_simple(pw_thread_loop_get_loop(v->loop),
					 "lenslink-vmic", props,
					 &stream_events, v);
	if (!v->stream) {
		LOGE("vmic: stream creation failed");
		return false;
	}

	uint8_t pod_buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
	struct spa_audio_info_raw info = {
		.format = SPA_AUDIO_FORMAT_S16,
		.rate = 48000,
		.channels = 2,
		.position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR},
	};
	const struct spa_pod *params[1] = {
		spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info),
	};

	if (pw_thread_loop_start(v->loop) < 0) {
		LOGE("vmic: loop start failed");
		return false;
	}

	pw_thread_loop_lock(v->loop);
	int ret = pw_stream_connect(v->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				    PW_STREAM_FLAG_RT_PROCESS |
					    PW_STREAM_FLAG_MAP_BUFFERS,
				    params, 1);
	pw_thread_loop_unlock(v->loop);
	if (ret < 0) {
		LOGE("vmic: stream connect failed");
		return false;
	}

	LOGI("virtual microphone ready: 48 kHz stereo");
	return true;
}

void vmic_stop(struct vmic *v)
{
	if (v->loop)
		pw_thread_loop_stop(v->loop);
	if (v->stream) {
		struct pw_stream *s = v->stream;
		v->stream = NULL;
		pw_stream_destroy(s);
	}
	if (v->loop)
		pw_thread_loop_destroy(v->loop);
	free(v->ring);
	pthread_mutex_destroy(&v->lock);
	memset(v, 0, sizeof(*v));
}

void vmic_push(struct vmic *v, const int16_t *pcm, size_t frames)
{
	if (!v->stream)
		return;
	pthread_mutex_lock(&v->lock);
	ring_push(v->ring, (const uint8_t *)pcm, frames * 4);
	pthread_mutex_unlock(&v->lock);
}
