#ifndef _TW_WIFIRADAR_H
#define _TW_WIFIRADAR_H
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include "panel.h"

/*
 * The Wi-Fi radar: the networks in reach as contacts on a sensor whose sweep
 * goes round, the stronger the nearer the middle. One scan serves every radar
 * there is (the Wi-Fi radar widget on the desktop or the taskbar, the moving
 * wallpaper of the Starship Console); each draws it in its own colors.
 */

#define RADAR_MAX 64

struct radar_net {
	char ssid[96], security[48], uuid[48];
	int signal, freq;
	bool active, known, secured;
	bool autoconnect; // a saved one: NetworkManager connects to it by itself
	double angle;               // radians clockwise from the top, from the name
	double r_from, r_to, moved; // share of the radius, eased from r_from to r_to since moved
	double appeared, gone;      // when it came, when it left (0 while it is there)
};

enum radar_status {
	RADAR_LOOKING,   // no answer yet
	RADAR_READY,
	RADAR_OFF,       // Wi-Fi is switched off
	RADAR_NO_DEVICE, // no Wi-Fi adapter
	RADAR_NO_NM,     // no NetworkManager (nmcli)
};

/* The colors and manners a radar is drawn in. */
struct radar_look {
	uint32_t disc;          // the screen behind the rings, 0 for none
	uint32_t ring, sweep;
	uint32_t fg, dim;
	uint32_t blip, linked, bad;
	uint32_t card_bg, card_fg, card_border;
	char family[160];       // font family, without a size
	bool upper;             // names in capitals (the Starship Console)
	bool bearings;          // 000 090 180 270 around the outside
	bool shadow;            // text gets a shadow (straight on the wallpaper)
	bool mono;              // ink on paper (the Atari ST): hatching, no shades
	bool bluetooth;         // Bluetooth devices too, as diamonds
	double scale;           // 1 for text at 100 pixel cells
	double turn;            // seconds for a round of the sweep
	double cards_x, cards_y, cards_w, cards_h; // where the card of a contact may go
};

/* Where a radar is: its middle and the radius of the outer ring. */
struct radar_geom {
	double cx, cy, R;
};

/* While anything holds the radar it scans; the last release stops it. */
void wifiradar_hold(struct panel *panel);
void wifiradar_release(void);
/* Looks again soon (after connecting, a click on the empty radar). */
void wifiradar_rescan(void);
/* fn is called when what the radar knows changed (a scan came back, a connection). */
void wifiradar_listen(void (*fn)(void *data), void *data);
void wifiradar_unlisten(void (*fn)(void *data), void *data);

enum radar_status wifiradar_status(void);
/* The networks, including those fading out after they left. */
int wifiradar_slots(void);
const struct radar_net *wifiradar_slot(int i);
/* How many are in reach now, and the one connected to (NULL for none). */
int wifiradar_count(void);
const char *wifiradar_linked(void);
/* The network being connected to, by the radar or the network flyout; NULL for none. */
const char *wifiradar_connecting(void);
/* Faster frames while something happens: connecting, a ping, a contact moving. */
bool wifiradar_busy(void);

/* The radar in a box: as big as fits, room left around it for the ticks. */
void wifiradar_geom(double x, double y, double w, double h, const struct radar_look *look,
	struct radar_geom *g);
void wifiradar_draw(cairo_t *cr, const struct radar_geom *g, const struct radar_look *look,
	int hover);
/*
 * The contact at a point, -1 for none: a Wi-Fi network below RADAR_MAX, a
 * Bluetooth device RADAR_MAX and up.
 */
int wifiradar_hit(const struct radar_geom *g, const struct radar_look *look, double x,
	double y);
bool wifiradar_in_disc(const struct radar_geom *g, double x, double y);
/*
 * A click: on a contact connects to it (a saved or open network at once, a
 * new one with a password through the network flyout, the one connected to
 * opens its details there); on the empty radar sends a ping and looks again,
 * or switches Wi-Fi on when it is off.
 */
void wifiradar_click(const struct radar_geom *g, const struct radar_look *look, double x,
	double y, struct popup_anchor anchor);
/* The menu of the contact at a point (right click); false when there is none there. */
bool wifiradar_menu(const struct radar_geom *g, const struct radar_look *look, double x,
	double y, struct popup_anchor anchor);
/* "panel radar <connect|disconnect|forget|autoconnect-on|autoconnect-off|details|rescan> [hex ssid]" */
void wifiradar_command(struct panel *panel, int argc, char **argv);
/* Small radar for the taskbar in a size x size square, with the strongest contacts. */
void wifiradar_draw_icon(cairo_t *cr, double x, double y, double size, uint32_t fg,
	uint32_t sweep, bool square);

#endif
