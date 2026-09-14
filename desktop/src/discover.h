/* Phone discovery. Prefers avahi-browse (the desktop's mDNS responder
 * cache — the phone only announces via multicast and doesn't answer
 * one-shot QU queries on many networks), falls back to the raw-socket
 * browse shared with the OBS plugin. */

#pragma once

#include <stddef.h>

struct ll_phone {
	char name[64];
	char host[64]; /* dotted IPv4 */
};

/* Blocking (~1.5 s): fills up to `max` phones, returns the count. */
int ll_discover_phones(struct ll_phone *out, int max);
