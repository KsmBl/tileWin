#include <stdio.h>
#include <string.h>
#include "wire.h"

size_t dns_skip_name(const uint8_t *msg, size_t len, size_t off) {
	for (int labels = 0; labels < 128; labels++) {
		if (off >= len) {
			return 0;
		}
		uint8_t b = msg[off];
		if (b == 0) {
			return off + 1;
		}
		if ((b & 0xc0) == 0xc0) {
			return off + 2 <= len ? off + 2 : 0;
		}
		if (b & 0xc0) {
			return 0;
		}
		off += 1 + b;
	}
	return 0;
}

bool dns_parse_question(const uint8_t *msg, size_t len, struct dns_question *q) {
	if (len < DNS_HEADER || dns_u16(msg + 4) != 1) {
		return false;
	}
	size_t off = DNS_HEADER, out = 0, wire = 0;
	for (;;) {
		if (off >= len) {
			return false;
		}
		uint8_t n = msg[off++];
		if (n == 0) {
			break;
		}
		if (n & 0xc0 || off + n > len || (wire += n + 1) > 255) {
			return false; // a question is never compressed
		}
		if (out > 0) {
			q->name[out++] = '.';
		}
		for (uint8_t i = 0; i < n; i++) {
			uint8_t c = msg[off + i];
			if (c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';
			}
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
				q->name[out++] = c;
			} else {
				out += snprintf(q->name + out, 5, "\\%03u", c);
			}
		}
		off += n;
	}
	if (out == 0) {
		q->name[out++] = '.';
	}
	q->name[out] = '\0';
	if (off + 4 > len) {
		return false;
	}
	q->type = dns_u16(msg + off);
	q->qclass = dns_u16(msg + off + 2);
	q->end = off + 4;
	return true;
}

size_t dns_encode_name(const char *name, uint8_t *out, size_t cap) {
	size_t pos = 0;
	if (strcmp(name, ".") != 0) {
		const char *p = name;
		while (*p) {
			if (pos + 1 >= cap) {
				return 0;
			}
			size_t len_at = pos++;
			uint8_t n = 0;
			while (*p && *p != '.') {
				uint8_t c = (uint8_t)*p++;
				if (c == '\\' && p[0] >= '0' && p[0] <= '9' && p[1] && p[2]) {
					c = (uint8_t)((p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0'));
					p += 3;
				}
				if (n == 63 || pos + 1 >= cap) {
					return 0;
				}
				out[pos++] = c;
				n++;
			}
			if (n == 0) {
				return 0;
			}
			out[len_at] = n;
			if (*p == '.') {
				p++;
			}
		}
	}
	if (pos + 1 > cap || pos + 1 > 255) {
		return 0;
	}
	out[pos++] = 0;
	return pos;
}

/*
 * Walks the records after the question. For each it calls back with the
 * offset of the record's type; the TTL is at +4, the data length at +8 and the
 * data at +10. The section is 0 for answers, 1 for authority, 2 additional.
 */
static bool walk_records(uint8_t *msg, size_t len,
		bool (*each)(uint8_t *msg, size_t at, int section, void *data), void *data) {
	struct dns_question q;
	if (!dns_parse_question(msg, len, &q)) {
		return false;
	}
	size_t off = q.end;
	int counts[3] = { dns_u16(msg + 6), dns_u16(msg + 8), dns_u16(msg + 10) };
	for (int section = 0; section < 3; section++) {
		for (int i = 0; i < counts[section]; i++) {
			off = dns_skip_name(msg, len, off);
			if (off == 0 || off + 10 > len) {
				return false;
			}
			size_t end = off + 10 + dns_u16(msg + off + 8);
			if (end > len) {
				return false;
			}
			if (each && !each(msg, off, section, data)) {
				return true;
			}
			off = end;
		}
	}
	return true;
}

static bool find_opt(uint8_t *msg, size_t at, int section, void *data) {
	if (section == 2 && dns_u16(msg + at) == DNS_TYPE_OPT) {
		*(uint16_t *)data = dns_u16(msg + at + 2);
		return false;
	}
	return true;
}

uint16_t dns_udp_size(const uint8_t *msg, size_t len) {
	uint16_t size = 0;
	walk_records((uint8_t *)msg, len, find_opt, &size);
	return size < 512 ? 512 : size;
}

struct ttl_scan {
	bool negative, found;
	uint32_t ttl;
};

static bool shortest_ttl(uint8_t *msg, size_t at, int section, void *data) {
	struct ttl_scan *s = data;
	uint16_t type = dns_u16(msg + at);
	uint32_t ttl = dns_u32(msg + at + 4);
	if (s->negative) {
		// the SOA of the zone says how long "nothing" stays true
		if (section == 1 && type == DNS_TYPE_SOA && dns_u16(msg + at + 8) >= 22) {
			uint32_t minimum = dns_u32(msg + at + 10 + dns_u16(msg + at + 8) - 4);
			s->ttl = ttl < minimum ? ttl : minimum;
			s->found = true;
			return false;
		}
	} else if (section == 0 && type != DNS_TYPE_OPT) {
		if (!s->found || ttl < s->ttl) {
			s->ttl = ttl;
		}
		s->found = true;
	}
	return true;
}

bool dns_cache_ttl(const uint8_t *msg, size_t len, uint32_t *ttl) {
	if (len < DNS_HEADER || dns_truncated(msg)) {
		return false;
	}
	int rcode = dns_rcode(msg);
	if (rcode != DNS_RCODE_NOERROR && rcode != DNS_RCODE_NXDOMAIN) {
		return false;
	}
	struct ttl_scan s = {
		.negative = rcode == DNS_RCODE_NXDOMAIN || dns_u16(msg + 6) == 0,
	};
	if (!walk_records((uint8_t *)msg, len, shortest_ttl, &s) || !s.found) {
		return false;
	}
	*ttl = s.ttl;
	return true;
}

static bool cap_ttl(uint8_t *msg, size_t at, int section, void *data) {
	uint32_t cap = *(uint32_t *)data;
	if (dns_u16(msg + at) != DNS_TYPE_OPT && dns_u32(msg + at + 4) > cap) {
		dns_put32(msg + at + 4, cap);
	}
	return true;
}

void dns_cap_ttls(uint8_t *msg, size_t len, uint32_t cap) {
	walk_records(msg, len, cap_ttl, &cap);
}

size_t dns_build_query(uint8_t *out, size_t cap, uint16_t id, const char *name, uint16_t type) {
	if (cap < DNS_HEADER + 4 + 11) {
		return 0;
	}
	memset(out, 0, DNS_HEADER);
	dns_put16(out, id);
	out[2] = 0x01; // recursion desired
	dns_put16(out + 4, 1);
	dns_put16(out + 10, 1);
	size_t n = dns_encode_name(name, out + DNS_HEADER, cap - DNS_HEADER - 4 - 11);
	if (n == 0) {
		return 0;
	}
	size_t off = DNS_HEADER + n;
	dns_put16(out + off, type);
	dns_put16(out + off + 2, 1);
	off += 4;
	// OPT: root name, type 41, UDP size 1232, no flags, no options
	memset(out + off, 0, 11);
	dns_put16(out + off + 1, DNS_TYPE_OPT);
	dns_put16(out + off + 3, 1232);
	return off + 11;
}

size_t dns_build_reply(uint8_t *out, size_t cap, const uint8_t *query,
		const struct dns_question *q, int rcode, bool blocked_address) {
	if (q->end + 28 > cap) {
		return 0;
	}
	memcpy(out, query, q->end);
	out[2] = 0x80 | (query[2] & 0x79); // a response, its opcode and RD kept
	out[3] = 0x80 | (rcode & 0x0f);     // recursion available
	dns_put16(out + 4, 1);
	memset(out + 6, 0, 6);
	size_t off = q->end;
	if (blocked_address && rcode == DNS_RCODE_NOERROR && q->qclass == 1 &&
			(q->type == DNS_TYPE_A || q->type == DNS_TYPE_AAAA)) {
		uint16_t size = q->type == DNS_TYPE_A ? 4 : 16;
		dns_put16(out + 6, 1);
		dns_put16(out + off, 0xc00c); // the name of the question
		dns_put16(out + off + 2, q->type);
		dns_put16(out + off + 4, 1);
		dns_put32(out + off + 6, 2);
		dns_put16(out + off + 10, size);
		memset(out + off + 12, 0, size);
		off += 12 + size;
	}
	return off;
}

size_t dns_truncate(uint8_t *msg, size_t len, const struct dns_question *q) {
	if (len < q->end) {
		return len;
	}
	msg[2] |= 0x02;
	memset(msg + 6, 0, 6);
	return q->end;
}

bool dns_name_within(const char *name, const char *domain) {
	size_t n = strlen(name), d = strlen(domain);
	if (n == d) {
		return strcmp(name, domain) == 0;
	}
	return n > d && name[n - d - 1] == '.' && strcmp(name + n - d, domain) == 0;
}
