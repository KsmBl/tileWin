#ifndef _TW_DNS_WIRE_H
#define _TW_DNS_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The parts of a DNS message (RFC 1035) the DNS service of tileWin looks at.
 * Names are kept as text: lower case, without the final dot, and with every
 * byte that is not a letter, digit, '-' or '_' written as \DDD, so a name is
 * also a safe word in a file. The root is ".".
 */

#define DNS_HEADER 12
#define DNS_NAME_MAX 1025 // 255 bytes, each \DDD at worst, and the end

#define DNS_TYPE_A 1
#define DNS_TYPE_NS 2
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_SOA 6
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_OPT 41

#define DNS_RCODE_NOERROR 0
#define DNS_RCODE_SERVFAIL 2
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_REFUSED 5

struct dns_question {
	char name[DNS_NAME_MAX];
	uint16_t type, qclass;
	size_t end; // where the question ends in the message
};

static inline uint16_t dns_u16(const uint8_t *p) {
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t dns_u32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void dns_put16(uint8_t *p, uint16_t v) {
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

static inline void dns_put32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16 & 0xff;
	p[2] = v >> 8 & 0xff;
	p[3] = v & 0xff;
}

static inline int dns_rcode(const uint8_t *msg) {
	return msg[3] & 0x0f;
}

static inline bool dns_truncated(const uint8_t *msg) {
	return msg[2] & 0x02;
}

static inline bool dns_is_response(const uint8_t *msg) {
	return msg[2] & 0x80;
}

/* The offset after the name at off, following no pointers; 0 if broken. */
size_t dns_skip_name(const uint8_t *msg, size_t len, size_t off);
/* The one question of a query or its answer. False if there is not exactly one. */
bool dns_parse_question(const uint8_t *msg, size_t len, struct dns_question *q);
/* Turns a name as text back into labels; the length written, 0 if it does not fit. */
size_t dns_encode_name(const char *name, uint8_t *out, size_t cap);
/* The UDP size the asker can take: from its OPT record, else 512. */
uint16_t dns_udp_size(const uint8_t *msg, size_t len);
/*
 * How long an answer may be kept, in seconds: the shortest TTL of its answers,
 * or for "no such name" and "no data" the SOA's negative TTL. False when it
 * should not be kept at all (a failure, or nothing that says how long).
 */
bool dns_cache_ttl(const uint8_t *msg, size_t len, uint32_t *ttl);
/* Lowers every TTL in the message (but the OPT's) to at most cap. */
void dns_cap_ttls(uint8_t *msg, size_t len, uint32_t cap);
/* A query for name, with an OPT record that takes 1232 bytes over UDP. */
size_t dns_build_query(uint8_t *out, size_t cap, uint16_t id, const char *name, uint16_t type);
/*
 * The answer to a query by the service itself: an error (rcode), or for a
 * blocked name 0.0.0.0 or :: (or no data for other types) with a short TTL.
 */
size_t dns_build_reply(uint8_t *out, size_t cap, const uint8_t *query,
	const struct dns_question *q, int rcode, bool blocked_address);
/* Cuts an answer that is too big for UDP down to its question, with TC set. */
size_t dns_truncate(uint8_t *msg, size_t len, const struct dns_question *q);
/* Whether name is domain or below it ("a.b.example.com" is below "example.com"). */
bool dns_name_within(const char *name, const char *domain);

#endif
