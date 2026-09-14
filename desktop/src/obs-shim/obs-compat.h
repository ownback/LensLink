/* Minimal OBS-API stand-ins so usbmux.c and mdns.c compile unchanged in
 * the desktop daemon build. Provided via -I obs-shim (stub obs-module.h,
 * util/bmem.h, util/platform.h, util/dstr.h all include this). */

#pragma once

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_DEBUG 100
#define LOG_INFO 200
#define LOG_WARNING 300
#define LOG_ERROR 400

static inline void blog(int level, const char *fmt, ...)
{
	const char *tag = level >= LOG_ERROR	? "error"
			  : level >= LOG_WARNING ? "warn"
						 : "info";
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	char t[16];
	strftime(t, sizeof(t), "%H:%M:%S", &tm);
	fprintf(stderr, "%s [lenslink][%s] %s\n", t, tag, msg);
}

static inline void *bmalloc(size_t size)
{
	return malloc(size);
}

static inline void *bzalloc(size_t size)
{
	return calloc(1, size);
}

static inline void *brealloc(void *p, size_t size)
{
	return realloc(p, size);
}

static inline void bfree(void *p)
{
	free(p);
}

static inline uint64_t os_gettime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void os_sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static inline int astrcmpi(const char *a, const char *b)
{
	while (*a && *b) {
		int ca = tolower((unsigned char)*a++);
		int cb = tolower((unsigned char)*b++);
		if (ca != cb)
			return ca - cb;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
