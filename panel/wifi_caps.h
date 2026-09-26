#ifndef _TW_WIFI_CAPS_H
#define _TW_WIFI_CAPS_H
#include <stdbool.h>
#include <stdint.h>

/*
 * What a Wi-Fi link can reach at most: the adapter's capabilities and those of
 * each access point, from the kernel (nl80211, no privileges needed), and the
 * speed the two can do together: the older standard of the two, the fewer
 * spatial streams and the narrower channel, at the best modulation that allows.
 */

enum wifi_band { WIFI_BAND_2G, WIFI_BAND_5G, WIFI_BAND_6G, WIFI_BANDS };

/* 4 = 802.11n (Wi-Fi 4), 5 = ac, 6 = ax, 7 = be; 3 for the older ones. */
struct wifi_link_caps {
	int gen;
	int streams;
	int width; // MHz, the widest channel supported
};

struct wifi_adapter {
	bool known;                           // the kernel told
	struct wifi_link_caps band[WIFI_BANDS]; // gen 0: the band is not supported
};

struct wifi_ap {
	uint8_t bssid[6];
	int freq;
	struct wifi_link_caps caps;
};

enum wifi_band wifi_band_of(int freq_mhz);

/* The adapter behind a network interface; false when the kernel cannot be asked. */
bool wifi_adapter_caps(const char *ifname, struct wifi_adapter *out);

/* The access points of the last scan of that interface; the count, *out to free(). */
int wifi_scan_aps(const char *ifname, struct wifi_ap **out);

/*
 * The highest data rate in Mbit/s of a link of that standard, streams and
 * channel width; and what an adapter and an access point reach together on a
 * channel of width MHz (0: as wide as both can), filling *pair when not NULL.
 */
double wifi_phy_rate(int gen, int streams, int width);
double wifi_pair_rate(const struct wifi_adapter *adapter, const struct wifi_ap *ap, int width,
	struct wifi_link_caps *pair);

/* The link a connected interface has now. */
struct wifi_link {
	int freq;          // MHz of the channel
	int width;         // MHz wide
	double tx, rx;     // Mbit/s it sends and receives at, 0 when not known
};

/* The channel and the current rates of a connected interface; false when not connected. */
bool wifi_link_info(const char *ifname, struct wifi_link *out);

/* The channel number of a frequency in MHz. */
int wifi_channel(int freq);

/* "aa:bb:cc:dd:ee:ff" to bytes. */
bool wifi_parse_bssid(const char *text, uint8_t out[6]);

#endif
