#define _GNU_SOURCE
#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "doh.h"

#define TIMEOUT_MS 3500
#define ANSWER_MAX 65535

struct url {
	char host[256];
	int port;
};

static CURLSH *share;
static pthread_mutex_t locks[CURL_LOCK_DATA_LAST];
static char *ca_file;

static bool parse_url(const char *text, struct url *u) {
	if (strncasecmp(text, "https://", 8) != 0 || strlen(text) >= DOH_URL_MAX) {
		return false;
	}
	for (const char *c = text; *c; c++) {
		if ((unsigned char)*c <= ' ' || *c == '"' || *c == '\\') {
			return false;
		}
	}
	const char *host = text + 8, *end;
	u->port = 443;
	if (*host == '[') {
		end = strchr(host, ']');
		if (!end || end - host - 1 >= (long)sizeof(u->host) || end == host + 1) {
			return false;
		}
		snprintf(u->host, sizeof(u->host), "%.*s", (int)(end - host - 1), host + 1);
		end++;
	} else {
		end = host + strcspn(host, ":/?#");
		if (end == host || end - host >= (long)sizeof(u->host)) {
			return false;
		}
		snprintf(u->host, sizeof(u->host), "%.*s", (int)(end - host), host);
	}
	if (*end == ':') {
		char *after;
		long port = strtol(end + 1, &after, 10);
		if (after == end + 1 || port <= 0 || port > 65535) {
			return false;
		}
		u->port = (int)port;
		end = after;
	}
	return *end == '\0' || *end == '/' || *end == '?';
}

bool doh_url_valid(const char *url) {
	struct url u;
	return parse_url(url, &u);
}

void doh_set_ca_file(const char *path) {
	free(ca_file);
	ca_file = path ? strdup(path) : NULL;
}

static void lock(CURL *handle, curl_lock_data data, curl_lock_access access, void *user) {
	pthread_mutex_lock(&locks[data]);
}

static void unlock(CURL *handle, curl_lock_data data, void *user) {
	pthread_mutex_unlock(&locks[data]);
}

void doh_init(void) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) {
		pthread_mutex_init(&locks[i], NULL);
	}
	// the open connections and TLS sessions, for all the threads that ask
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock);
	curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlock);
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
}

struct body {
	uint8_t *data;
	size_t len;
	bool too_big;
};

static size_t on_data(char *ptr, size_t size, size_t n, void *user) {
	struct body *b = user;
	size_t len = size * n;
	if (b->len + len > ANSWER_MAX) {
		b->too_big = true;
		return 0;
	}
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	return len;
}

size_t doh_ask(const struct sockaddr_storage *addr, socklen_t addrlen, const char *url,
		const uint8_t *query, size_t qlen, uint8_t **answer, char *error, size_t error_size) {
	*answer = NULL;
	struct url u;
	if (!share || !parse_url(url, &u)) {
		snprintf(error, error_size, share ? "not an https:// address" : "no HTTPS");
		return 0;
	}
	// to the server's address as in the config, whatever the host name says
	char ip[INET6_ADDRSTRLEN] = "", connect_to[INET6_ADDRSTRLEN + 300];
	inet_ntop(addr->ss_family, addr->ss_family == AF_INET ?
		(const void *)&((const struct sockaddr_in *)addr)->sin_addr :
		(const void *)&((const struct sockaddr_in6 *)addr)->sin6_addr, ip, sizeof(ip));
	snprintf(connect_to, sizeof(connect_to), addr->ss_family == AF_INET6 ?
		"::[%s]:%d" : "::%s:%d", ip, u.port);
	struct curl_slist *to = curl_slist_append(NULL, connect_to);
	struct curl_slist *headers = curl_slist_append(NULL,
		"Content-Type: application/dns-message");
	headers = curl_slist_append(headers, "Accept: application/dns-message");
	struct body body = { malloc(ANSWER_MAX), 0, false };
	char curl_error[CURL_ERROR_SIZE] = "";

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_SHARE, share);
	curl_easy_setopt(c, CURLOPT_CONNECT_TO, to);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, query);
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)qlen);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)TIMEOUT_MS);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "tilewin-dnsd");
	curl_easy_setopt(c, CURLOPT_PROXY, ""); // never through a proxy of the environment
	if (ca_file) {
		curl_easy_setopt(c, CURLOPT_CAINFO, ca_file);
	}
	CURLcode res = curl_easy_perform(c);
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(c);
	curl_slist_free_all(to);
	curl_slist_free_all(headers);

	if (res == CURLE_PEER_FAILED_VERIFICATION) {
		if (strstr(curl_error, "subject name") || strstr(curl_error, "host name")) {
			snprintf(error, error_size, "the certificate is not for %s", u.host);
		} else {
			snprintf(error, error_size, "the certificate is not trusted: %s", curl_error);
		}
	} else if (res != CURLE_OK) {
		snprintf(error, error_size, "%s", body.too_big ? "the answer is too big" :
			*curl_error ? curl_error : curl_easy_strerror(res));
	} else if (status != 200) {
		snprintf(error, error_size, "the server answered HTTP %ld", status);
	} else if (body.len == 0) {
		snprintf(error, error_size, "the answer is empty");
	} else {
		*answer = body.data;
		return body.len;
	}
	free(body.data);
	return 0;
}
