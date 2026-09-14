/* Daemon state machine: owns the session, the virtual camera and
 * microphone, the config, and the IPC surface. */

#pragma once

#include <stdbool.h>

#include "config.h"

/* Runs until asked to quit (SIGTERM/SIGINT or an IPC "quit"). */
int daemon_run(const struct ll_config *initial);
void daemon_request_quit(void);
