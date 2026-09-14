#include "config.h"
#include "daemon.h"
#include "log.h"

#include <signal.h>
#include <string.h>

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--daemon") == 0)
			continue;
		if (strcmp(argv[i], "--version") == 0) {
			printf("lenslinkd " LENSLINK_VERSION "\n");
			return 0;
		}
		fprintf(stderr, "usage: lenslinkd [--version]\n");
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);

	struct ll_config cfg;
	ll_config_load(&cfg);
	LOGI("starting (enabled=%d usb=%d host=%s)", cfg.enabled, cfg.usb,
	     cfg.host);
	return daemon_run(&cfg);
}
