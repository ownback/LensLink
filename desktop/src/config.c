#include "config.h"

#include "json.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CARD_LABEL "LensLink Virtual Camera"

void ll_config_defaults(struct ll_config *c)
{
	memset(c, 0, sizeof(*c));
	c->enabled = true;
	c->remote_start = true;
}

static void config_dir(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(buf, size, "%s/lenslink", xdg);
	else {
		const char *home = getenv("HOME");
		snprintf(buf, size, "%s/.config/lenslink", home ? home : ".");
	}
}

const char *ll_config_path(void)
{
	static char path[512];
	char dir[256];
	config_dir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/config.json", dir);
	return path;
}

void ll_config_load(struct ll_config *c)
{
	ll_config_defaults(c);

	FILE *f = fopen(ll_config_path(), "r");
	if (!f)
		return;

	char json[4096];
	size_t n = fread(json, 1, sizeof(json) - 1, f);
	fclose(f);
	json[n] = 0;

	if (strstr(json, "\"enabled\""))
		c->enabled = json_get_bool(json, "enabled");
	if (strstr(json, "\"remote_start\""))
		c->remote_start = json_get_bool(json, "remote_start");
	if (strstr(json, "\"mic\""))
		c->mic = json_get_bool(json, "mic");
	if (strstr(json, "\"usb\""))
		c->usb = json_get_bool(json, "usb");

	json_get_string(json, "usb_udid", c->usb_udid, sizeof(c->usb_udid));
	json_get_string(json, "host", c->host, sizeof(c->host));
	json_get_string(json, "device", c->device, sizeof(c->device));
}

void ll_config_save(const struct ll_config *c)
{
	char dir[256];
	config_dir(dir, sizeof(dir));
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		LOGW("config: mkdir %s: %s", dir, strerror(errno));

	char tmp_path[520];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ll_config_path());
	FILE *f = fopen(tmp_path, "w");
	if (!f) {
		LOGW("config: cannot save %s: %s", tmp_path,
		     strerror(errno));
		return;
	}

	fprintf(f,
		"{\n"
		"  \"enabled\": %s,\n"
		"  \"remote_start\": %s,\n"
		"  \"mic\": %s,\n"
		"  \"usb\": %s,\n"
		"  \"usb_udid\": \"%s\",\n"
		"  \"host\": \"%s\",\n"
		"  \"device\": \"%s\"\n"
		"}\n",
		c->enabled ? "true" : "false",
		c->remote_start ? "true" : "false", c->mic ? "true" : "false",
		c->usb ? "true" : "false", c->usb_udid, c->host, c->device);
	fclose(f);

	if (rename(tmp_path, ll_config_path()) != 0)
		LOGW("config: rename failed: %s", strerror(errno));
}
