#include "vcam.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define CARD_LABEL "LensLink Virtual Camera"

static bool is_loopback_card(const char *path)
{
	int fd = open(path, O_RDWR);
	if (fd < 0)
		return false;
	struct v4l2_capability caps;
	memset(&caps, 0, sizeof(caps));
	bool ok = ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0 &&
		  strstr((const char *)caps.card, CARD_LABEL) != NULL;
	close(fd);
	return ok;
}

static bool find_device(char *out, size_t size)
{
	for (int i = 0; i < 64; i++) {
		snprintf(out, size, "/dev/video%d", i);
		if (is_loopback_card(out))
			return true;
	}
	return false;
}

bool vcam_open(struct vcam *v, const char *device, unsigned w, unsigned h)
{
	bool had_warned = v->warned;
	memset(v, 0, sizeof(*v));
	v->fd = -1;
	v->warned = had_warned;

	char path[64];
	if (device && device[0]) {
		snprintf(path, sizeof(path), "%s", device);
	} else if (!find_device(path, sizeof(path))) {
		if (!v->warned) {
			LOGE("no v4l2loopback device labelled \"%s\" found — "
			     "load it with: modprobe v4l2loopback "
			     "video_nr=9 card_label=\"%s\" exclusive_caps=1",
			     CARD_LABEL, CARD_LABEL);
			v->warned = true;
		}
		return false;
	}

	v->fd = open(path, O_RDWR);
	if (v->fd < 0) {
		if (!v->warned) {
			LOGE("cannot open %s: %s", path, strerror(errno));
			v->warned = true;
		}
		return false;
	}
	snprintf(v->path, sizeof(v->path), "%s", path);

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(v->fd, VIDIOC_S_FMT, &fmt) != 0) {
		LOGE("VIDIOC_S_FMT on %s failed: %s", path, strerror(errno));
		vcam_close(v);
		return false;
	}

	v->width = w;
	v->height = h;
	v->fourcc = fmt.fmt.pix.pixelformat;
	v->packed_size = (size_t)w * h * 3 / 2;
	v->packed = malloc(v->packed_size);
	if (!v->packed) {
		vcam_close(v);
		return false;
	}

	LOGI("virtual camera ready: %s %ux%u", path, w, h);
	return true;
}

void vcam_close(struct vcam *v)
{
	if (v->fd >= 0)
		close(v->fd);
	free(v->packed);
	bool had_warned = v->warned;
	memset(v, 0, sizeof(*v));
	v->fd = -1;
	v->warned = had_warned;
}

static bool copy_plane(uint8_t *dst, size_t dst_stride, const uint8_t *src,
		       int src_stride, unsigned height)
{
	if ((size_t)src_stride == dst_stride) {
		memcpy(dst, src, dst_stride * height);
		return true;
	}
	for (unsigned y = 0; y < height; y++)
		memcpy(dst + y * dst_stride, src + y * src_stride, dst_stride);
	return true;
}

bool vcam_write(struct vcam *v, const AVFrame *f)
{
	unsigned w = (unsigned)f->width, h = (unsigned)f->height;
	bool nv12 = f->format == AV_PIX_FMT_NV12;
	bool yuv420p = f->format == AV_PIX_FMT_YUV420P;

	/* Reopen on format change; the bitstream is authoritative. */
	if (v->fd >= 0 &&
	    ((nv12 && v->fourcc != V4L2_PIX_FMT_NV12) ||
	     (yuv420p && v->fourcc != V4L2_PIX_FMT_YUV420) || v->width != w ||
	     v->height != h)) {
		char path[64];
		snprintf(path, sizeof(path), "%s", v->path);
		vcam_close(v);
		vcam_open(v, path, w, h);
	}
	if (v->fd < 0 && !vcam_open(v, v->path[0] ? v->path : NULL, w, h))
		return false;

	size_t ysize = (size_t)w * h;
	size_t csize = ysize / 4;

	if (yuv420p) {
		copy_plane(v->packed, w, f->data[0], f->linesize[0], h);
		copy_plane(v->packed + ysize, w / 2, f->data[1],
			   f->linesize[1], h / 2);
		copy_plane(v->packed + ysize + csize, w / 2, f->data[2],
			   f->linesize[2], h / 2);
	} else if (nv12) {
		struct v4l2_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		fmt.fmt.pix.width = w;
		fmt.fmt.pix.height = h;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;
		ioctl(v->fd, VIDIOC_S_FMT, &fmt);
		v->fourcc = V4L2_PIX_FMT_NV12;
		copy_plane(v->packed, w, f->data[0], f->linesize[0], h);
		copy_plane(v->packed + ysize, w, f->data[1], f->linesize[1],
			   h / 2);
	} else {
		if (!v->warned) {
			LOGW("unsupported decoded pixel format %d",
			     f->format);
			v->warned = true;
		}
		return false;
	}

	size_t total =
		v->fourcc == V4L2_PIX_FMT_NV12 ? ysize + ysize / 2 : ysize * 3 / 2;
	return write(v->fd, v->packed, total) == (ssize_t)total;
}
