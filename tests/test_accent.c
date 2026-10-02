/*
 * The accent color: picked from a picture as the hue most of its colorful
 * pixels share, and "$accent" in a theme following the wallpaper when asked.
 * Run with TILEWIN_DATADIR at the source tree.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_accent.h"
#include "tw_theme.h"

static int failures;

static void fail(const char *what, uint32_t got) {
	fprintf(stderr, "FAIL: %s (got #%08x)\n", what, got);
	failures++;
}

/* A w x h picture of premultiplied ARGB in one color, part of it in another. */
static uint32_t *picture(int w, int h, uint32_t rgb, uint32_t other, int other_pixels) {
	uint32_t *p = malloc(sizeof(uint32_t) * w * h);
	for (int i = 0; i < w * h; i++) {
		p[i] = 0xff000000 | (i < other_pixels ? other : rgb);
	}
	return p;
}

static int channel(uint32_t c, int shift) {
	return (c >> shift) & 0xff;
}

int main(void) {
	uint32_t c = 0;
	// a red picture: red, dark enough for white text
	uint32_t *red = picture(200, 100, 0xe03020, 0, 0);
	if (!tw_accent_pick((unsigned char *)red, 200, 100, 800, &c) ||
			channel(c, 24) < 2 * channel(c, 16) || channel(c, 24) < 2 * channel(c, 8)) {
		fail("a red picture gives no red accent", c);
	}
	// greys carry no color
	uint32_t *grey = picture(200, 100, 0x808080, 0xf0f0f0, 5000);
	if (tw_accent_pick((unsigned char *)grey, 200, 100, 800, &c)) {
		fail("a grey picture gives an accent", c);
	}
	// a wide blue sky over a few red specks: blue
	uint32_t *sky = picture(200, 100, 0x2060d0, 0xd02020, 1500);
	if (!tw_accent_pick((unsigned char *)sky, 200, 100, 800, &c) ||
			channel(c, 8) <= channel(c, 24)) {
		fail("a mostly blue picture does not give blue", c);
	}
	free(red);
	free(grey);
	free(sky);

	// "$accent" in Windows 10: its own blue, then the wallpaper's color
	char dir[] = "/tmp/tw-accent-XXXXXX";
	if (!mkdtemp(dir)) {
		return 1;
	}
	char config[64], cache[64];
	snprintf(config, sizeof(config), "%s/config", dir);
	snprintf(cache, sizeof(cache), "%s/cache", dir);
	setenv("XDG_CONFIG_HOME", config, 1);
	setenv("XDG_CACHE_HOME", cache, 1);
	tw_accent_reload();
	char *error = NULL;
	struct tw_theme *t = tw_theme_load("win10", &error);
	if (!t) {
		fprintf(stderr, "FAIL: win10 does not load (%s)\n", error ? error : "");
		return 1;
	}
	if ((c = tw_theme_color(t, "flyout.accent", 0)) != 0x0078d7ff) {
		fail("the accent of Windows 10 is not its blue", c);
	}
	tw_accent_store(0xb03514ff);
	if ((c = tw_theme_color(t, "flyout.accent", 0)) != 0x0078d7ff) {
		fail("the wallpaper's accent is used without being asked for", c);
	}
	tw_accent_set_from_wallpaper(true);
	if ((c = tw_theme_color(t, "flyout.accent", 0)) != 0xb03514ff) {
		fail("the accent does not follow the wallpaper", c);
	}
	if ((c = tw_theme_color(t, "decoration.active.frame", 0)) != 0xb03514d0) {
		fail("$accent/d0 does not keep its alpha", c);
	}
	c = tw_theme_color(t, "taskbar.indicator", 0);
	if (channel(c, 24) <= 0xb0 || channel(c, 8) <= 0x14) {
		fail("$accent_light is not lighter", c);
	}
	tw_theme_free(t);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd) != 0) {
		fprintf(stderr, "could not remove %s\n", dir);
	}
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("accent colors as they should be\n");
	return 0;
}
