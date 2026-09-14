/* Unix-socket JSON IPC between the daemon and the tray. One request line
 * in, one response line out. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Starts the IPC server (its own thread). Returns false when the socket
 * can't be created — daemon keeps running without it. */
bool ipc_server_start(void (*handler)(void *ud, const char *request,
				      char *response, size_t size),
		      void *ud);
void ipc_server_stop(void);

/* Client side (tray): sends `request`, fills `response`. False when the
 * daemon isn't reachable. */
bool ipc_request(const char *request, char *response, size_t size);

/* Socket path (for diagnostics). Static storage. */
const char *ipc_socket_path(void);
