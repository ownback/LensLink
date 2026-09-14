#include "ipc.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_REQ_MAX 4096

static pthread_t ipc_thread;
static bool ipc_active;
static int listen_fd = -1;
static void (*req_handler)(void *ud, const char *, char *, size_t);
static void *handler_ud;

const char *ipc_socket_path(void)
{
	static char path[96];
	if (!path[0]) {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		snprintf(path, sizeof(path), "%s/lenslinkd.sock",
			 runtime && *runtime ? runtime : "/tmp");
	}
	return path;
}

static void *ipc_server_loop(void *data)
{
	(void)data;
	char req[IPC_REQ_MAX];
	char resp[IPC_REQ_MAX];

	while (ipc_active) {
		int fd = accept(listen_fd, NULL, NULL);
		if (fd < 0)
			break;

		size_t len = 0;
		while (len < sizeof(req) - 1) {
			char ch;
			ssize_t n = recv(fd, &ch, 1, 0);
			if (n <= 0)
				break;
			if (ch == '\n')
				break;
			req[len++] = ch;
		}
		req[len] = 0;

		resp[0] = 0;
		if (len)
			req_handler(handler_ud, req, resp, sizeof(resp));
		if (!resp[0])
			snprintf(resp, sizeof(resp),
				 "{\"type\":\"error\",\"error\":\"no reply\"}");

		strcat(resp, "\n");
		send(fd, resp, strlen(resp), MSG_NOSIGNAL);
		close(fd);
	}
	return NULL;
}

bool ipc_server_start(void (*handler)(void *ud, const char *request,
				      char *response, size_t size),
		      void *ud)
{
	req_handler = handler;
	handler_ud = ud;

	unlink(ipc_socket_path());

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOGW("ipc: socket failed: %s", strerror(errno));
		return false;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
		 ipc_socket_path());
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		LOGW("ipc: bind %s failed: %s", addr.sun_path,
		     strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	if (listen(listen_fd, 4) != 0) {
		LOGW("ipc: listen failed: %s", strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return false;
	}

	ipc_active = true;
	if (pthread_create(&ipc_thread, NULL, ipc_server_loop, NULL) != 0) {
		ipc_active = false;
		close(listen_fd);
		listen_fd = -1;
		return false;
	}
	LOGI("ipc: listening on %s", ipc_socket_path());
	return true;
}

void ipc_server_stop(void)
{
	if (!ipc_active)
		return;
	ipc_active = false;
	if (listen_fd >= 0) {
		/* Unblock accept() with a dummy client connection. */
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
				 ipc_socket_path());
			connect(fd, (struct sockaddr *)&addr, sizeof(addr));
			close(fd);
		}
		pthread_join(ipc_thread, NULL);
		close(listen_fd);
		listen_fd = -1;
	}
	unlink(ipc_socket_path());
}

bool ipc_request(const char *request, char *response, size_t size)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
		 ipc_socket_path());
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return false;
	}

	char line[IPC_REQ_MAX];
	snprintf(line, sizeof(line), "%s\n", request);
	if (send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) {
		close(fd);
		return false;
	}

	size_t got = 0;
	while (got < size - 1) {
		ssize_t n = recv(fd, response + got, size - 1 - got, 0);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (memchr(response, '\n', got))
			break;
	}
	close(fd);
	response[got] = 0;
	while (got > 0 && response[got - 1] == '\n')
		response[--got] = 0;
	return got > 0;
}
