#include <errno.h>
#include <json.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "ipc.h"
#include "settings.h"

static const char ipc_magic[] = { 'i', '3', '-', 'i', 'p', 'c' };

/*
 * Only talks to tileWin: SWAYSOCK is used as a fallback only when it points
 * at a tileWin socket, so commands never reach a plain sway session.
 */
static char *socket_path(void) {
	const char *path = g_getenv("TILEWINSOCK");
	if (path && *path) {
		return g_strdup(path);
	}
	path = g_getenv("SWAYSOCK");
	if (path && strstr(path, "tilewin")) {
		return g_strdup(path);
	}
	return NULL;
}

static bool write_all(int fd, const void *data, size_t len) {
	const char *p = data;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

static bool read_all(int fd, void *data, size_t len) {
	char *p = data;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

char *tw_ipc_request(uint32_t type, const char *payload) {
	char *path = socket_path();
	if (!path) {
		return NULL;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		g_free(path);
		return NULL;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
	g_free(path);
	struct timeval tv = { .tv_sec = 3 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return NULL;
	}
	uint32_t len = payload ? strlen(payload) : 0;
	char header[14];
	memcpy(header, ipc_magic, sizeof(ipc_magic));
	memcpy(header + 6, &len, 4);
	memcpy(header + 10, &type, 4);
	char *reply = NULL;
	if (write_all(fd, header, sizeof(header)) && write_all(fd, payload, len) &&
			read_all(fd, header, sizeof(header))) {
		uint32_t reply_len;
		memcpy(&reply_len, header + 6, 4);
		reply = g_malloc(reply_len + 1);
		if (read_all(fd, reply, reply_len)) {
			reply[reply_len] = '\0';
		} else {
			g_free(reply);
			reply = NULL;
		}
	}
	close(fd);
	return reply;
}

bool tw_ipc_available(void) {
	char *reply = tw_ipc_request(IPC_GET_TILEWIN, "");
	g_free(reply);
	return reply != NULL;
}

bool tw_ipc_command(const char *command, char **error) {
	char *reply = tw_ipc_request(IPC_COMMAND, command);
	if (!reply) {
		if (error) {
			*error = g_strdup("tileWin is not running");
		}
		return false;
	}
	bool ok = true;
	json_object *obj = json_tokener_parse(reply);
	g_free(reply);
	int n = obj && json_object_is_type(obj, json_type_array) ?
		(int)json_object_array_length(obj) : 0;
	for (int i = 0; i < n; i++) {
		json_object *result = json_object_array_get_idx(obj, i);
		json_object *success, *err;
		if (json_object_object_get_ex(result, "success", &success) &&
				!json_object_get_boolean(success)) {
			ok = false;
			if (error && !*error && json_object_object_get_ex(result, "error", &err)) {
				*error = g_strdup(json_object_get_string(err));
			}
		}
	}
	json_object_put(obj);
	if (!ok && error && !*error) {
		*error = g_strdup("command failed");
	}
	return ok;
}

char *tw_ipc_state(const char *key) {
	char *reply = tw_ipc_request(IPC_GET_TILEWIN, "");
	if (!reply) {
		return NULL;
	}
	char *value = NULL;
	json_object *obj = json_tokener_parse(reply);
	g_free(reply);
	json_object *field;
	if (obj && json_object_object_get_ex(obj, key, &field) && field) {
		value = g_strdup(json_object_get_string(field));
	}
	json_object_put(obj);
	return value;
}
