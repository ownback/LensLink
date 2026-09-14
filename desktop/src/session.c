#include "session.h"

#include "decode.h"
#include "discover.h"
#include "json.h"
#include "log.h"
#include "net-compat.h"
#include "protocol.h"

#include <usbmux.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RECV_CHUNK (64 * 1024)
#define SEND_BUF_MAX (256 * 1024)
#define CTL_QUEUE_MAX 16

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms)
{
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

struct latency_tracker {
	bool have_offset;
	int64_t offset_ns;
	uint64_t offset_rtt;
	uint64_t offset_time;

	uint64_t sum_ns, count;
	uint64_t last_sync_send;
};

static void latency_on_timesync(struct latency_tracker *t, uint64_t t1,
				uint64_t t2, uint64_t t3)
{
	if (t3 <= t1)
		return;
	uint64_t rtt = t3 - t1;
	int64_t offset = (int64_t)t2 - (int64_t)((t1 + t3) / 2);
	bool stale = t3 - t->offset_time > 10000000000ULL;
	if (!t->have_offset || rtt <= t->offset_rtt || stale) {
		t->have_offset = true;
		t->offset_ns = offset;
		t->offset_rtt = rtt;
		t->offset_time = t3;
	}
}

static void latency_on_frame(struct latency_tracker *t, uint64_t pts_phone_ns,
			     uint64_t now)
{
	if (!t->have_offset)
		return;
	int64_t captured_local = (int64_t)pts_phone_ns - t->offset_ns;
	int64_t lat = (int64_t)now - captured_local;
	if (lat < 0)
		lat = 0;
	t->sum_ns += (uint64_t)lat;
	t->count++;
}

struct client {
	socket_t sock;
	struct session *owner;
	uint8_t *rbuf;
	size_t rlen, rcap;
	uint8_t *sbuf;
	size_t slen, scap;
	bool sfailed;

	struct ddecode dec;
	bool dec_open;
	enum AVCodecID codec;

	bool standby;
	bool mic_sent;
	bool armed; /* remote start may fire on the next standby HELLO */
	char name[128];
	struct latency_tracker lat;
	uint64_t connected_ns;

	uint64_t video_packets, video_bytes, keyframes, frames, decode_errors;
	uint64_t audio_bytes;
	uint64_t first_frame_ns;
	uint64_t next_decoder_attempt;

	unsigned fps_now, kbps_now;
	uint64_t win_start, win_frames, win_bytes;
	int latency_ms;
};

struct session {
	pthread_mutex_t lock;
	pthread_t thread;
	bool thread_active;
	volatile bool stop;

	struct session_cfg cfg;
	uint64_t gen;

	char *ctl[CTL_QUEUE_MAX];
	int ctl_count;

	struct session_stats stats;
	struct session_cbs cbs;
};

/* --- outbound framing ------------------------------------------------ */

static void client_flush(struct client *c)
{
	while (c->slen > 0 && !c->sfailed) {
		int n = (int)send(c->sock, (const char *)c->sbuf, (int)c->slen,
				  0);
		if (n > 0) {
			memmove(c->sbuf, c->sbuf + n, c->slen - (size_t)n);
			c->slen -= (size_t)n;
		} else if (n < 0 && net_would_block()) {
			return;
		} else {
			c->sfailed = true;
		}
	}
}

static void client_send(struct client *c, const void *data, size_t len)
{
	if (c->sfailed)
		return;
	const uint8_t *p = data;
	if (c->slen == 0) {
		while (len > 0) {
			int n = (int)send(c->sock, (const char *)p, (int)len,
					  0);
			if (n > 0) {
				p += n;
				len -= (size_t)n;
			} else if (n < 0 && net_would_block()) {
				break;
			} else {
				c->sfailed = true;
				return;
			}
		}
		if (len == 0)
			return;
	}
	if (c->slen + len > SEND_BUF_MAX) {
		LOGW("peer not draining control channel, dropping connection");
		c->sfailed = true;
		return;
	}
	if (c->slen + len > c->scap) {
		size_t cap = c->scap ? c->scap : 4096;
		while (cap < c->slen + len)
			cap *= 2;
		uint8_t *grown = realloc(c->sbuf, cap);
		if (!grown) {
			c->sfailed = true;
			return;
		}
		c->sbuf = grown;
		c->scap = cap;
	}
	memcpy(c->sbuf + c->slen, p, len);
	c->slen += len;
}

static void send_control(struct client *c, const char *json)
{
	size_t len = strlen(json);
	uint8_t hdr[OBSC_HEADER_SIZE];
	obsc_build_header(hdr, OBSC_PKT_CONTROL, 0, 0, (uint32_t)len);
	client_send(c, hdr, OBSC_HEADER_SIZE);
	client_send(c, json, len);
	LOGI("control: %s", json);
}

/* --- dial ------------------------------------------------------------ */

static socket_t tcp_dial(const char *host, uint16_t port)
{
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
		struct addrinfo hints = {0};
		struct addrinfo *res = NULL;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
			return OBSC_INVALID_SOCKET;
		memcpy(&addr, res->ai_addr, sizeof(addr));
		addr.sin_port = htons(port);
		freeaddrinfo(res);
	}

	socket_t s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == OBSC_INVALID_SOCKET)
		return s;

	net_set_nonblocking(s);
	int ret = connect(s, (struct sockaddr *)&addr, sizeof(addr));
	if (ret != 0 && errno != EINPROGRESS) {
		net_close(s);
		return OBSC_INVALID_SOCKET;
	}
	while (ret != 0) {
		int r = net_wait(s, NET_WAIT_WRITE, 100);
		if (r < 0) {
			net_close(s);
			return OBSC_INVALID_SOCKET;
		}
		if (r & NET_WAIT_WRITE)
			break;
	}
	int err = 0;
	socklen_t len = sizeof(err);
	getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
	if (err != 0) {
		net_close(s);
		return OBSC_INVALID_SOCKET;
	}
	int yes = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes,
		   sizeof(yes));
	return s;
}

/* --- inbound ---------------------------------------------------------- */

static void close_decoder(struct client *c)
{
	if (c->dec_open) {
		ddecode_close(&c->dec);
		c->dec_open = false;
	}
}

static void on_decoded_frame(struct session *s, struct client *c,
			     const AVFrame *f)
{
	c->frames++;
	c->win_frames++;
	if (c->first_frame_ns == 0) {
		c->first_frame_ns = now_ns();
		LOGI("first decoded frame %dx%d after %llu ms", f->width,
		     f->height,
		     (unsigned long long)((c->first_frame_ns -
					   c->connected_ns) /
					  1000000ULL));
	}
	if (s->cbs.video_frame)
		s->cbs.video_frame(s->cbs.ud, f);
}

static void frame_trampoline(void *ud, const AVFrame *f)
{
	struct client *c = ud;
	on_decoded_frame(c->owner, c, f);
}

static bool handle_packet(struct session *s, struct client *c,
			  const struct obsc_header *hdr,
			  const uint8_t *payload)
{
	switch (hdr->type) {
	case OBSC_PKT_HELLO: {
		char json[512] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);
		json_get_string(json, "name", c->name, sizeof(c->name));
		char kind[16] = {0};
		json_get_string(json, "kind", kind, sizeof(kind));
		c->standby = strcmp(kind, "screen") != 0 &&
			     json_get_bool(json, "standby");
		c->connected_ns = now_ns();

		pthread_mutex_lock(&s->lock);
		snprintf(s->stats.name, sizeof(s->stats.name), "%s", c->name);
		s->stats.connected = true;
		s->stats.standby = c->standby;
		pthread_mutex_unlock(&s->lock);

		LOGI("phone connected: %s (%s%s)",
		     c->name[0] ? c->name : "(unnamed)",
		     kind[0] ? kind : "camera", c->standby ? ", standby" : "");
		if (s->cbs.link)
			s->cbs.link(s->cbs.ud, true);

		if (c->standby) {
			pthread_mutex_lock(&s->lock);
			bool want = s->cfg.remote_start;
			pthread_mutex_unlock(&s->lock);
			if (want && c->armed) {
				c->armed = false;
				LOGI("app idle — starting camera remotely");
				send_control(c, "{\"cmd\":\"start_stream\"}");
			}
		}
		break;
	}
	case OBSC_PKT_VIDEO_CONFIG: {
		char json[512] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);

		char codec[32] = {0};
		json_get_string(json, "codec", codec, sizeof(codec));
		long w = 0, h = 0, fps = 0;
		json_get_int(json, "width", &w);
		json_get_int(json, "height", &h);
		json_get_int(json, "fps", &fps);

		enum AVCodecID id = strcmp(codec, "hevc") == 0
					    ? AV_CODEC_ID_HEVC
					    : AV_CODEC_ID_H264;
		if (id != c->codec) {
			close_decoder(c);
			c->codec = id;
		}
		if (c->standby) {
			c->standby = false;
			pthread_mutex_lock(&s->lock);
			s->stats.standby = false;
			pthread_mutex_unlock(&s->lock);
		}

		pthread_mutex_lock(&s->lock);
		snprintf(s->stats.codec, sizeof(s->stats.codec), "%.7s",
			 codec);
		s->stats.width = (unsigned)w;
		s->stats.height = (unsigned)h;
		s->stats.fps_conf = (unsigned)fps;
		pthread_mutex_unlock(&s->lock);
		LOGI("video config: %s %ldx%ld@%ld", codec, w, h, fps);
		break;
	}
	case OBSC_PKT_VIDEO: {
		bool keyframe = (hdr->flags & OBSC_FLAG_KEYFRAME) != 0;
		c->video_packets++;
		c->video_bytes += hdr->payload_size;
		c->win_bytes += hdr->payload_size;
		if (keyframe)
			c->keyframes++;

		if (!c->dec_open) {
			if (!keyframe)
				break;
			uint64_t now = now_ns();
			if (now < c->next_decoder_attempt)
				break;
			if (!ddecode_open(&c->dec, c->codec != AV_CODEC_ID_NONE
							? c->codec
							: AV_CODEC_ID_H264)) {
				c->next_decoder_attempt = now + 5000000000ULL;
				break;
			}
			c->dec_open = true;
			c->next_decoder_attempt = 0;
		}
		if (!ddecode_run(&c->dec, payload, hdr->payload_size,
				 hdr->pts_ns, frame_trampoline, c)) {
			c->decode_errors++;
			LOGW("decoder error, resetting");
			close_decoder(c);
			break;
		}
		latency_on_frame(&c->lat, hdr->pts_ns, now_ns());
		break;
	}
	case OBSC_PKT_SCREEN_AUDIO:
		c->audio_bytes += hdr->payload_size;
		if (s->cbs.audio_pcm && hdr->payload_size >= 4)
			s->cbs.audio_pcm(s->cbs.ud, (const int16_t *)payload,
					 hdr->payload_size / 4);
		break;
	case OBSC_PKT_STATE: {
		char json[1024] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);
		pthread_mutex_lock(&s->lock);
		s->stats.mic_on = json_get_bool(json, "micEnabled");
		pthread_mutex_unlock(&s->lock);
		break;
	}
	case OBSC_PKT_TIMESYNC_RESP:
		if (hdr->payload_size >= 8)
			latency_on_timesync(&c->lat, obsc_read_u64(payload),
					    hdr->pts_ns, now_ns());
		break;
	case OBSC_PKT_AUDIO:
	case OBSC_PKT_PING:
	case OBSC_PKT_DIAG:
	case OBSC_PKT_REQUEST:
	default:
		break;
	}
	return true;
}

static bool client_read(struct session *s, struct client *c)
{
	if (c->rcap - c->rlen < RECV_CHUNK) {
		size_t cap = c->rcap ? c->rcap : RECV_CHUNK;
		while (cap < c->rlen + RECV_CHUNK)
			cap *= 2;
		uint8_t *grown = realloc(c->rbuf, cap);
		if (!grown)
			return false;
		c->rbuf = grown;
		c->rcap = cap;
	}
	int n = (int)recv(c->sock, (char *)c->rbuf + c->rlen,
			  (int)(c->rcap - c->rlen), 0);
	if (n == 0)
		return false;
	if (n < 0) {
		if (net_would_block())
			return true;
		return false;
	}
	c->rlen += (size_t)n;

	size_t off = 0;
	bool ok = true;
	while (c->rlen - off >= OBSC_HEADER_SIZE) {
		struct obsc_header hdr;
		if (!obsc_parse_header(c->rbuf + off, &hdr)) {
			LOGW("bad packet header, dropping connection");
			ok = false;
			break;
		}
		size_t total = OBSC_HEADER_SIZE + hdr.payload_size;
		if (c->rlen - off < total)
			break;
		if (!handle_packet(s, c, &hdr,
				   c->rbuf + off + OBSC_HEADER_SIZE)) {
			ok = false;
			break;
		}
		off += total;
	}

	if (off >= c->rlen) {
		c->rlen = 0;
	} else if (off > 0) {
		memmove(c->rbuf, c->rbuf + off, c->rlen - off);
		c->rlen -= off;
	}
	if (c->rcap > 2 * RECV_CHUNK && c->rlen < RECV_CHUNK) {
		uint8_t *shrunk = realloc(c->rbuf, 2 * RECV_CHUNK);
		if (shrunk) {
			c->rbuf = shrunk;
			c->rcap = 2 * RECV_CHUNK;
		}
	}
	return ok;
}

/* --- 1 Hz bookkeeping -------------------------------------------------- */

static void drain_control_queue(struct session *s, struct client *c)
{
	char *pending[CTL_QUEUE_MAX];
	int count;
	pthread_mutex_lock(&s->lock);
	count = s->ctl_count;
	memcpy(pending, s->ctl, sizeof(char *) * (size_t)count);
	s->ctl_count = 0;
	pthread_mutex_unlock(&s->lock);
	for (int i = 0; i < count; i++) {
		send_control(c, pending[i]);
		free(pending[i]);
	}
}

static void apply_mic_setting(struct client *c, bool want)
{
	if (c->mic_sent == want)
		return;
	c->mic_sent = want;
	send_control(c, want ? "{\"cmd\":\"send_mic\",\"on\":true}"
			     : "{\"cmd\":\"send_mic\",\"on\":false}");
}

static void stats_tick(struct session *s, struct client *c)
{
	uint64_t now = now_ns();

	if (c->win_start == 0)
		c->win_start = now;
	uint64_t elapsed = now - c->win_start;
	if (elapsed >= 1000000000ULL) {
		c->fps_now = (unsigned)(c->win_frames * 1000000000ULL /
					elapsed);
		c->kbps_now =
			(unsigned)(c->win_bytes * 8 / 1000 /
				   (elapsed / 1000000000ULL));
		c->win_frames = 0;
		c->win_bytes = 0;
		c->win_start = now;
	}
	if (c->lat.count) {
		c->latency_ms =
			(int)(c->lat.sum_ns / c->lat.count / 1000000);
		c->lat.sum_ns = 0;
		c->lat.count = 0;
	}

	pthread_mutex_lock(&s->lock);
	struct session_stats *st = &s->stats;
	st->frames = c->frames;
	st->decode_errors = c->decode_errors;
	st->video_packets = c->video_packets;
	st->audio_bytes = c->audio_bytes;
	st->fps_now = c->fps_now;
	st->kbps_now = c->kbps_now;
	st->streaming = c->frames > 0 && c->fps_now > 0;
	st->latency_ms = c->latency_ms;
	pthread_mutex_unlock(&s->lock);
}

/* --- thread ------------------------------------------------------------ */

static bool same_connection(const struct session_cfg *a,
			    const struct session_cfg *b)
{
	return a->usb == b->usb && a->enabled == b->enabled &&
	       a->remote_start == b->remote_start &&
	       strcmp(a->host, b->host) == 0 &&
	       strcmp(a->usb_udid, b->usb_udid) == 0;
}

static void reset_stats(struct session *s)
{
	pthread_mutex_lock(&s->lock);
	s->stats.connected = false;
	s->stats.standby = false;
	s->stats.streaming = false;
	s->stats.frames = 0;
	s->stats.decode_errors = 0;
	s->stats.video_packets = 0;
	s->stats.audio_bytes = 0;
	s->stats.fps_now = 0;
	s->stats.kbps_now = 0;
	s->stats.latency_ms = 0;
	s->stats.mic_on = false;
	s->stats.name[0] = 0;
	s->stats.codec[0] = 0;
	s->stats.width = 0;
	s->stats.height = 0;
	s->stats.fps_conf = 0;
	pthread_mutex_unlock(&s->lock);
}

static void dial_loop(struct session *s)
{
	uint64_t seen_gen = s->gen;

	while (!s->stop) {
		struct session_cfg cfg;
		pthread_mutex_lock(&s->lock);
		cfg = s->cfg;
		uint64_t gen = s->gen;
		pthread_mutex_unlock(&s->lock);
		seen_gen = gen;

		if (!cfg.enabled) {
			reset_stats(s);
			sleep_ms(300);
			continue;
		}

		socket_t sock = OBSC_INVALID_SOCKET;
		if (cfg.usb) {
			struct usbmux_device devs[16];
			int n = usbmux_list_devices(devs, 16);
			int chosen = -1;
			for (int i = 0; i < n; i++) {
				if (cfg.usb_udid[0] &&
				    strcmp(cfg.usb_udid, devs[i].udid) != 0)
					continue;
				chosen = i;
				break;
			}
			if (chosen < 0) {
				sleep_ms(1000);
				continue;
			}
			sock = usbmux_connect_device(devs[chosen].id,
						     OBSC_USB_PORT);
		} else {
			char host[64];
			snprintf(host, sizeof(host), "%s", cfg.host);
			if (!host[0]) {
				struct ll_phone phones[8];
				int n = ll_discover_phones(phones, 8);
				if (n > 0)
					snprintf(host, sizeof(host), "%s",
						 phones[0].host);
			}
			if (!host[0]) {
				reset_stats(s);
				sleep_ms(1500);
				continue;
			}
			sock = tcp_dial(host, OBSC_USB_PORT);
		}

		if (sock == OBSC_INVALID_SOCKET) {
			reset_stats(s);
			sleep_ms(2000);
			continue;
		}
		LOGI("connected to phone (%s)", cfg.usb ? "USB" : "Wi-Fi");

		/* A fresh TCP connection is fresh reachability: remote start
		 * re-arms here. It disarms when it fires, so a user-stop
		 * followed by the app's standby HELLO on the same
		 * connection can't restart the camera (no ping-pong). */
		struct client c;
		memset(&c, 0, sizeof(c));
		c.sock = sock;
		c.owner = s;
		c.codec = AV_CODEC_ID_NONE;
		c.armed = true;

		while (!s->stop) {
			struct session_cfg tick_cfg;
			pthread_mutex_lock(&s->lock);
			tick_cfg = s->cfg;
			uint64_t gen = s->gen;
			pthread_mutex_unlock(&s->lock);

			if (gen != seen_gen) {
				seen_gen = gen;
				if (!same_connection(&tick_cfg, &cfg)) {
					LOGI("target changed, redialing");
					break;
				}
				cfg = tick_cfg;
			}

			int events = NET_WAIT_READ;
			if (c.slen > 0)
				events |= NET_WAIT_WRITE;
			int ret = net_wait(sock, events, 200);
			if (ret < 0)
				break;
			if (c.slen > 0)
				client_flush(&c);
			if (c.sfailed)
				break;

			uint64_t now = now_ns();
			if (now - c.lat.last_sync_send > 1000000000ULL) {
				c.lat.last_sync_send = now;
				uint8_t hdr[OBSC_HEADER_SIZE];
				obsc_build_header(hdr, OBSC_PKT_TIMESYNC_REQ,
						  0, now, 0);
				client_send(&c, hdr, OBSC_HEADER_SIZE);
				apply_mic_setting(&c, tick_cfg.mic);
			}

			drain_control_queue(s, &c);

			stats_tick(s, &c);
			if ((ret & NET_WAIT_READ) && !client_read(s, &c))
				break;
		}

		LOGI("phone connection ended");
		close_decoder(&c);
		free(c.rbuf);
		free(c.sbuf);
		net_close(sock);

		pthread_mutex_lock(&s->lock);
		s->stats.connected = false;
		s->stats.streaming = false;
		pthread_mutex_unlock(&s->lock);
		if (s->cbs.link)
			s->cbs.link(s->cbs.ud, false);
		sleep_ms(500);
	}
}

static void *session_thread(void *data)
{
	struct session *s = data;
	dial_loop(s);
	return NULL;
}

/* --- public API --------------------------------------------------------- */

struct session *session_start(const struct session_cfg *cfg,
			      const struct session_cbs *cbs)
{
	struct session *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	pthread_mutex_init(&s->lock, NULL);
	s->cfg = *cfg;
	s->cbs = *cbs;
	if (pthread_create(&s->thread, NULL, session_thread, s) == 0) {
		s->thread_active = true;
	} else {
		pthread_mutex_destroy(&s->lock);
		free(s);
		return NULL;
	}
	return s;
}

void session_stop(struct session *s)
{
	if (!s)
		return;
	s->stop = true;
	if (s->thread_active)
		pthread_join(s->thread, NULL);
	for (int i = 0; i < s->ctl_count; i++)
		free(s->ctl[i]);
	pthread_mutex_destroy(&s->lock);
	free(s);
}

void session_apply(struct session *s, const struct session_cfg *cfg)
{
	pthread_mutex_lock(&s->lock);
	s->cfg = *cfg;
	s->gen++;
	pthread_mutex_unlock(&s->lock);
}

void session_control(struct session *s, const char *json)
{
	pthread_mutex_lock(&s->lock);
	if (s->ctl_count < CTL_QUEUE_MAX)
		s->ctl[s->ctl_count++] = strdup(json);
	pthread_mutex_unlock(&s->lock);
}

void session_stats_copy(struct session *s, struct session_stats *out)
{
	pthread_mutex_lock(&s->lock);
	*out = s->stats;
	pthread_mutex_unlock(&s->lock);
}
