#include "daemon.h"

#include "config.h"
#include "discover.h"
#include "ipc.h"
#include "json.h"
#include "log.h"
#include "session.h"
#include "vcam.h"
#include "vmic.h"

#include <usbmux.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_bool g_quit = false;

void daemon_request_quit(void)
{
	atomic_store(&g_quit, true);
}

struct daemon {
	struct ll_config cfg;
	pthread_mutex_t lock;
	struct session *session;
	struct vcam vcam;
	struct vmic vmic;
	bool vcam_failed_logged;
	bool vmic_error_logged;
	char status[192];
};

static void on_video_frame(void *ud, const AVFrame *frame)
{
	struct daemon *d = ud;
	if (d->vcam.fd < 0) {
		char dev[64];
		pthread_mutex_lock(&d->lock);
		snprintf(dev, sizeof(dev), "%s", d->cfg.device);
		pthread_mutex_unlock(&d->lock);
		if (!vcam_open(&d->vcam, dev[0] ? dev : NULL,
			       (unsigned)frame->width,
			       (unsigned)frame->height))
			return;
	}
	if (!vcam_write(&d->vcam, frame)) {
		if (!d->vcam_failed_logged) {
			LOGW("virtual camera write failed — is the "
			     "v4l2loopback module loaded?");
			d->vcam_failed_logged = true;
		}
	}
}

static void on_audio_pcm(void *ud, const int16_t *pcm, size_t frames)
{
	struct daemon *d = ud;
	if (!d->vmic.ring) {
		LOGI("first audio chunk (%zu frames) — starting virtual "
		     "microphone",
		     frames);
		if (d->vmic_error_logged)
			return;
		if (!vmic_start(&d->vmic)) {
			LOGW("virtual microphone failed to start — will "
			     "retry on the next connection");
			d->vmic_error_logged = true;
			return;
		}
	}
	vmic_push(&d->vmic, pcm, frames);
}

static void on_link(void *ud, bool up)
{
	struct daemon *d = ud;
	if (up)
		return;
	vcam_close(&d->vcam);
	if (d->vmic.ring || d->vmic.loop)
		vmic_stop(&d->vmic);
	d->vcam_failed_logged = false;
	d->vmic_error_logged = false;
	LOGI("virtual devices released");
}

static void push_config(struct daemon *d)
{
	struct session_cfg sc;
	pthread_mutex_lock(&d->lock);
	sc.enabled = d->cfg.enabled;
	sc.remote_start = d->cfg.remote_start;
	sc.mic = d->cfg.mic;
	sc.usb = d->cfg.usb;
	snprintf(sc.usb_udid, sizeof(sc.usb_udid), "%s", d->cfg.usb_udid);
	snprintf(sc.host, sizeof(sc.host), "%s", d->cfg.host);
	pthread_mutex_unlock(&d->lock);
	if (d->session)
		session_apply(d->session, &sc);
}

/* --- IPC surface ------------------------------------------------------- */

static void list_phones(char *resp, size_t size)
{
	struct ll_phone wifi[8];
	int n_wifi = ll_discover_phones(wifi, 8);
	struct usbmux_device usb[8];
	int n_usb = usbmux_list_devices(usb, 8);

	size_t off = (size_t)snprintf(resp, size,
				      "{\"type\":\"phones\",\"wifi\":[");
	for (int i = 0; i < n_wifi && off < size - 64; i++) {
		char esc[160], esc_host[128];
		json_escape_to(esc, sizeof(esc), wifi[i].name);
		json_escape_to(esc_host, sizeof(esc_host), wifi[i].host);
		off += (size_t)snprintf(resp + off, size - off,
					"%s{\"name\":%s,\"host\":%s}",
					i ? "," : "", esc, esc_host);
	}
	off += (size_t)snprintf(resp + off, size - off, "],\"usb\":[");
	for (int i = 0; i < n_usb && off < size - 64; i++) {
		char esc[160];
		json_escape_to(esc, sizeof(esc), usb[i].udid);
		off += (size_t)snprintf(resp + off, size - off,
					"%s{\"udid\":%s}", i ? "," : "",
					esc);
	}
	snprintf(resp + off, size - off, "]}");
}

static void handle_request(void *ud, const char *req, char *resp,
			   size_t size)
{
	struct daemon *d = ud;

	char cmd[32] = {0};
	json_get_string(req, "cmd", cmd, sizeof(cmd));

	if (strcmp(cmd, "status") == 0) {
		struct session_stats st = {0};
		if (d->session)
			session_stats_copy(d->session, &st);

		char name_escaped[128];
		bool enabled, mic, usb;
		char host[64], udid[64], device[64];
		pthread_mutex_lock(&d->lock);
		json_escape_to(name_escaped, sizeof(name_escaped),
			       st.name[0] ? st.name : "");
		enabled = d->cfg.enabled;
		mic = d->cfg.mic;
		usb = d->cfg.usb;
		snprintf(host, sizeof(host), "%s", d->cfg.host);
		snprintf(udid, sizeof(udid), "%s", d->cfg.usb_udid);
		snprintf(device, sizeof(device), "%s", d->cfg.device);
		pthread_mutex_unlock(&d->lock);
		snprintf(resp, size,
			 "{\"type\":\"status\",\"enabled\":%s,"
			 "\"connected\":%s,\"standby\":%s,"
			 "\"streaming\":%s,\"mic_on\":%s,\"mic\":%s,"
			 "\"name\":%s,\"width\":%u,\"height\":%u,"
			 "\"fps\":%u,\"kbps\":%u,\"latency\":%d,"
			 "\"codec\":\"%.7s\",\"usb\":%s,"
			 "\"host\":\"%s\",\"udid\":\"%s\",\"device\":\"%s\"}",
			 enabled ? "true" : "false",
			 st.connected ? "true" : "false",
			 st.standby ? "true" : "false",
			 st.streaming ? "true" : "false",
			 st.mic_on ? "true" : "false",
			 mic ? "true" : "false", name_escaped, st.width,
			 st.height, st.fps_now, st.kbps_now, st.latency_ms,
			 st.codec, usb ? "true" : "false", host, udid,
			 device);
		return;
	}

	if (strcmp(cmd, "enabled") == 0) {
		bool on = json_get_bool(req, "on");
		pthread_mutex_lock(&d->lock);
		d->cfg.enabled = on;
		pthread_mutex_unlock(&d->lock);
		push_config(d);
		ll_config_save(&d->cfg);
		snprintf(resp, size, "{\"type\":\"ok\"}");
		return;
	}

	if (strcmp(cmd, "mic") == 0) {
		bool on = json_get_bool(req, "on");
		pthread_mutex_lock(&d->lock);
		d->cfg.mic = on;
		pthread_mutex_unlock(&d->lock);
		push_config(d);
		ll_config_save(&d->cfg);
		snprintf(resp, size, "{\"type\":\"ok\"}");
		return;
	}

	if (strcmp(cmd, "camera") == 0) {
		/* Manual start/stop of the phone's camera; a manual stop
		 * sticks because remote start only fires from a fresh
		 * connection (see session.c). */
		bool on = json_get_bool(req, "on");
		if (d->session)
			session_control(d->session, on
						       ? "{\"cmd\":\"start_stream\"}"
						       : "{\"cmd\":\"stop_stream\"}");
		snprintf(resp, size, "{\"type\":\"ok\"}");
		return;
	}

	if (strcmp(cmd, "use") == 0) {
		char host[64] = {0}, udid[64] = {0};
		json_get_string(req, "host", host, sizeof(host));
		json_get_string(req, "udid", udid, sizeof(udid));
		bool usb = json_get_bool(req, "usb");
		pthread_mutex_lock(&d->lock);
		snprintf(d->cfg.host, sizeof(d->cfg.host), "%s", host);
		snprintf(d->cfg.usb_udid, sizeof(d->cfg.usb_udid), "%s",
			 udid);
		d->cfg.usb = usb;
		pthread_mutex_unlock(&d->lock);
		push_config(d);
		ll_config_save(&d->cfg);
		snprintf(resp, size, "{\"type\":\"ok\"}");
		return;
	}

	if (strcmp(cmd, "phones") == 0) {
		list_phones(resp, size);
		return;
	}

	if (strcmp(cmd, "quit") == 0) {
		snprintf(resp, size, "{\"type\":\"ok\"}");
		daemon_request_quit();
		return;
	}

	snprintf(resp, size, "{\"type\":\"error\",\"error\":\"unknown cmd\"}");
}

/* --- main loop --------------------------------------------------------- */

int daemon_run(const struct ll_config *initial)
{
	struct daemon d;
	memset(&d, 0, sizeof(d));
	d.cfg = *initial;
	pthread_mutex_init(&d.lock, NULL);
	d.vcam.fd = -1;

	signal(SIGPIPE, SIG_IGN);

	struct session_cbs cbs = {
		.ud = &d,
		.video_frame = on_video_frame,
		.audio_pcm = on_audio_pcm,
		.link = on_link,
	};

	struct session_cfg sc = {
		.enabled = d.cfg.enabled,
		.remote_start = d.cfg.remote_start,
		.mic = d.cfg.mic,
		.usb = d.cfg.usb,
	};
	snprintf(sc.usb_udid, sizeof(sc.usb_udid), "%s", d.cfg.usb_udid);
	snprintf(sc.host, sizeof(sc.host), "%s", d.cfg.host);
	d.session = session_start(&sc, &cbs);
	if (!d.session) {
		LOGE("failed to start session");
		return 1;
	}

	ipc_server_start(handle_request, &d);
	LOGI("lenslinkd running — config: %s", ll_config_path());

	while (!atomic_load(&g_quit))
		sleep(1);

	ipc_server_stop();
	session_stop(d.session);
	vcam_close(&d.vcam);
	if (d.vmic.ring || d.vmic.loop)
		vmic_stop(&d.vmic);
	pthread_mutex_destroy(&d.lock);
	LOGI("lenslinkd stopped");
	return 0;
}
