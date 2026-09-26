/*
 * Wi-Fi capabilities from nl80211, over a plain generic netlink socket: the
 * bands of the adapter (GET_WIPHY) and the information elements of the access
 * points in the last scan (GET_SCAN). Both work without privileges.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "wifi_caps.h"

#define BUF_SIZE 65536

enum wifi_band wifi_band_of(int freq) {
	return freq >= 5925 ? WIFI_BAND_6G : freq >= 4900 ? WIFI_BAND_5G : WIFI_BAND_2G;
}

/* ---------- the rates ---------- */

/*
 * Per stream at the best modulation of each standard with its shortest guard
 * interval: 802.11n MCS 7, ac MCS 9 (MCS 8 at 20 MHz), ax MCS 11, be MCS 13.
 */
double wifi_phy_rate(int gen, int streams, int width) {
	int w = width >= 320 ? 4 : width >= 160 ? 3 : width >= 80 ? 2 : width >= 40 ? 1 : 0;
	static const double rates[4][5] = {
		{ 72.2, 150, 150, 150, 150 },           // n: 40 MHz at most
		{ 86.7, 200, 433.3, 866.7, 866.7 },     // ac
		{ 143.4, 286.8, 600.5, 1201, 1201 },    // ax
		{ 172.1, 344.1, 720.6, 1441.2, 2882.4 }, // be
	};
	if (gen < 4) {
		return 54; // a or g
	}
	int g = gen > 7 ? 3 : gen - 4;
	return rates[g][w] * (streams < 1 ? 1 : streams);
}

static int min_int(int a, int b) {
	return a < b ? a : b;
}

double wifi_pair_rate(const struct wifi_adapter *adapter, const struct wifi_ap *ap, int width,
		struct wifi_link_caps *pair) {
	enum wifi_band band = wifi_band_of(ap->freq);
	const struct wifi_link_caps *mine = &adapter->band[band];
	if (!adapter->known || mine->gen == 0 || ap->caps.gen == 0) {
		return 0;
	}
	struct wifi_link_caps p = {
		.gen = min_int(mine->gen, ap->caps.gen),
		.streams = min_int(mine->streams, ap->caps.streams),
		.width = min_int(mine->width, ap->caps.width),
	};
	if (band == WIFI_BAND_2G && p.gen == 5) {
		p.gen = 4; // ac is 5 GHz only
	}
	int limit = p.gen <= 3 ? 20 : p.gen == 4 ? 40 : p.gen == 7 ? 320 : 160;
	p.width = min_int(p.width, limit);
	if (width > 0) {
		p.width = min_int(p.width, width); // what the access point uses now
	}
	if (p.streams < 1) {
		p.streams = 1;
	}
	if (pair) {
		*pair = p;
	}
	return wifi_phy_rate(p.gen, p.streams, p.width);
}

bool wifi_parse_bssid(const char *text, uint8_t out[6]) {
	unsigned v[6];
	if (sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		out[i] = (uint8_t)v[i];
	}
	return true;
}

/* ---------- netlink ---------- */

struct nl {
	int fd;
	int family;
	uint32_t seq;
};

static struct nlattr *put_attr(struct nlmsghdr *msg, uint16_t type, const void *data, int len) {
	struct nlattr *a = (struct nlattr *)((char *)msg + NLMSG_ALIGN(msg->nlmsg_len));
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	if (len) {
		memcpy((char *)a + NLA_HDRLEN, data, len);
	}
	msg->nlmsg_len = NLMSG_ALIGN(msg->nlmsg_len) + NLA_ALIGN(a->nla_len);
	return a;
}

static bool nl_send(struct nl *nl, int type, int flags, int cmd, int ifindex, bool split) {
	if (nl->family <= 0 && type != GENL_ID_CTRL) {
		return false;
	}
	char buf[256] = { 0 };
	struct nlmsghdr *msg = (struct nlmsghdr *)buf;
	msg->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	msg->nlmsg_type = type;
	msg->nlmsg_flags = NLM_F_REQUEST | flags;
	msg->nlmsg_seq = ++nl->seq;
	struct genlmsghdr *genl = NLMSG_DATA(msg);
	genl->cmd = cmd;
	genl->version = 1;
	if (type == GENL_ID_CTRL) {
		put_attr(msg, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	} else {
		uint32_t index = ifindex;
		put_attr(msg, NL80211_ATTR_IFINDEX, &index, 4);
		if (split) {
			put_attr(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP, NULL, 0);
		}
	}
	return send(nl->fd, buf, msg->nlmsg_len, 0) == (ssize_t)msg->nlmsg_len;
}

#define FOR_ATTRS(a, start, len) \
	for (struct nlattr *a = (struct nlattr *)(start); (char *)a + NLA_HDRLEN <= (char *)(start) + (len) && \
		a->nla_len >= NLA_HDRLEN && (char *)a + a->nla_len <= (char *)(start) + (len); \
		a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)))

static void *attr_data(struct nlattr *a) {
	return (char *)a + NLA_HDRLEN;
}

static int attr_len(struct nlattr *a) {
	return a->nla_len - NLA_HDRLEN;
}

static int attr_type(struct nlattr *a) {
	return a->nla_type & NLA_TYPE_MASK;
}

typedef void (*nl_handler)(struct nlattr *attrs, int len, void *data);

/* Reads the answers to the last request until it is done, each to the handler. */
static bool nl_receive(struct nl *nl, nl_handler handler, void *data) {
	char *buf = malloc(BUF_SIZE);
	bool ok = false, done = false;
	while (buf && !done) {
		ssize_t n = recv(nl->fd, buf, BUF_SIZE, 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		for (struct nlmsghdr *msg = (struct nlmsghdr *)buf; NLMSG_OK(msg, (size_t)n);
				msg = NLMSG_NEXT(msg, n)) {
			if (msg->nlmsg_seq != nl->seq) {
				continue;
			}
			if (msg->nlmsg_type == NLMSG_DONE) {
				done = ok = true;
				break;
			}
			if (msg->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err = NLMSG_DATA(msg);
				done = true;
				ok = err->error == 0;
				break;
			}
			struct genlmsghdr *genl = NLMSG_DATA(msg);
			int len = msg->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
			handler((struct nlattr *)((char *)genl + GENL_HDRLEN), len, data);
			if (!(msg->nlmsg_flags & NLM_F_MULTI)) {
				done = ok = true;
			}
		}
	}
	free(buf);
	return ok;
}

static void family_handler(struct nlattr *attrs, int len, void *data) {
	FOR_ATTRS(a, attrs, len) {
		if (attr_type(a) == CTRL_ATTR_FAMILY_ID) {
			*(int *)data = *(uint16_t *)attr_data(a);
		}
	}
}

static bool nl_open(struct nl *nl) {
	*nl = (struct nl){ .fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC) };
	if (nl->fd < 0) {
		return false;
	}
	struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 }; // never hold the taskbar up
	setsockopt(nl->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	if (bind(nl->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			!nl_send(nl, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 0, false) ||
			!nl_receive(nl, family_handler, &nl->family) || nl->family <= 0) {
		close(nl->fd);
		return false;
	}
	return true;
}

/* ---------- capabilities ---------- */

/* The spatial streams of a map of 2 bits a stream, 3 meaning none. */
static int streams_of_map(uint16_t map) {
	int streams = 0;
	for (int s = 0; s < 8; s++) {
		if (((map >> (2 * s)) & 3) != 3) {
			streams = s + 1;
		}
	}
	return streams;
}

/* The spatial streams of an 802.11n MCS set: a byte of 8 MCS each. */
static int streams_of_ht(const uint8_t *mcs) {
	int streams = 0;
	for (int s = 0; s < 4; s++) {
		if (mcs[s]) {
			streams = s + 1;
		}
	}
	return streams;
}

/* A better standard found: its streams and width replace the older ones. */
static void raise_caps(struct wifi_link_caps *c, int gen, int streams, int width) {
	if (gen > c->gen) {
		c->gen = gen;
		if (streams > 0) {
			c->streams = streams;
		}
	} else if (gen == c->gen && streams > c->streams) {
		c->streams = streams;
	}
	if (width > c->width) {
		c->width = width;
	}
	if (c->streams < 1) {
		c->streams = 1;
	}
}

/* The widths of an 802.11ax PHY capability: bits 1 to 3 of its first byte. */
static int he_width(uint8_t phy0, enum wifi_band band) {
	if (band == WIFI_BAND_2G) {
		return phy0 & 0x02 ? 40 : 20;
	}
	return phy0 & 0x08 ? 160 : phy0 & 0x04 ? 80 : 20;
}

struct wiphy_ctx {
	struct wifi_adapter *out;
};

static void iftype_data(struct nlattr *list, struct wifi_link_caps *c, enum wifi_band band) {
	FOR_ATTRS(entry, attr_data(list), attr_len(list)) {
		bool station = false;
		int he_streams = 0, width = 0, gen = 0;
		FOR_ATTRS(a, attr_data(entry), attr_len(entry)) {
			switch (attr_type(a)) {
			case NL80211_BAND_IFTYPE_ATTR_IFTYPES:
				FOR_ATTRS(t, attr_data(a), attr_len(a)) {
					station |= attr_type(t) == NL80211_IFTYPE_STATION;
				}
				break;
			case NL80211_BAND_IFTYPE_ATTR_HE_CAP_PHY:
				if (attr_len(a) >= 1) {
					gen = gen > 6 ? gen : 6;
					width = he_width(*(uint8_t *)attr_data(a), band);
				}
				break;
			case NL80211_BAND_IFTYPE_ATTR_HE_CAP_MCS_SET:
				if (attr_len(a) >= 2) {
					uint16_t map;
					memcpy(&map, attr_data(a), 2);
					he_streams = streams_of_map(map);
				}
				break;
			case NL80211_BAND_IFTYPE_ATTR_EHT_CAP_PHY:
				if (attr_len(a) >= 1) {
					gen = 7;
					if (band == WIFI_BAND_6G && (*(uint8_t *)attr_data(a) & 0x02)) {
						width = 320;
					}
				}
				break;
			}
		}
		if (station && gen) {
			raise_caps(c, gen, he_streams, width);
		}
	}
}

static void wiphy_handler(struct nlattr *attrs, int len, void *data) {
	struct wiphy_ctx *ctx = data;
	FOR_ATTRS(a, attrs, len) {
		if (attr_type(a) != NL80211_ATTR_WIPHY_BANDS) {
			continue;
		}
		FOR_ATTRS(b, attr_data(a), attr_len(a)) {
			int index = attr_type(b);
			enum wifi_band band = index == NL80211_BAND_2GHZ ? WIFI_BAND_2G :
				index == NL80211_BAND_5GHZ ? WIFI_BAND_5G :
				index == NL80211_BAND_6GHZ ? WIFI_BAND_6G : WIFI_BANDS;
			if (band == WIFI_BANDS) {
				continue; // 60 GHz and others
			}
			struct wifi_link_caps *c = &ctx->out->band[band];
			FOR_ATTRS(p, attr_data(b), attr_len(b)) {
				switch (attr_type(p)) {
				case NL80211_BAND_ATTR_FREQS:
					if (!c->gen) {
						c->gen = 3; // the band is there, at least a or g
						c->streams = 1;
						c->width = 20;
					}
					break;
				case NL80211_BAND_ATTR_HT_MCS_SET:
					if (attr_len(p) >= 4) {
						raise_caps(c, 4, streams_of_ht(attr_data(p)), 0);
					}
					break;
				case NL80211_BAND_ATTR_HT_CAPA:
					if (attr_len(p) >= 2) {
						uint16_t capa;
						memcpy(&capa, attr_data(p), 2);
						raise_caps(c, 4, 0, capa & 0x02 ? 40 : 20);
					}
					break;
				case NL80211_BAND_ATTR_VHT_MCS_SET:
					if (attr_len(p) >= 2) {
						uint16_t map;
						memcpy(&map, attr_data(p), 2);
						raise_caps(c, 5, streams_of_map(map), 80);
					}
					break;
				case NL80211_BAND_ATTR_VHT_CAPA:
					if (attr_len(p) >= 4) {
						uint32_t capa;
						memcpy(&capa, attr_data(p), 4);
						raise_caps(c, 5, 0, (capa >> 2) & 3 ? 160 : 80);
					}
					break;
				case NL80211_BAND_ATTR_IFTYPE_DATA:
					iftype_data(p, c, band);
					break;
				}
			}
			ctx->out->known = true;
		}
	}
}

bool wifi_adapter_caps(const char *ifname, struct wifi_adapter *out) {
	memset(out, 0, sizeof(*out));
	int ifindex = (int)if_nametoindex(ifname);
	struct nl nl;
	if (!ifindex || !nl_open(&nl)) {
		return false;
	}
	struct wiphy_ctx ctx = { out };
	if (nl_send(&nl, nl.family, NLM_F_DUMP, NL80211_CMD_GET_WIPHY, ifindex, true)) {
		nl_receive(&nl, wiphy_handler, &ctx);
	}
	close(nl.fd);
	return out->known;
}

/* ---------- access points ---------- */

/* The capabilities an access point announces in its information elements. */
static void parse_ies(const uint8_t *ie, int len, int freq, struct wifi_link_caps *c) {
	enum wifi_band band = wifi_band_of(freq);
	*c = (struct wifi_link_caps){ 3, 1, 20 };
	for (int i = 0; i + 2 <= len && i + 2 + ie[i + 1] <= len; i += 2 + ie[i + 1]) {
		int id = ie[i], n = ie[i + 1];
		const uint8_t *d = ie + i + 2;
		if (id == 45 && n >= 7) { // HT capabilities
			raise_caps(c, 4, streams_of_ht(d + 3), d[0] & 0x02 ? 40 : 20);
		} else if (id == 191 && n >= 6) { // VHT capabilities
			uint32_t capa = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
			uint16_t map = d[4] | d[5] << 8;
			raise_caps(c, 5, streams_of_map(map), (capa >> 2) & 3 ? 160 : 80);
		} else if (id == 255 && n >= 1) { // an extension
			if (d[0] == 35 && n >= 1 + 6 + 11 + 2) { // HE capabilities
				uint16_t map = d[18] | d[19] << 8;
				raise_caps(c, 6, streams_of_map(map), he_width(d[7], band));
			} else if (d[0] == 108 && n >= 1 + 2 + 9) { // EHT capabilities
				raise_caps(c, 7, 0, band == WIFI_BAND_6G && (d[3] & 0x02) ? 320 : 0);
			}
		}
	}
	if (band == WIFI_BAND_2G && c->gen == 5) {
		c->gen = 4;
	}
}

struct scan_ctx {
	struct wifi_ap *aps;
	int count, cap;
};

static void scan_handler(struct nlattr *attrs, int len, void *data) {
	struct scan_ctx *ctx = data;
	FOR_ATTRS(a, attrs, len) {
		if (attr_type(a) != NL80211_ATTR_BSS) {
			continue;
		}
		struct wifi_ap ap = { 0 };
		const uint8_t *ies = NULL;
		int ies_len = 0;
		bool have_bssid = false;
		FOR_ATTRS(b, attr_data(a), attr_len(a)) {
			switch (attr_type(b)) {
			case NL80211_BSS_BSSID:
				if (attr_len(b) >= 6) {
					memcpy(ap.bssid, attr_data(b), 6);
					have_bssid = true;
				}
				break;
			case NL80211_BSS_FREQUENCY:
				if (attr_len(b) >= 4) {
					memcpy(&ap.freq, attr_data(b), 4);
				}
				break;
			case NL80211_BSS_INFORMATION_ELEMENTS:
				ies = attr_data(b);
				ies_len = attr_len(b);
				break;
			case NL80211_BSS_BEACON_IES:
				if (!ies) {
					ies = attr_data(b);
					ies_len = attr_len(b);
				}
				break;
			}
		}
		if (!have_bssid || !ies) {
			continue;
		}
		parse_ies(ies, ies_len, ap.freq, &ap.caps);
		if (ctx->count == ctx->cap) {
			ctx->cap = ctx->cap ? ctx->cap * 2 : 32;
			struct wifi_ap *grown = realloc(ctx->aps, ctx->cap * sizeof(*grown));
			if (!grown) {
				return;
			}
			ctx->aps = grown;
		}
		ctx->aps[ctx->count++] = ap;
	}
}

int wifi_scan_aps(const char *ifname, struct wifi_ap **out) {
	*out = NULL;
	int ifindex = (int)if_nametoindex(ifname);
	struct nl nl;
	if (!ifindex || !nl_open(&nl)) {
		return 0;
	}
	struct scan_ctx ctx = { 0 };
	if (nl_send(&nl, nl.family, NLM_F_DUMP, NL80211_CMD_GET_SCAN, ifindex, false)) {
		nl_receive(&nl, scan_handler, &ctx);
	}
	close(nl.fd);
	*out = ctx.aps;
	return ctx.count;
}

/* ---------- the link now ---------- */

int wifi_channel(int freq) {
	if (freq == 2484) {
		return 14;
	}
	if (freq >= 5955) {
		return (freq - 5950) / 5;
	}
	if (freq >= 4900) {
		return (freq - 5000) / 5;
	}
	return (freq - 2407) / 5;
}

static int width_mhz(uint32_t width) {
	switch (width) {
	case NL80211_CHAN_WIDTH_40: return 40;
	case NL80211_CHAN_WIDTH_80: return 80;
	case NL80211_CHAN_WIDTH_80P80:
	case NL80211_CHAN_WIDTH_160: return 160;
	case NL80211_CHAN_WIDTH_320: return 320;
	default: return 20;
	}
}

static void interface_handler(struct nlattr *attrs, int len, void *data) {
	struct wifi_link *link = data;
	FOR_ATTRS(a, attrs, len) {
		if (attr_type(a) == NL80211_ATTR_WIPHY_FREQ && attr_len(a) >= 4) {
			memcpy(&link->freq, attr_data(a), 4);
		} else if (attr_type(a) == NL80211_ATTR_CHANNEL_WIDTH && attr_len(a) >= 4) {
			uint32_t w;
			memcpy(&w, attr_data(a), 4);
			link->width = width_mhz(w);
		}
	}
}

/* A rate of NL80211_STA_INFO_[TR]X_BITRATE, in Mbit/s. */
static double bitrate(struct nlattr *rate) {
	double mbit = 0;
	FOR_ATTRS(r, attr_data(rate), attr_len(rate)) {
		if (attr_type(r) == NL80211_RATE_INFO_BITRATE32 && attr_len(r) >= 4) {
			uint32_t v;
			memcpy(&v, attr_data(r), 4);
			mbit = v / 10.0; // in 100 kbit/s
		} else if (attr_type(r) == NL80211_RATE_INFO_BITRATE && attr_len(r) >= 2 && mbit == 0) {
			uint16_t v;
			memcpy(&v, attr_data(r), 2);
			mbit = v / 10.0;
		}
	}
	return mbit;
}

static void station_handler(struct nlattr *attrs, int len, void *data) {
	struct wifi_link *link = data;
	FOR_ATTRS(a, attrs, len) {
		if (attr_type(a) != NL80211_ATTR_STA_INFO) {
			continue;
		}
		FOR_ATTRS(i, attr_data(a), attr_len(a)) {
			if (attr_type(i) == NL80211_STA_INFO_TX_BITRATE) {
				link->tx = bitrate(i);
			} else if (attr_type(i) == NL80211_STA_INFO_RX_BITRATE) {
				link->rx = bitrate(i);
			}
		}
	}
}

bool wifi_link_info(const char *ifname, struct wifi_link *out) {
	memset(out, 0, sizeof(*out));
	int ifindex = (int)if_nametoindex(ifname);
	struct nl nl;
	if (!ifindex || !nl_open(&nl)) {
		return false;
	}
	if (nl_send(&nl, nl.family, 0, NL80211_CMD_GET_INTERFACE, ifindex, false)) {
		nl_receive(&nl, interface_handler, out);
	}
	// the station of a client is the access point it is connected to
	if (out->freq && nl_send(&nl, nl.family, NLM_F_DUMP, NL80211_CMD_GET_STATION, ifindex,
			false)) {
		nl_receive(&nl, station_handler, out);
	}
	close(nl.fd);
	return out->freq > 0 && (out->tx > 0 || out->rx > 0);
}
