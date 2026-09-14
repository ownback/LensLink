#include "discover.h"

#include "log.h"

#include <mdns.h>

#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Splits an avahi-browse -rpt resolved line on ';'. Returns field count. */
static int split(char *line, char **fields, int max)
{
	int n = 0;
	char *p = line;
	while (n < max) {
		fields[n++] = p;
		char *sep = strchr(p, ';');
		if (!sep)
			break;
		*sep = 0;
		p = sep + 1;
	}
	return n;
}

/* Avahi resolved-line layout:
 * =;<iface>;IPv4;<name>;<type>;<domain>;<hostname>;<addr>;<port>[;txt] */
static int take_avahi_line(const char *line, struct ll_phone *out, int max,
			   int count)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", line);
	char *f[10] = {0};
	int n = split(buf, f, 10);
	if (n < 9 || strcmp(f[0], "=") != 0 || strcmp(f[2], "IPv4") != 0)
		return count;
	/* Only the service we advertise. */
	if (strcmp(f[4], "_lenslink._tcp") != 0)
		return count;
	/* Already have this address? */
	for (int i = 0; i < count; i++)
		if (strcmp(out[i].host, f[7]) == 0)
			return count;
	if (count >= max)
		return count;
	snprintf(out[count].name, sizeof(out[count].name), "%s", f[3]);
	snprintf(out[count].host, sizeof(out[count].host), "%s", f[7]);
	return count + 1;
}

static int browse_avahi(struct ll_phone *out, int max)
{
	/* -r resolve, -p parseable, -t terminate after dumping the cache. */
	FILE *p = popen("avahi-browse -rpt _lenslink._tcp", "r");
	if (!p)
		return -1;

	int fd = fileno(p);
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	int count = 0;
	char line[512];
	size_t len = 0;
	time_t start = time(NULL);

	for (;;) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int r = poll(&pfd, 1, 150);
		if (r < 0)
			break;
		if (r == 0) {
			if (time(NULL) - start >= 2)
				break; /* -t should have ended it */
			continue;
		}
		char chunk[256];
		int n = (int)read(fd, chunk, sizeof(chunk) - 1);
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++) {
			if (chunk[i] == '\n' || chunk[i] == '\r') {
				line[len] = 0;
				if (len && line[0] == '=')
					count = take_avahi_line(
						line, out, max, count);
				len = 0;
				if (count >= max)
					break;
			} else if (len + 1 < sizeof(line)) {
				line[len++] = chunk[i];
			}
		}
		if (count >= max)
			break;
	}

	pclose(p);
	return count;
}

int ll_discover_phones(struct ll_phone *out, int max)
{
	if (max <= 0)
		return 0;

	int n = browse_avahi(out, max);
	if (n > 0)
		return n;

	/* No avahi (or nothing cached): the raw one-shot query shared with
	 * the OBS plugin still works on networks whose responder answers
	 * unicast QU queries. */
	struct mdns_result raw[8];
	int m = mdns_browse("_lenslink._tcp.local", 1000, raw, 8);
	for (int i = 0; i < m && n < max; i++) {
		bool dup = false;
		for (int j = 0; j < n; j++)
			if (strcmp(out[j].host, raw[i].host) == 0)
				dup = true;
		if (dup)
			continue;
		snprintf(out[n].name, sizeof(out[n].name), "%.63s",
			 raw[i].name);
		snprintf(out[n].host, sizeof(out[n].host), "%.63s",
			 raw[i].host);
		n++;
	}
	return n;
}
