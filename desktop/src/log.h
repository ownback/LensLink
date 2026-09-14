/* LensLink desktop daemon logging. */

#pragma once

#include <stdio.h>
#include <time.h>

static inline void ll_log_time(char *buf, size_t size)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	strftime(buf, size, "%H:%M:%S", &tm);
}

#define LL_LOG(level, fmt, ...)                                        \
	do {                                                           \
		char t_[16];                                           \
		ll_log_time(t_, sizeof(t_));                           \
		fprintf(stderr, "%s [lenslink][%s] " fmt "\n", t_,     \
			level, ##__VA_ARGS__);                         \
	} while (0)

#define LOGI(fmt, ...) LL_LOG("info", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LL_LOG("warn", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LL_LOG("error", fmt, ##__VA_ARGS__)
