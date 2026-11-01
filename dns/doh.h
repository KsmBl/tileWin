#ifndef _TILEWIN_DNS_DOH_H
#define _TILEWIN_DNS_DOH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * DNS over HTTPS (RFC 8484): a question POSTed as application/dns-message to
 * an address like https://cloudflare-dns.com/dns-query, over HTTP/2 where the
 * server speaks it (some speak nothing else), through libcurl. The connection
 * goes to the address of the server as it is in the config, so no name has to
 * be looked up first, and the certificate has to be valid for the host name
 * of the URL. Connections are kept open and used again.
 */

#define DOH_URL_MAX 200

/* Before any thread asks: sets libcurl up. */
void doh_init(void);

/* Whether a URL is one this can ask: https://host[:port]/path. */
bool doh_url_valid(const char *url);

/*
 * The CA certificates to check servers against, instead of the system's (for
 * the tests). Before the first question.
 */
void doh_set_ca_file(const char *path);

/*
 * Asks the question (a DNS message of qlen bytes) and waits for the answer,
 * at most a few seconds. Returns its length and stores it, newly allocated, in
 * *answer; 0 and why in error when there is none. Safe from many threads.
 */
size_t doh_ask(const struct sockaddr_storage *addr, socklen_t addrlen, const char *url,
	const uint8_t *query, size_t qlen, uint8_t **answer, char *error, size_t error_size);

#endif
