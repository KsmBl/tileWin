#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "domainset.h"
#include "wire.h"

/*
 * tilewin-dnsd: the DNS service of tileWin. It sits between the computer and
 * the DNS servers (systemd-resolved asks it, NetworkManager points resolved
 * here) and does what the DNS page of the settings app offers:
 *
 *  - servers for all networks, or of their own for single networks (read from
 *    NetworkManager's connection files), else the ones the network hands out;
 *  - of many servers, the fastest ones: each is timed once an hour (or as
 *    often as set) and the queries go to the fastest few at once;
 *  - a cache with a shortest and a longest time to keep an answer;
 *  - prefetching: the names asked for most in the last days are asked again
 *    before their answer runs out, even when nobody asks for them any more.
 *    Only the queries of apps are counted for that, never the prefetches, or
 *    the names it prefetches would keep themselves at the top;
 *  - block lists like Pi-hole's: hosts files, plain lists and adblock rules,
 *    downloaded once a day, with names allowed and blocked by hand.
 *
 * Its state is in /var/lib/tilewin-dns (the counts, the lists) and what it is
 * doing in /run/tilewin-dns (status, prefetch), which the settings app reads.
 * SIGHUP reads the config again, SIGUSR1 times the servers now, SIGUSR2
 * downloads the block lists now.
 */

#define MAX_SERVERS 32
#define MAX_TARGETS 8
#define MAX_CONNS 64
#define FAIL_MS 5000
#define RETRY_MS 1200
#define TIMEOUT_MS 4000
#define TEST_TIMEOUT_MS 2000
#define TEST_PROBES 3
#define STATS_DAYS 30
#define STATS_MAX 100000
#define PREFETCH_MAX 2000
#define PREFETCH_EVERY_MS 5000
#define PREFETCH_AGAIN_MS 30000
#define PREFETCH_PER_ROUND 25
#define BUCKETS 65536
#define UP_SOCKETS 4
#define LIST_MAX_AGE (24 * 3600)

struct strv {
	char **v;
	int n;
};

struct server {
	char text[80];
	struct sockaddr_storage addr;
	socklen_t len;
	int score; // ms, -1 untested
};

struct config {
	struct server servers[MAX_SERVERS];
	int nservers;
	int fastest, test_interval;
	bool cache;
	int cache_min, cache_max, cache_size;
	bool prefetch;
	int prefetch_percent, prefetch_days;
	struct strv prefetch_add, prefetch_skip;
	bool blocking, block_nxdomain;
	struct strv lists, allow, block;
};

struct centry {
	struct centry *next;
	char *key;
	uint8_t *msg;
	size_t len;
	int64_t stored, expires, used; // ms
};

struct sentry {
	struct sentry *next;
	char *key; // "name type"
	int32_t day[STATS_DAYS];
	uint32_t count[STATS_DAYS];
};

struct pfitem {
	char *name;
	uint16_t type;
	uint32_t count;
	int64_t last;
};

enum pkind { P_UDP, P_TCP, P_PREFETCH, P_TEST };

struct pending {
	uint16_t id;
	uint32_t gen;
	enum pkind kind;
	struct sockaddr_storage client;
	socklen_t clientlen;
	uint16_t client_id, client_udp;
	uint32_t conn_serial;
	struct dns_question q;
	uint8_t *query, *fail;
	size_t qlen, fail_len;
	struct server targets[MAX_TARGETS];
	int ntargets, nsent, nfailed;
	bool tcp_busy;
	int64_t started, retry_at, deadline;
	int test_server, test_probe;
	uint32_t test_gen;
};

struct conn {
	int fd;
	uint32_t serial;
	uint8_t buf[2 + 65535];
	size_t have;
	int64_t last;
};

struct tcp_job {
	uint16_t id;
	uint32_t gen;
	struct server target;
	uint8_t *query, *answer;
	size_t qlen, alen;
};

struct list_meta {
	int count;
	time_t mtime;
	char error[96];
};

static struct {
	char *conf_path, *state_dir, *run_dir, *nm_run, *nm_keyfiles, *listen_text;
	struct sockaddr_storage listen_addr;
	struct config conf;
	int udp_fd, tcp_fd, sig_fd, job_pipe[2];
	int up4[UP_SOCKETS], up6[UP_SOCKETS];
	struct conn conns[MAX_CONNS];
	uint32_t conn_serial, gen;
	struct server up[MAX_TARGETS];
	int nup, batch;
	const char *source;
	struct server net_own[MAX_TARGETS], net_auto[MAX_TARGETS];
	int nown, nauto;
	char net_sig[4096];
	struct pending *pend[65536];
	int npending;
	struct centry *cache[BUCKETS];
	int ncache;
	struct sentry *stats[BUCKETS];
	int nstats;
	bool stats_dirty;
	struct pfitem *pf;
	int npf;
	struct domainset blocked, allowed;
	struct list_meta *lists;
	int nlists;
	int *download_queue, ndownload;
	pid_t download_pid;
	int download_index;
	bool lists_changed;
	int test_samples[MAX_SERVERS][TEST_PROBES];
	int tests_outstanding;
	uint32_t test_gen;
	time_t tested_at;
	int today;
	unsigned long queries, cached, blocked_count, prefetched, forwarded;
	bool status_dirty;
	int64_t next_test, next_prefetch, next_prefetch_list, next_stats_save, next_net,
		next_list_check, next_status;
	bool quit;
} d;

/* ---------- little helpers ---------- */

static void logmsg(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Days since 1970 in local time, for the counts per day. */
static int local_day(void) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	return (int)((t + tm.tm_gmtoff) / 86400);
}

static uint32_t hash_str(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	return h;
}

static uint16_t random16(void) {
	uint16_t v = 0;
	if (getrandom(&v, sizeof(v), 0) != sizeof(v)) {
		v = (uint16_t)(rand() ^ now_ms());
	}
	return v;
}

static void strv_add(struct strv *s, const char *text) {
	s->v = realloc(s->v, sizeof(char *) * (s->n + 1));
	s->v[s->n++] = strdup(text);
}

static void strv_clear(struct strv *s) {
	for (int i = 0; i < s->n; i++) {
		free(s->v[i]);
	}
	free(s->v);
	s->v = NULL;
	s->n = 0;
}

static bool strv_has(const struct strv *s, const char *text) {
	for (int i = 0; i < s->n; i++) {
		if (strcmp(s->v[i], text) == 0) {
			return true;
		}
	}
	return false;
}

/* Writes a file whole or not at all. */
static bool write_file(const char *path, const char *text, mode_t mode, gid_t group) {
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) {
		return false;
	}
	size_t len = strlen(text), done = 0;
	while (done < len) {
		ssize_t n = write(fd, text + done, len - done);
		if (n <= 0) {
			close(fd);
			unlink(tmp);
			return false;
		}
		done += n;
	}
	if (group != (gid_t)-1 && fchown(fd, (uid_t)-1, group) != 0) {
		// not root (the tests): the file stays the user's
	}
	fchmod(fd, mode);
	close(fd);
	return rename(tmp, path) == 0;
}

/* A growing piece of text. */
struct text {
	char *s;
	size_t len, size;
};

static void text_add(struct text *t, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *piece = NULL;
	int n = vasprintf(&piece, fmt, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	if (t->len + n + 1 > t->size) {
		t->size = (t->len + n + 1) * 2;
		t->s = realloc(t->s, t->size);
	}
	memcpy(t->s + t->len, piece, n + 1);
	t->len += n;
	free(piece);
}

/* ---------- addresses ---------- */

/* "1.1.1.1", "1.1.1.1:5353", "2606:4700::1111" or "[::1]:5353". */
static bool parse_address(const char *text, uint16_t port, struct sockaddr_storage *addr,
		socklen_t *len) {
	char host[80];
	snprintf(host, sizeof(host), "%s", text);
	char *colon = strrchr(host, ':');
	if (host[0] == '[') {
		char *close = strchr(host, ']');
		if (!close) {
			return false;
		}
		*close = '\0';
		if (close[1] == ':') {
			port = (uint16_t)atoi(close + 2);
		}
		memmove(host, host + 1, strlen(host));
	} else if (colon && strchr(host, ':') == colon) {
		*colon = '\0';
		port = (uint16_t)atoi(colon + 1);
	}
	memset(addr, 0, sizeof(*addr));
	struct sockaddr_in *v4 = (struct sockaddr_in *)addr;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)addr;
	if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port = htons(port);
		*len = sizeof(*v4);
		return port != 0;
	}
	char *percent = strchr(host, '%');
	if (percent) {
		*percent = '\0';
	}
	if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port = htons(port);
		*len = sizeof(*v6);
		return port != 0;
	}
	return false;
}

static bool same_address(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
	if (a->ss_family != b->ss_family) {
		return false;
	}
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
		return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
	}
	const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
	return x->sin6_port == y->sin6_port &&
		memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
}

/* Whether a server would be the service itself or the resolver that asks it. */
static bool is_loop(const struct server *s) {
	if (same_address(&s->addr, &d.listen_addr)) {
		return true;
	}
	if (s->addr.ss_family == AF_INET) {
		uint32_t a = ntohl(((const struct sockaddr_in *)&s->addr)->sin_addr.s_addr);
		uint16_t port = ntohs(((const struct sockaddr_in *)&s->addr)->sin_port);
		return port == 53 && (a == 0x7f000035 || a == 0x7f000036); // 127.0.0.53/54
	}
	return false;
}

static bool make_server(const char *text, struct server *s) {
	memset(s, 0, sizeof(*s));
	snprintf(s->text, sizeof(s->text), "%s", text);
	s->score = -1;
	return parse_address(text, 53, &s->addr, &s->len) && !is_loop(s);
}

/* ---------- the config ---------- */

static bool yes(const char *word) {
	return word && (strcmp(word, "yes") == 0 || strcmp(word, "on") == 0 ||
		strcmp(word, "true") == 0 || strcmp(word, "1") == 0);
}

static int number(const char *word, int low, int high, int fallback) {
	if (!word) {
		return fallback;
	}
	char *end;
	long v = strtol(word, &end, 10);
	if (*end || v < low || v > high) {
		return fallback;
	}
	return (int)v;
}

static void config_clear(struct config *c) {
	strv_clear(&c->prefetch_add);
	strv_clear(&c->prefetch_skip);
	strv_clear(&c->lists);
	strv_clear(&c->allow);
	strv_clear(&c->block);
	memset(c, 0, sizeof(*c));
	c->fastest = 2;
	c->test_interval = 3600;
	c->cache = true;
	c->cache_min = 0;
	c->cache_max = 86400;
	c->cache_size = 10000;
	c->prefetch = true;
	c->prefetch_percent = 5;
	c->prefetch_days = 7;
	c->blocking = true;
}

/* The words after the key, lowered, for names. */
static void add_names(struct strv *s, char *rest) {
	char *save = NULL;
	for (char *w = strtok_r(rest, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
		for (char *p = w; *p; p++) {
			*p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
		}
		size_t len = strlen(w);
		while (len > 1 && w[len - 1] == '.') {
			w[--len] = '\0';
		}
		if (!strv_has(s, w)) {
			strv_add(s, w);
		}
	}
}

static void load_config(void) {
	config_clear(&d.conf);
	FILE *f = fopen(d.conf_path, "r");
	if (!f) {
		logmsg("no %s: the network's own servers, with a cache", d.conf_path);
		return;
	}
	char *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, f) > 0) {
		char *p = line;
		p[strcspn(p, "\r\n")] = '\0';
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '#' || *p == '\0') {
			continue;
		}
		char *rest = p + strcspn(p, " \t");
		if (*rest) {
			*rest++ = '\0';
			while (*rest == ' ' || *rest == '\t') {
				rest++;
			}
		}
		char *first = rest;
		char first_word[256];
		snprintf(first_word, sizeof(first_word), "%.*s", (int)strcspn(first, " \t"), first);
		struct config *c = &d.conf;
		if (strcmp(p, "servers") == 0 || strcmp(p, "server") == 0) {
			char *save = NULL;
			for (char *w = strtok_r(rest, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) {
				if (c->nservers < MAX_SERVERS && make_server(w, &c->servers[c->nservers])) {
					c->nservers++;
				} else {
					logmsg("%s: server '%s' left out", d.conf_path, w);
				}
			}
		} else if (strcmp(p, "fastest") == 0) {
			c->fastest = number(first_word, 0, MAX_TARGETS, 2);
		} else if (strcmp(p, "test_interval") == 0) {
			c->test_interval = number(first_word, 60, 7 * 86400, 3600);
		} else if (strcmp(p, "cache") == 0) {
			c->cache = yes(first_word);
		} else if (strcmp(p, "cache_min") == 0) {
			c->cache_min = number(first_word, 0, 7 * 86400, 0);
		} else if (strcmp(p, "cache_max") == 0) {
			c->cache_max = number(first_word, 0, 7 * 86400, 86400);
		} else if (strcmp(p, "cache_size") == 0) {
			c->cache_size = number(first_word, 100, 1000000, 10000);
		} else if (strcmp(p, "prefetch") == 0) {
			c->prefetch = yes(first_word);
		} else if (strcmp(p, "prefetch_percent") == 0) {
			c->prefetch_percent = number(first_word, 1, 100, 5);
		} else if (strcmp(p, "prefetch_days") == 0) {
			c->prefetch_days = number(first_word, 1, STATS_DAYS, 7);
		} else if (strcmp(p, "prefetch_add") == 0) {
			add_names(&c->prefetch_add, rest);
		} else if (strcmp(p, "prefetch_skip") == 0) {
			add_names(&c->prefetch_skip, rest);
		} else if (strcmp(p, "blocking") == 0) {
			c->blocking = yes(first_word);
		} else if (strcmp(p, "block_answer") == 0) {
			c->block_nxdomain = strcmp(first_word, "nxdomain") == 0;
		} else if (strcmp(p, "blocklist") == 0) {
			if (*first_word && !strv_has(&c->lists, first_word)) {
				strv_add(&c->lists, first_word);
			}
		} else if (strcmp(p, "allow") == 0) {
			add_names(&c->allow, rest);
		} else if (strcmp(p, "block") == 0) {
			add_names(&c->block, rest);
		} else {
			logmsg("%s: unknown setting '%s'", d.conf_path, p);
		}
	}
	free(line);
	fclose(f);
	if (d.conf.cache_max && d.conf.cache_max < d.conf.cache_min) {
		d.conf.cache_max = d.conf.cache_min;
	}
}

/* ---------- the network's servers, from NetworkManager ---------- */

static void add_servers(struct server *list, int *n, const char *text) {
	char copy[1024];
	snprintf(copy, sizeof(copy), "%s", text);
	char *save = NULL;
	for (char *w = strtok_r(copy, " ;,\t", &save); w; w = strtok_r(NULL, " ;,\t", &save)) {
		if (*n < MAX_TARGETS && make_server(w, &list[*n])) {
			bool known = false;
			for (int i = 0; i < *n; i++) {
				known |= same_address(&list[i].addr, &list[*n].addr);
			}
			if (!known) {
				(*n)++;
			}
		}
	}
}

/* The own servers of a connection, from its keyfile; whether it has any. */
static bool keyfile_servers(const char *uuid, struct server *list, int *n, bool *only_own) {
	char dirs[PATH_MAX];
	snprintf(dirs, sizeof(dirs), "%s", d.nm_keyfiles);
	char *save = NULL;
	for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		DIR *dh = opendir(dir);
		if (!dh) {
			continue;
		}
		struct dirent *de;
		while ((de = readdir(dh))) {
			if (de->d_name[0] == '.') {
				continue;
			}
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			FILE *f = fopen(path, "r");
			if (!f) {
				continue;
			}
			char line[1024], section[32] = "", dns[2][1024] = { "", "" };
			bool match = false, ignore[2] = { false, false };
			while (fgets(line, sizeof(line), f)) {
				line[strcspn(line, "\r\n")] = '\0';
				if (line[0] == '[') {
					snprintf(section, sizeof(section), "%.*s", (int)strcspn(line + 1, "]"),
						line + 1);
					continue;
				}
				int family = strcmp(section, "ipv4") == 0 ? 0 : strcmp(section, "ipv6") == 0 ? 1 : -1;
				if (strcmp(section, "connection") == 0 && strncmp(line, "uuid=", 5) == 0) {
					match = strcmp(line + 5, uuid) == 0;
				} else if (family >= 0 && strncmp(line, "dns=", 4) == 0) {
					snprintf(dns[family], sizeof(dns[family]), "%s", line + 4);
				} else if (family >= 0 && strncmp(line, "ignore-auto-dns=", 16) == 0) {
					ignore[family] = yes(line + 16);
				}
			}
			fclose(f);
			if (match) {
				closedir(dh);
				add_servers(list, n, dns[0]);
				add_servers(list, n, dns[1]);
				*only_own = ignore[0];
				return *n > 0;
			}
		}
		closedir(dh);
	}
	return false;
}

struct device {
	char uuid[64], servers[1024];
	int metric;
};

static int by_metric(const void *a, const void *b) {
	return ((const struct device *)a)->metric - ((const struct device *)b)->metric;
}

static void read_network(void) {
	d.nown = d.nauto = 0;
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/devices", d.nm_run);
	DIR *dh = opendir(dir);
	struct device devices[16];
	int ndev = 0;
	struct dirent *de;
	while (dh && (de = readdir(dh)) && ndev < 16) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[PATH_MAX + 256];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		struct device *dev = &devices[ndev];
		memset(dev, 0, sizeof(*dev));
		dev->metric = 100000;
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			char *eq = strchr(line, '=');
			if (!eq) {
				continue;
			}
			*eq = '\0';
			if (strcmp(line, "connection-uuid") == 0) {
				snprintf(dev->uuid, sizeof(dev->uuid), "%s", eq + 1);
			} else if (strcmp(line, "route-metric-default-effective") == 0) {
				dev->metric = atoi(eq + 1);
			} else if (strlen(line) > 12 && strcmp(line + strlen(line) - 12, "name_servers") == 0) {
				size_t used = strlen(dev->servers);
				snprintf(dev->servers + used, sizeof(dev->servers) - used, " %s", eq + 1);
			}
		}
		fclose(f);
		if (dev->uuid[0]) {
			ndev++;
		}
	}
	if (dh) {
		closedir(dh);
	}
	qsort(devices, ndev, sizeof(devices[0]), by_metric);
	for (int i = 0; i < ndev; i++) {
		if (d.nown == 0) {
			bool only_own = true;
			if (keyfile_servers(devices[i].uuid, d.net_own, &d.nown, &only_own) && !only_own) {
				add_servers(d.net_own, &d.nown, devices[i].servers);
			}
		}
		add_servers(d.net_auto, &d.nauto, devices[i].servers);
	}
}

/* Changes when a network comes, goes or is changed, so it is read only then. */
static void network_signature(char *out, size_t size) {
	size_t used = 0;
	out[0] = '\0';
	char paths[PATH_MAX * 2];
	snprintf(paths, sizeof(paths), "%s/devices:%s", d.nm_run, d.nm_keyfiles);
	char *save = NULL;
	for (char *dir = strtok_r(paths, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		struct stat st;
		if (stat(dir, &st) == 0) {
			used += snprintf(out + used, size - used, "%ld.%ld;", (long)st.st_mtim.tv_sec,
				st.st_mtim.tv_nsec);
		}
		DIR *dh = opendir(dir);
		struct dirent *de;
		while (dh && (de = readdir(dh)) && used + 64 < size) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			if (de->d_name[0] != '.' && stat(path, &st) == 0) {
				used += snprintf(out + used, size - used, "%ld.%ld/%ld;",
					(long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, (long)st.st_size);
			}
		}
		if (dh) {
			closedir(dh);
		}
	}
}

/* ---------- which servers are asked ---------- */

static int compare_score(const void *a, const void *b) {
	const struct server *x = a, *y = b;
	int sx = x->score < 0 ? FAIL_MS - 1 : x->score;
	int sy = y->score < 0 ? FAIL_MS - 1 : y->score;
	return sx - sy;
}

static void choose_upstreams(void) {
	struct server list[MAX_SERVERS];
	int n = 0, batch = 2;
	if (d.nown > 0) {
		memcpy(list, d.net_own, sizeof(struct server) * d.nown);
		n = d.nown;
		d.source = "network-own";
	} else if (d.conf.nservers > 0) {
		memcpy(list, d.conf.servers, sizeof(struct server) * d.conf.nservers);
		n = d.conf.nservers;
		if (d.conf.fastest > 0) {
			// the untested keep their order, behind those known to answer
			for (int i = 1; i < n; i++) {
				for (int j = i; j > 0 && compare_score(&list[j - 1], &list[j]) > 0; j--) {
					struct server t = list[j];
					list[j] = list[j - 1];
					list[j - 1] = t;
				}
			}
			batch = d.conf.fastest;
			d.source = "fastest";
		} else {
			d.source = "all-networks";
		}
	} else if (d.nauto > 0) {
		memcpy(list, d.net_auto, sizeof(struct server) * d.nauto);
		n = d.nauto;
		d.source = "network";
	} else {
		make_server("1.1.1.1", &list[n++]);
		make_server("9.9.9.9", &list[n++]);
		d.source = "fallback";
	}
	d.nup = n < MAX_TARGETS ? n : MAX_TARGETS;
	memcpy(d.up, list, sizeof(struct server) * d.nup);
	d.batch = batch < d.nup ? batch : d.nup;
	d.status_dirty = true;
}

/* ---------- the cache ---------- */

static void cache_key(char *key, size_t size, const struct dns_question *q) {
	snprintf(key, size, "%s %u %u", q->name, q->type, q->qclass);
}

static struct centry *cache_find(const char *key) {
	for (struct centry *e = d.cache[hash_str(key) % BUCKETS]; e; e = e->next) {
		if (strcmp(e->key, key) == 0) {
			return e;
		}
	}
	return NULL;
}

static void cache_remove(struct centry *victim) {
	struct centry **at = &d.cache[hash_str(victim->key) % BUCKETS];
	while (*at && *at != victim) {
		at = &(*at)->next;
	}
	if (*at) {
		*at = victim->next;
	}
	free(victim->key);
	free(victim->msg);
	free(victim);
	d.ncache--;
}

static void cache_clear(void) {
	for (int b = 0; b < BUCKETS; b++) {
		while (d.cache[b]) {
			cache_remove(d.cache[b]);
		}
	}
}

/* Makes room: the gone first, then the ones asked for longest ago. */
static void cache_make_room(int64_t now) {
	for (int b = 0; b < BUCKETS; b++) {
		for (struct centry *e = d.cache[b], *next; e; e = next) {
			next = e->next;
			if (e->expires <= now) {
				cache_remove(e);
			}
		}
	}
	int goal = d.conf.cache_size - d.conf.cache_size / 16;
	while (d.ncache > goal) {
		struct centry *oldest = NULL;
		for (int b = 0; b < BUCKETS; b++) {
			for (struct centry *e = d.cache[b]; e; e = e->next) {
				if (!oldest || e->used < oldest->used) {
					oldest = e;
				}
			}
		}
		if (!oldest) {
			break;
		}
		cache_remove(oldest);
	}
}

/* Keeps an answer; how long it is kept, 0 if not at all. */
static uint32_t cache_store(const struct dns_question *q, const uint8_t *msg, size_t len,
		int64_t now) {
	uint32_t ttl;
	if (!d.conf.cache || !dns_cache_ttl(msg, len, &ttl)) {
		return 0;
	}
	if ((int)ttl < d.conf.cache_min) {
		ttl = d.conf.cache_min;
	}
	if (d.conf.cache_max > 0 && (int)ttl > d.conf.cache_max) {
		ttl = d.conf.cache_max;
	}
	if (ttl == 0) {
		return 0;
	}
	char key[DNS_NAME_MAX + 32];
	cache_key(key, sizeof(key), q);
	struct centry *e = cache_find(key);
	if (!e) {
		if (d.ncache >= d.conf.cache_size) {
			cache_make_room(now);
		}
		e = calloc(1, sizeof(*e));
		e->key = strdup(key);
		uint32_t b = hash_str(key) % BUCKETS;
		e->next = d.cache[b];
		d.cache[b] = e;
		d.ncache++;
		e->used = now;
	}
	free(e->msg);
	e->msg = malloc(len);
	memcpy(e->msg, msg, len);
	e->len = len;
	e->stored = now;
	e->expires = now + (int64_t)ttl * 1000;
	return ttl;
}

/* ---------- what apps ask for, per day ---------- */

static struct sentry *stats_find(const char *key, bool create) {
	uint32_t b = hash_str(key) % BUCKETS;
	for (struct sentry *e = d.stats[b]; e; e = e->next) {
		if (strcmp(e->key, key) == 0) {
			return e;
		}
	}
	if (!create || d.nstats >= STATS_MAX) {
		return NULL;
	}
	struct sentry *e = calloc(1, sizeof(*e));
	e->key = strdup(key);
	e->next = d.stats[b];
	d.stats[b] = e;
	d.nstats++;
	return e;
}

static void stats_add(struct sentry *e, int day, uint32_t count) {
	int b = day % STATS_DAYS;
	if (e->day[b] != day) {
		e->day[b] = day;
		e->count[b] = 0;
	}
	e->count[b] += count;
}

static uint32_t stats_window(const struct sentry *e, int today, int days) {
	uint32_t sum = 0;
	for (int b = 0; b < STATS_DAYS; b++) {
		if (e->count[b] && e->day[b] > today - days && e->day[b] <= today) {
			sum += e->count[b];
		}
	}
	return sum;
}

/*
 * Counts a query of an app. Only here: a prefetch never comes this way, so
 * the names it keeps fresh are not counted again and again by it, and only
 * stay at the top while the apps themselves keep asking for them.
 */
static void stats_count(const struct dns_question *q) {
	char key[DNS_NAME_MAX + 8];
	snprintf(key, sizeof(key), "%s %u", q->name, q->type);
	struct sentry *e = stats_find(key, true);
	if (e) {
		stats_add(e, d.today, 1);
		d.stats_dirty = true;
	}
}

static void stats_path(char *path, size_t size) {
	snprintf(path, size, "%s/stats", d.state_dir);
}

static void stats_load(void) {
	char path[PATH_MAX];
	stats_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char *line = NULL;
	size_t size = 0;
	while (getline(&line, &size, f) > 0) {
		line[strcspn(line, "\r\n")] = '\0';
		char *save = NULL;
		char *name = strtok_r(line, " ", &save);
		char *type = strtok_r(NULL, " ", &save);
		if (!name || !type || name[0] == '#') {
			continue;
		}
		char key[DNS_NAME_MAX + 8];
		snprintf(key, sizeof(key), "%s %s", name, type);
		struct sentry *e = stats_find(key, true);
		for (char *w = strtok_r(NULL, " ", &save); e && w; w = strtok_r(NULL, " ", &save)) {
			int day;
			unsigned count;
			if (sscanf(w, "%d:%u", &day, &count) == 2 && day > d.today - STATS_DAYS &&
					day <= d.today) {
				stats_add(e, day, count);
			}
		}
	}
	free(line);
	fclose(f);
}

static void stats_save(void) {
	struct text t = { 0 };
	text_add(&t, "# name type day:count ... (days since 1970), only the queries of apps\n");
	for (int b = 0; b < BUCKETS; b++) {
		for (struct sentry *e = d.stats[b]; e; e = e->next) {
			if (stats_window(e, d.today, STATS_DAYS) == 0) {
				continue;
			}
			text_add(&t, "%s", e->key);
			for (int i = 0; i < STATS_DAYS; i++) {
				if (e->count[i] && e->day[i] > d.today - STATS_DAYS) {
					text_add(&t, " %d:%u", e->day[i], e->count[i]);
				}
			}
			text_add(&t, "\n");
		}
	}
	char path[PATH_MAX];
	stats_path(path, sizeof(path));
	if (!write_file(path, t.s, 0600, (gid_t)-1)) {
		logmsg("could not write %s: %s", path, strerror(errno));
	}
	free(t.s);
	d.stats_dirty = false;
}

/* Names gone from the window are forgotten. */
static void stats_prune(void) {
	for (int b = 0; b < BUCKETS; b++) {
		struct sentry **at = &d.stats[b];
		while (*at) {
			struct sentry *e = *at;
			if (stats_window(e, d.today, STATS_DAYS) == 0) {
				*at = e->next;
				free(e->key);
				free(e);
				d.nstats--;
			} else {
				at = &e->next;
			}
		}
	}
}

/* ---------- block lists ---------- */

static bool is_blocked(const char *name) {
	return d.conf.blocking && domainset_matches(&d.blocked, name) &&
		!domainset_matches(&d.allowed, name);
}

static bool is_download(const char *url) {
	return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

/* Where a list is kept: a download under the state, a file where it is. */
static void list_path(const char *url, char *path, size_t size) {
	if (strncmp(url, "file://", 7) == 0) {
		snprintf(path, size, "%s", url + 7);
	} else if (strncmp(url, "file:", 5) == 0) {
		snprintf(path, size, "%s", url + 5);
	} else if (url[0] == '/') {
		snprintf(path, size, "%s", url);
	} else {
		snprintf(path, size, "%s/lists/%08x.list", d.state_dir, hash_str(url));
	}
}

static void rebuild_blocklist(void) {
	struct domainset blocked, allowed;
	domainset_init(&blocked);
	domainset_init(&allowed);
	free(d.lists);
	d.nlists = d.conf.lists.n;
	d.lists = calloc(d.nlists ? d.nlists : 1, sizeof(*d.lists));
	for (int i = 0; i < d.conf.lists.n; i++) {
		char path[PATH_MAX];
		list_path(d.conf.lists.v[i], path, sizeof(path));
		FILE *f = fopen(path, "r");
		if (!f) {
			snprintf(d.lists[i].error, sizeof(d.lists[i].error), "%s",
				is_download(d.conf.lists.v[i]) ? "not-downloaded" : "missing");
			continue;
		}
		struct stat st;
		if (fstat(fileno(f), &st) == 0) {
			d.lists[i].mtime = st.st_mtime;
		}
		char *line = NULL;
		size_t size = 0;
		while (getline(&line, &size, f) > 0) {
			d.lists[i].count += domainset_add_list_line(&blocked, line);
		}
		free(line);
		fclose(f);
	}
	for (int i = 0; i < d.conf.block.n; i++) {
		domainset_add_rule(&blocked, d.conf.block.v[i], true);
	}
	for (int i = 0; i < d.conf.allow.n; i++) {
		domainset_add_rule(&allowed, d.conf.allow.v[i], true);
	}
	domainset_finish(&d.blocked);
	domainset_finish(&d.allowed);
	d.blocked = blocked;
	d.allowed = allowed;
	d.status_dirty = true;
	logmsg("%zu names blocked from %d lists", d.blocked.count, d.conf.lists.n);
}

static void start_next_download(void);

/* The lists to download: the missing and the old, or all of them. */
static void check_lists(bool all) {
	if (d.download_pid > 0) {
		return;
	}
	free(d.download_queue);
	d.download_queue = calloc(d.conf.lists.n + 1, sizeof(int));
	d.ndownload = 0;
	for (int i = 0; i < d.conf.lists.n; i++) {
		if (!is_download(d.conf.lists.v[i])) {
			continue;
		}
		char path[PATH_MAX];
		list_path(d.conf.lists.v[i], path, sizeof(path));
		struct stat st;
		if (all || stat(path, &st) != 0 || time(NULL) - st.st_mtime > LIST_MAX_AGE) {
			d.download_queue[d.ndownload++] = i;
		}
	}
	start_next_download();
}

static void start_next_download(void) {
	if (d.download_pid > 0) {
		return;
	}
	if (d.ndownload == 0) {
		if (d.lists_changed) {
			d.lists_changed = false;
			rebuild_blocklist();
		}
		return;
	}
	int i = d.download_queue[--d.ndownload];
	d.download_index = i;
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX + 8];
	snprintf(dir, sizeof(dir), "%s/lists", d.state_dir);
	mkdir(dir, 0755);
	list_path(d.conf.lists.v[i], path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execlp("curl", "curl", "-fsSL", "--max-time", "180", "--max-filesize", "209715200",
			"--proto", "=http,https", "-o", tmp, d.conf.lists.v[i], (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		snprintf(d.lists[i].error, sizeof(d.lists[i].error), "curl-not-started");
		return;
	}
	d.download_pid = pid;
	logmsg("downloading %s", d.conf.lists.v[i]);
}

static void download_done(int status) {
	int i = d.download_index;
	d.download_pid = 0;
	if (i < d.conf.lists.n && i < d.nlists) {
		char path[PATH_MAX], tmp[PATH_MAX + 8];
		list_path(d.conf.lists.v[i], path, sizeof(path));
		snprintf(tmp, sizeof(tmp), "%s.part", path);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && rename(tmp, path) == 0) {
			d.lists[i].error[0] = '\0';
			d.lists_changed = true;
		} else {
			unlink(tmp);
			snprintf(d.lists[i].error, sizeof(d.lists[i].error), "download-failed");
			logmsg("could not download %s", d.conf.lists.v[i]);
			d.status_dirty = true;
		}
	}
	start_next_download();
}

/* ---------- prefetching ---------- */

static bool skip_prefetch(const char *name) {
	for (int i = 0; i < d.conf.prefetch_skip.n; i++) {
		if (dns_name_within(name, d.conf.prefetch_skip.v[i])) {
			return true;
		}
	}
	return is_blocked(name);
}

struct ranked {
	struct sentry *e;
	uint32_t count;
};

static int by_count(const void *a, const void *b) {
	const struct ranked *x = a, *y = b;
	if (x->count != y->count) {
		return x->count < y->count ? 1 : -1;
	}
	return strcmp(x->e->key, y->e->key);
}

static void pf_add(const char *name, uint16_t type, uint32_t count) {
	for (int i = 0; i < d.npf; i++) {
		if (d.pf[i].type == type && strcmp(d.pf[i].name, name) == 0) {
			return;
		}
	}
	if (d.npf >= PREFETCH_MAX) {
		return;
	}
	d.pf = realloc(d.pf, sizeof(*d.pf) * (d.npf + 1));
	d.pf[d.npf++] = (struct pfitem){ strdup(name), type, count, 0 };
}

static void write_prefetch_file(void) {
	struct text t = { 0 };
	text_add(&t, "");
	for (int i = 0; i < d.npf; i++) {
		text_add(&t, "%u %s %u\n", d.pf[i].count, d.pf[i].name, d.pf[i].type);
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/prefetch", d.run_dir);
	struct group *wheel = getgrnam("wheel");
	// the names say what the computer looks at: only for those who may administer it
	write_file(path, t.s, geteuid() == 0 ? 0640 : 0644, geteuid() == 0 && wheel ?
		wheel->gr_gid : (gid_t)-1);
	free(t.s);
}

/*
 * The top part of the names apps asked for in the last days, and the names
 * always prefetched. The counts come only from the apps (see stats_count).
 */
static void prefetch_recompute(void) {
	for (int i = 0; i < d.npf; i++) {
		free(d.pf[i].name);
	}
	free(d.pf);
	d.pf = NULL;
	d.npf = 0;
	if (!d.conf.prefetch || !d.conf.cache) {
		write_prefetch_file();
		return;
	}
	struct ranked *all = malloc(sizeof(*all) * (d.nstats + 1));
	int n = 0;
	for (int b = 0; b < BUCKETS; b++) {
		for (struct sentry *e = d.stats[b]; e; e = e->next) {
			uint32_t count = stats_window(e, d.today, d.conf.prefetch_days);
			if (count > 0) {
				all[n++] = (struct ranked){ e, count };
			}
		}
	}
	qsort(all, n, sizeof(*all), by_count);
	int take = (int)(((long)n * d.conf.prefetch_percent + 99) / 100);
	for (int i = 0; i < n && i < take; i++) {
		char name[DNS_NAME_MAX];
		unsigned type = 0;
		if (sscanf(all[i].e->key, "%1024s %u", name, &type) != 2 || skip_prefetch(name)) {
			continue;
		}
		pf_add(name, (uint16_t)type, all[i].count);
	}
	free(all);
	for (int i = 0; i < d.conf.prefetch_add.n; i++) {
		if (!is_blocked(d.conf.prefetch_add.v[i])) {
			pf_add(d.conf.prefetch_add.v[i], DNS_TYPE_A, 0);
			pf_add(d.conf.prefetch_add.v[i], DNS_TYPE_AAAA, 0);
		}
	}
	write_prefetch_file();
	d.status_dirty = true;
}

/* ---------- asking the servers ---------- */

static void free_pending(struct pending *p) {
	if (d.pend[p->id] == p) {
		d.pend[p->id] = NULL;
		d.npending--;
	}
	free(p->query);
	free(p->fail);
	free(p);
}

static struct pending *new_pending(enum pkind kind, const struct dns_question *q,
		const uint8_t *query, size_t qlen) {
	if (d.npending > 60000) {
		return NULL;
	}
	struct pending *p = calloc(1, sizeof(*p));
	uint16_t id;
	do {
		id = random16();
	} while (d.pend[id]);
	p->id = id;
	p->gen = ++d.gen;
	p->kind = kind;
	p->q = *q;
	p->query = malloc(qlen);
	memcpy(p->query, query, qlen);
	p->qlen = qlen;
	dns_put16(p->query, id);
	p->started = now_ms();
	p->retry_at = p->started + RETRY_MS;
	p->deadline = p->started + TIMEOUT_MS;
	d.pend[id] = p;
	d.npending++;
	return p;
}

static void send_to_target(struct pending *p, int i) {
	const struct server *s = &p->targets[i];
	int *socks = s->addr.ss_family == AF_INET ? d.up4 : d.up6;
	int fd = socks[random16() % UP_SOCKETS];
	if (fd < 0 || sendto(fd, p->query, p->qlen, 0, (const struct sockaddr *)&s->addr,
			s->len) < 0) {
		p->nfailed++;
	}
}

static void send_more(struct pending *p, int upto) {
	while (p->nsent < upto && p->nsent < p->ntargets) {
		send_to_target(p, p->nsent++);
	}
}

static void forward(struct pending *p) {
	p->ntargets = d.nup;
	memcpy(p->targets, d.up, sizeof(struct server) * d.nup);
	send_more(p, d.batch);
	d.forwarded++;
}

static void deliver(struct pending *p, uint8_t *msg, size_t len) {
	dns_put16(msg, p->client_id);
	if (p->kind == P_UDP) {
		if (len > p->client_udp) {
			len = dns_truncate(msg, len, &p->q);
		}
		sendto(d.udp_fd, msg, len, MSG_DONTWAIT, (struct sockaddr *)&p->client, p->clientlen);
		return;
	}
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &d.conns[i];
		if (c->fd >= 0 && c->serial == p->conn_serial) {
			uint8_t prefix[2];
			dns_put16(prefix, (uint16_t)len);
			struct iovec iov[2] = { { prefix, 2 }, { msg, len } };
			struct msghdr mh = { .msg_iov = iov, .msg_iovlen = 2 };
			sendmsg(c->fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
			return;
		}
	}
}

static void reply_now(struct pending *p, int rcode) {
	uint8_t out[DNS_HEADER + 300 + 32];
	size_t len = dns_build_reply(out, sizeof(out), p->query, &p->q, rcode, false);
	if (len) {
		deliver(p, out, len);
	}
}

/* A speed test probe is over: its time, or FAIL_MS when it got none. */
static void test_sample(struct pending *p, int ms) {
	if (p->test_gen != d.test_gen || p->test_server >= d.conf.nservers) {
		return; // a test from before the config changed
	}
	d.test_samples[p->test_server][p->test_probe] = ms;
	if (--d.tests_outstanding > 0) {
		return;
	}
	for (int i = 0; i < d.conf.nservers; i++) {
		int *s = d.test_samples[i];
		// the middle of three, so one slow or lost answer does not decide
		int a = s[0], b = s[1], c = s[2];
		int mid = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
		d.conf.servers[i].score = mid;
		logmsg("server %s: %d ms", d.conf.servers[i].text, mid);
	}
	d.tested_at = time(NULL);
	choose_upstreams();
}

static void finish(struct pending *p, uint8_t *msg, size_t len, bool good) {
	if (p->kind == P_TEST) {
		test_sample(p, good ? (int)(now_ms() - p->started) : FAIL_MS);
	} else {
		uint32_t kept = good ? cache_store(&p->q, msg, len, now_ms()) : 0;
		if (kept) {
			dns_cap_ttls(msg, len, kept); // nobody keeps it longer than the service
		}
		if (p->kind == P_PREFETCH) {
			d.prefetched++;
		} else if (msg) {
			deliver(p, msg, len);
		} else {
			reply_now(p, DNS_RCODE_SERVFAIL);
		}
	}
	free_pending(p);
}

static void *tcp_worker(void *data) {
	struct tcp_job *job = data;
	int fd = socket(job->target.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct timeval tv = { 3, 0 };
	if (fd >= 0) {
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		uint8_t prefix[2];
		dns_put16(prefix, (uint16_t)job->qlen);
		if (connect(fd, (struct sockaddr *)&job->target.addr, job->target.len) == 0 &&
				send(fd, prefix, 2, MSG_NOSIGNAL) == 2 &&
				send(fd, job->query, job->qlen, MSG_NOSIGNAL) == (ssize_t)job->qlen &&
				recv(fd, prefix, 2, MSG_WAITALL) == 2) {
			size_t len = dns_u16(prefix);
			job->answer = malloc(len ? len : 1);
			if (recv(fd, job->answer, len, MSG_WAITALL) == (ssize_t)len) {
				job->alen = len;
			}
		}
		close(fd);
	}
	ssize_t ignored = write(d.job_pipe[1], &job, sizeof(job));
	(void)ignored;
	return NULL;
}

/* The answer did not fit in UDP: the same question once more over TCP. */
static void ask_over_tcp(struct pending *p, const struct server *target) {
	struct tcp_job *job = calloc(1, sizeof(*job));
	job->id = p->id;
	job->gen = p->gen;
	job->target = *target;
	job->query = malloc(p->qlen);
	memcpy(job->query, p->query, p->qlen);
	job->qlen = p->qlen;
	p->tcp_busy = true;
	p->deadline = now_ms() + TIMEOUT_MS;
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, tcp_worker, job) != 0) {
		free(job->query);
		free(job);
		p->tcp_busy = false;
	}
	pthread_attr_destroy(&attr);
}

static void tcp_job_done(void) {
	struct tcp_job *job;
	if (read(d.job_pipe[0], &job, sizeof(job)) != sizeof(job)) {
		return;
	}
	struct pending *p = d.pend[job->id];
	struct dns_question q;
	if (p && p->gen == job->gen) {
		if (job->alen >= DNS_HEADER && dns_parse_question(job->answer, job->alen, &q) &&
				dns_u16(job->answer) == p->id && strcmp(q.name, p->q.name) == 0 &&
				q.type == p->q.type) {
			finish(p, job->answer, job->alen, true);
		} else {
			finish(p, NULL, 0, false);
		}
	}
	free(job->query);
	free(job->answer);
	free(job);
}

static void on_upstream(int fd) {
	uint8_t buf[65536];
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof(from);
	ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
	if (n < DNS_HEADER || !dns_is_response(buf)) {
		return;
	}
	struct pending *p = d.pend[dns_u16(buf)];
	if (!p || p->tcp_busy) {
		return;
	}
	int target = -1;
	for (int i = 0; i < p->nsent; i++) {
		if (same_address(&p->targets[i].addr, &from)) {
			target = i;
		}
	}
	struct dns_question q;
	if (target < 0 || !dns_parse_question(buf, n, &q) || strcmp(q.name, p->q.name) != 0 ||
			q.type != p->q.type || q.qclass != p->q.qclass) {
		return; // not the answer to this question: spoofed, or very late
	}
	int rcode = dns_rcode(buf);
	if (p->kind == P_TEST) {
		finish(p, buf, n, rcode != DNS_RCODE_SERVFAIL && rcode != DNS_RCODE_REFUSED);
		return;
	}
	if (rcode == DNS_RCODE_SERVFAIL || rcode == DNS_RCODE_REFUSED) {
		free(p->fail);
		p->fail = malloc(n);
		memcpy(p->fail, buf, n);
		p->fail_len = n;
		if (++p->nfailed < p->nsent) {
			return; // another server may still answer
		}
		if (p->nsent < p->ntargets) {
			send_more(p, p->nsent + 1);
			return;
		}
		finish(p, buf, n, false);
		return;
	}
	if (dns_truncated(buf) && p->kind != P_UDP) {
		ask_over_tcp(p, &p->targets[target]);
		return;
	}
	finish(p, buf, n, !dns_truncated(buf));
}

/* ---------- the questions of the apps ---------- */

static void answer_query(uint8_t *msg, size_t len, enum pkind kind,
		const struct sockaddr_storage *from, socklen_t fromlen, uint32_t conn_serial) {
	struct dns_question q;
	if (len < DNS_HEADER || dns_is_response(msg) || !dns_parse_question(msg, len, &q)) {
		return;
	}
	struct pending tmp = { .kind = kind, .client_id = dns_u16(msg), .conn_serial = conn_serial,
		.q = q, .query = msg, .qlen = len };
	tmp.client_udp = dns_udp_size(msg, len);
	if (from) {
		tmp.client = *from;
		tmp.clientlen = fromlen;
	}
	uint8_t out[65536];
	if ((msg[2] >> 3 & 0x0f) != 0) {
		size_t n = dns_build_reply(out, sizeof(out), msg, &q, 4, false); // not implemented
		deliver(&tmp, out, n);
		return;
	}
	d.queries++;
	stats_count(&q);
	if (is_blocked(q.name)) {
		d.blocked_count++;
		size_t n = dns_build_reply(out, sizeof(out), msg, &q,
			d.conf.block_nxdomain ? DNS_RCODE_NXDOMAIN : DNS_RCODE_NOERROR, !d.conf.block_nxdomain);
		deliver(&tmp, out, n);
		return;
	}
	int64_t now = now_ms();
	char key[DNS_NAME_MAX + 32];
	cache_key(key, sizeof(key), &q);
	struct centry *e = d.conf.cache ? cache_find(key) : NULL;
	if (e && e->expires > now && e->len <= sizeof(out)) {
		memcpy(out, e->msg, e->len);
		struct dns_question cq;
		if (dns_parse_question(out, e->len, &cq) && cq.end == q.end) {
			memcpy(out + DNS_HEADER, msg + DNS_HEADER, q.end - DNS_HEADER); // the asker's case
		}
		out[2] = (out[2] & ~0x01) | (msg[2] & 0x01);
		dns_cap_ttls(out, e->len, (uint32_t)((e->expires - now + 999) / 1000));
		e->used = now;
		d.cached++;
		deliver(&tmp, out, e->len);
		return;
	}
	struct pending *p = new_pending(kind, &q, msg, len);
	if (!p) {
		size_t n = dns_build_reply(out, sizeof(out), msg, &q, DNS_RCODE_SERVFAIL, false);
		deliver(&tmp, out, n);
		return;
	}
	p->client = tmp.client;
	p->clientlen = tmp.clientlen;
	p->client_id = tmp.client_id;
	p->client_udp = tmp.client_udp;
	p->conn_serial = conn_serial;
	forward(p);
}

static void on_udp(void) {
	uint8_t buf[65536];
	for (int i = 0; i < 64; i++) {
		struct sockaddr_storage from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = recvfrom(d.udp_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from,
			&fromlen);
		if (n < 0) {
			return;
		}
		answer_query(buf, n, P_UDP, &from, fromlen, 0);
	}
}

static void close_conn(struct conn *c) {
	close(c->fd);
	c->fd = -1;
}

static void on_tcp_accept(void) {
	int fd = accept4(d.tcp_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) {
		return;
	}
	for (int i = 0; i < MAX_CONNS; i++) {
		if (d.conns[i].fd < 0) {
			d.conns[i].fd = fd;
			d.conns[i].serial = ++d.conn_serial;
			d.conns[i].have = 0;
			d.conns[i].last = now_ms();
			return;
		}
	}
	close(fd);
}

static void on_tcp_data(struct conn *c) {
	ssize_t n = recv(c->fd, c->buf + c->have, sizeof(c->buf) - c->have, MSG_DONTWAIT);
	if (n <= 0) {
		if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			close_conn(c);
		}
		return;
	}
	c->have += n;
	c->last = now_ms();
	while (c->have >= 2) {
		size_t len = dns_u16(c->buf);
		if (c->have < 2 + len) {
			break;
		}
		answer_query(c->buf + 2, len, P_TCP, NULL, 0, c->serial);
		memmove(c->buf, c->buf + 2 + len, c->have - 2 - len);
		c->have -= 2 + len;
	}
}

/* ---------- timing the servers ---------- */

static void test_servers(void) {
	d.test_gen++;
	d.tests_outstanding = 0;
	if (d.conf.nservers == 0) {
		return;
	}
	// names that are asked for a lot, so the servers have them at hand: the
	// time measured is the way there and back, not their own lookups
	const char *names[TEST_PROBES] = { "example.com", "wikipedia.org", "cloudflare.com" };
	char top[TEST_PROBES][DNS_NAME_MAX];
	int ntop = 0;
	for (int i = 0; i < d.npf && ntop < TEST_PROBES; i++) {
		if (d.pf[i].type == DNS_TYPE_A) {
			snprintf(top[ntop], sizeof(top[ntop]), "%s", d.pf[i].name);
			names[ntop] = top[ntop];
			ntop++;
		}
	}
	for (int s = 0; s < d.conf.nservers; s++) {
		for (int k = 0; k < TEST_PROBES; k++) {
			d.test_samples[s][k] = FAIL_MS;
			uint8_t query[512];
			size_t len = dns_build_query(query, sizeof(query), 0, names[k], DNS_TYPE_A);
			struct dns_question q;
			if (!len || !dns_parse_question(query, len, &q)) {
				continue;
			}
			struct pending *p = new_pending(P_TEST, &q, query, len);
			if (!p) {
				continue;
			}
			p->deadline = p->started + TEST_TIMEOUT_MS;
			p->retry_at = INT64_MAX;
			p->test_server = s;
			p->test_probe = k;
			p->test_gen = d.test_gen;
			p->targets[0] = d.conf.servers[s];
			p->ntargets = 1;
			d.tests_outstanding++;
			send_more(p, 1);
		}
	}
}

/* ---------- prefetching, every few seconds ---------- */

static void prefetch_round(int64_t now) {
	if (!d.conf.prefetch || !d.conf.cache) {
		return;
	}
	int budget = PREFETCH_PER_ROUND;
	for (int i = 0; i < d.npf && budget > 0; i++) {
		struct pfitem *item = &d.pf[i];
		if (item->last && now - item->last < PREFETCH_AGAIN_MS) {
			continue;
		}
		char key[DNS_NAME_MAX + 32];
		snprintf(key, sizeof(key), "%s %u 1", item->name, item->type);
		struct centry *e = cache_find(key);
		if (e) {
			int64_t life = e->expires - e->stored, left = e->expires - now;
			// asked again in the last tenth of its time, before anyone misses it
			if (left > PREFETCH_EVERY_MS * 2 && left > life / 10) {
				continue;
			}
		}
		uint8_t query[1100];
		size_t len = dns_build_query(query, sizeof(query), 0, item->name, item->type);
		struct dns_question q;
		if (!len || !dns_parse_question(query, len, &q)) {
			continue;
		}
		struct pending *p = new_pending(P_PREFETCH, &q, query, len);
		if (!p) {
			return;
		}
		forward(p);
		item->last = now;
		budget--;
	}
}

/* ---------- status for the settings app ---------- */

static void write_status(void) {
	struct text t = { 0 };
	text_add(&t, "pid %d\n", (int)getpid());
	text_add(&t, "source %s\n", d.source);
	for (int i = 0; i < d.nup; i++) {
		text_add(&t, "%s %s\n", i < d.batch ? "use" : "backup", d.up[i].text);
	}
	for (int i = 0; i < d.conf.nservers; i++) {
		text_add(&t, "server %s %d\n", d.conf.servers[i].text, d.conf.servers[i].score);
	}
	text_add(&t, "tested %ld\n", (long)d.tested_at);
	text_add(&t, "cache %d\n", d.ncache);
	text_add(&t, "today %lu %lu %lu %lu\n", d.queries, d.cached, d.blocked_count, d.prefetched);
	text_add(&t, "blocked_names %zu\n", d.blocked.count);
	for (int i = 0; i < d.conf.lists.n && i < d.nlists; i++) {
		text_add(&t, "list %d %ld %s %s\n", d.lists[i].count, (long)d.lists[i].mtime,
			d.lists[i].error[0] ? d.lists[i].error : "ok", d.conf.lists.v[i]);
	}
	text_add(&t, "downloading %d\n", d.download_pid > 0 ? 1 : 0);
	text_add(&t, "prefetch_names %d\n", d.npf);
	text_add(&t, "counted_names %d\n", d.nstats);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/status", d.run_dir);
	write_file(path, t.s, 0644, (gid_t)-1);
	free(t.s);
	d.status_dirty = false;
}

/* ---------- setting up ---------- */

static int open_socket(int family, int type, const struct sockaddr_storage *addr,
		socklen_t len) {
	int fd = socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (addr && bind(fd, (const struct sockaddr *)addr, len) != 0) {
		close(fd);
		return -1;
	}
	if (type == SOCK_STREAM && listen(fd, 64) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void reload(void) {
	load_config();
	cache_clear(); // the limits may have changed what may be kept
	rebuild_blocklist();
	read_network();
	choose_upstreams();
	prefetch_recompute();
	check_lists(false);
	d.next_test = now_ms() + 200;
	logmsg("config read: %d servers, fastest %d, cache %s, prefetch %s, %d block lists",
		d.conf.nservers, d.conf.fastest, d.conf.cache ? "on" : "off",
		d.conf.prefetch ? "on" : "off", d.conf.lists.n);
}

static void housekeeping(int64_t now) {
	int today = local_day();
	if (today != d.today) {
		d.today = today;
		d.queries = d.cached = d.blocked_count = d.prefetched = 0;
		stats_prune();
	}
	if (d.npending > 0) {
		for (int id = 0; id < 65536; id++) {
			struct pending *p = d.pend[id];
			if (!p) {
				continue;
			}
			if (now >= p->deadline) {
				if (p->fail) {
					finish(p, p->fail, p->fail_len, false);
				} else {
					finish(p, NULL, 0, false);
				}
			} else if (now >= p->retry_at && !p->tcp_busy) {
				p->retry_at = INT64_MAX;
				send_more(p, p->ntargets); // the rest, as the first did not answer
			}
		}
	}
	for (int i = 0; i < MAX_CONNS; i++) {
		if (d.conns[i].fd >= 0 && now - d.conns[i].last > 10000) {
			close_conn(&d.conns[i]);
		}
	}
	if (now >= d.next_net) {
		d.next_net = now + 2000;
		char sig[sizeof(d.net_sig)];
		network_signature(sig, sizeof(sig));
		if (strcmp(sig, d.net_sig) != 0) {
			memcpy(d.net_sig, sig, sizeof(sig));
			read_network();
			choose_upstreams();
		}
	}
	if (now >= d.next_test) {
		d.next_test = now + (int64_t)d.conf.test_interval * 1000;
		test_servers();
	}
	if (now >= d.next_prefetch_list) {
		d.next_prefetch_list = now + 3600 * 1000;
		prefetch_recompute();
	}
	if (now >= d.next_prefetch) {
		d.next_prefetch = now + PREFETCH_EVERY_MS;
		prefetch_round(now);
	}
	if (now >= d.next_stats_save) {
		d.next_stats_save = now + 600 * 1000;
		if (d.stats_dirty) {
			stats_save();
		}
	}
	if (now >= d.next_list_check) {
		d.next_list_check = now + 3600 * 1000;
		check_lists(false);
	}
	if (now >= d.next_status) {
		d.next_status = now + (d.status_dirty ? 1000 : 10000);
		write_status();
	}
}

static void on_signal(void) {
	struct signalfd_siginfo si;
	while (read(d.sig_fd, &si, sizeof(si)) == sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGHUP:
			reload();
			break;
		case SIGUSR1:
			d.next_test = 0;
			break;
		case SIGUSR2:
			check_lists(true);
			d.status_dirty = true;
			break;
		case SIGCHLD:
			for (;;) {
				int status;
				pid_t pid = waitpid(-1, &status, WNOHANG);
				if (pid <= 0) {
					break;
				}
				if (pid == d.download_pid) {
					download_done(status);
				}
			}
			break;
		default:
			d.quit = true;
		}
	}
}

static void usage(const char *name) {
	fprintf(stderr,
		"usage: %s [-c config] [-s state dir] [-r run dir] [-l address:port]\n"
		"          [-n NetworkManager run dir] [-k keyfile dirs, : separated]\n", name);
}

int main(int argc, char **argv) {
	d.conf_path = "/etc/tileWin/dns.conf";
	d.state_dir = "/var/lib/tilewin-dns";
	d.run_dir = "/run/tilewin-dns";
	d.listen_text = "127.0.0.153:53";
	d.nm_run = "/run/NetworkManager";
	d.nm_keyfiles = "/etc/NetworkManager/system-connections:/run/NetworkManager/system-connections";
	int opt;
	while ((opt = getopt(argc, argv, "c:s:r:l:n:k:h")) != -1) {
		switch (opt) {
		case 'c': d.conf_path = optarg; break;
		case 's': d.state_dir = optarg; break;
		case 'r': d.run_dir = optarg; break;
		case 'l': d.listen_text = optarg; break;
		case 'n': d.nm_run = optarg; break;
		case 'k': d.nm_keyfiles = optarg; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}
	socklen_t listen_len;
	if (!parse_address(d.listen_text, 53, &d.listen_addr, &listen_len)) {
		logmsg("cannot listen on '%s'", d.listen_text);
		return 2;
	}
	mkdir(d.state_dir, 0700);
	mkdir(d.run_dir, 0755);
	for (int i = 0; i < MAX_CONNS; i++) {
		d.conns[i].fd = -1;
	}
	int family = d.listen_addr.ss_family;
	d.udp_fd = open_socket(family, SOCK_DGRAM, &d.listen_addr, listen_len);
	d.tcp_fd = open_socket(family, SOCK_STREAM, &d.listen_addr, listen_len);
	if (d.udp_fd < 0 || d.tcp_fd < 0) {
		logmsg("cannot listen on %s: %s", d.listen_text, strerror(errno));
		return 1;
	}
	for (int i = 0; i < UP_SOCKETS; i++) {
		d.up4[i] = open_socket(AF_INET, SOCK_DGRAM, NULL, 0);
		d.up6[i] = open_socket(AF_INET6, SOCK_DGRAM, NULL, 0);
	}
	sigset_t mask;
	sigemptyset(&mask);
	int signals[] = { SIGHUP, SIGUSR1, SIGUSR2, SIGTERM, SIGINT, SIGCHLD };
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaddset(&mask, signals[i]);
	}
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);
	d.sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (pipe2(d.job_pipe, O_CLOEXEC) != 0) {
		return 1;
	}
	d.today = local_day();
	stats_load();
	reload();
	network_signature(d.net_sig, sizeof(d.net_sig));
	d.next_test = 0;
	logmsg("answering on %s", d.listen_text);

	while (!d.quit) {
		struct pollfd fds[6 + 2 * UP_SOCKETS + MAX_CONNS];
		int n = 0;
		fds[n++] = (struct pollfd){ d.udp_fd, POLLIN, 0 };
		fds[n++] = (struct pollfd){ d.tcp_fd, POLLIN, 0 };
		fds[n++] = (struct pollfd){ d.sig_fd, POLLIN, 0 };
		fds[n++] = (struct pollfd){ d.job_pipe[0], POLLIN, 0 };
		int first_up = n;
		for (int i = 0; i < UP_SOCKETS; i++) {
			fds[n++] = (struct pollfd){ d.up4[i], POLLIN, 0 };
			fds[n++] = (struct pollfd){ d.up6[i], POLLIN, 0 };
		}
		int first_conn = n;
		int conn_at[MAX_CONNS];
		for (int i = 0; i < MAX_CONNS; i++) {
			if (d.conns[i].fd >= 0) {
				conn_at[n - first_conn] = i;
				fds[n++] = (struct pollfd){ d.conns[i].fd, POLLIN, 0 };
			}
		}
		int timeout = d.npending > 0 ? 100 : 1000;
		if (poll(fds, n, timeout) < 0 && errno != EINTR) {
			logmsg("poll: %s", strerror(errno));
			break;
		}
		if (fds[0].revents & POLLIN) {
			on_udp();
		}
		if (fds[1].revents & POLLIN) {
			on_tcp_accept();
		}
		if (fds[2].revents & POLLIN) {
			on_signal();
		}
		if (fds[3].revents & POLLIN) {
			tcp_job_done();
		}
		for (int i = first_up; i < first_conn; i++) {
			if (fds[i].fd >= 0 && fds[i].revents & POLLIN) {
				for (int k = 0; k < 64; k++) {
					on_upstream(fds[i].fd);
				}
			}
		}
		for (int i = first_conn; i < n; i++) {
			struct conn *c = &d.conns[conn_at[i - first_conn]];
			if (c->fd == fds[i].fd && fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				on_tcp_data(c);
			}
		}
		housekeeping(now_ms());
	}
	stats_save();
	logmsg("stopped");
	return 0;
}
