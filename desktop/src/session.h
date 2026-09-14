/* Phone connection: dial loop (Wi-Fi/USB), protocol packet handling, and
 * timesync — distilled from the OBS plugin's ios-camera-source.c into an
 * OBS-free session that feeds callbacks. */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

struct session_cfg {
	bool enabled;      /* dial and feed the virtual devices */
	bool remote_start; /* start the phone's camera on standby */
	bool mic;          /* ask the phone to stream its mic */
	bool usb;
	char usb_udid[64];
	char host[64];
};

struct session_stats {
	bool connected;
	bool standby;
	bool streaming; /* decoded frames flowing */
	char name[128];
	char codec[8];
	unsigned width, height, fps_conf;
	bool mic_on; /* app-reported mic send state */
	uint64_t frames, decode_errors, video_packets, audio_bytes;
	unsigned fps_now;
	unsigned kbps_now;
	int latency_ms;
	char transport[8]; /* "wifi" or "usb" */
};

struct session_cbs {
	void *ud;
	/* Session thread context — copy or write out synchronously. */
	void (*video_frame)(void *ud, const AVFrame *frame);
	void (*audio_pcm)(void *ud, const int16_t *pcm, size_t frames);
	/* up: TCP established; down: connection ended. */
	void (*link)(void *ud, bool up);
};

struct session;

struct session *session_start(const struct session_cfg *cfg,
			      const struct session_cbs *cbs);
void session_stop(struct session *s);

/* Thread-safe config update; connection-affecting changes redial. */
void session_apply(struct session *s, const struct session_cfg *cfg);

/* Queue a CONTROL command (e.g. {"cmd":"start_stream"}). Thread-safe. */
void session_control(struct session *s, const char *json);

void session_stats_copy(struct session *s, struct session_stats *out);
