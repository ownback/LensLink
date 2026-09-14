/* v4l2loopback writer: turns decoded frames into a system webcam. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/frame.h>

struct vcam {
	int fd;
	char path[64];
	unsigned width, height;
	uint32_t fourcc;
	uint8_t *packed;
	size_t packed_size;
	bool warned;
};

/* Opens the loopback device (device may be NULL — auto-detects by card
 * label) and negotiates `w`x`h`. Re-calls with new dimensions re-negotiate
 * in place. */
bool vcam_open(struct vcam *v, const char *device, unsigned w, unsigned h);
void vcam_close(struct vcam *v);
bool vcam_write(struct vcam *v, const AVFrame *f);
