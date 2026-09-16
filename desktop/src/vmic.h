/* PipeWire virtual microphone: publishes an "Audio/Source" node fed by
 * the phone's mic audio (48 kHz stereo S16LE off the wire). Lives only
 * while a stream is up, so apps see the mic appear and disappear with it. */

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vmic {
	void *loop; /* pw_thread_loop* */
	void *stream;
	void *ring;
	pthread_mutex_t lock;
	bool reconnect;      /* stream died — rebuild on the next push */
	uint64_t last_retry_ns;
};

bool vmic_start(struct vmic *v);
void vmic_stop(struct vmic *v);
void vmic_push(struct vmic *v, const int16_t *pcm, size_t frames);
