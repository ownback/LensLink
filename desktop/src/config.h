/* Daemon configuration, persisted at $XDG_CONFIG_HOME/lenslink/config.json
 * so background start restores the last state. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

struct ll_config {
	bool enabled;      /* dial the phone and feed the virtual devices */
	bool remote_start; /* start the phone's camera when it's reachable and idle */
	bool mic;          /* ask the phone to send its mic as the stream audio */
	bool usb;          /* connect over USB instead of Wi-Fi */
	char usb_udid[64]; /* empty = first USB phone */
	char host[64];     /* phone IP for Wi-Fi; empty = autodetect via mDNS */
	char device[64];   /* v4l2loopback path; empty = autodetect by card label */
};

void ll_config_defaults(struct ll_config *c);

/* Missing file reads as defaults; malformed file falls back to defaults. */
void ll_config_load(struct ll_config *c);
void ll_config_save(const struct ll_config *c);

/* Config file path (for diagnostics). Static storage. */
const char *ll_config_path(void);
