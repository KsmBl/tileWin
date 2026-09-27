#include <stdlib.h>
#include "saver_util.h"

/*
 * Doomsday: the end of the desktop, in the kind picked. Hellfire burns the
 * windows down, Thunderstorm drowns them in rain and lightning, Blizzard
 * buries them in snow and ice. Each kind is a saver of its own, run inside
 * this one with the scaling and clearing it wants; their settings are
 * Doomsday's, "doomsday_<kind>_<setting>", shown for the kind picked.
 */

const struct saver *const doomsday_kinds[] = {
	&saver_hellfire,
	&saver_storm,
	&saver_blizzard,
};

const int doomsday_kind_count = sizeof(doomsday_kinds) / sizeof(doomsday_kinds[0]);

static void *doomsday_create(int width, int height, const struct saver_options *options) {
	const struct saver *kind = doomsday_kinds[saver_choice(options, &saver_doomsday, "kind")];
	// Doomsday is sped up already: the kind runs at its own pace within it
	struct saver_options inner = *options;
	inner.speed = 1;
	inner.show_stats = false; // shown once, by Doomsday's own run
	return saver_run_new(kind, width, height, &inner);
}

static void doomsday_draw(void *state, cairo_t *cr, int width, int height, double dt) {
	saver_run_draw(state, cr, dt);
}

static void doomsday_destroy(void *state) {
	saver_run_free(state);
}

static const char *const kind_values[] = { "hellfire", "thunderstorm", "blizzard", NULL };
static const char *const kind_labels[] = { "Hellfire", "Thunderstorm", "Blizzard", NULL };
static const char *const scraps_values[] = { "some", "few", "many", NULL };
static const char *const scraps_labels[] = { "Some", "Few", "Many", NULL };
static const char *const rain_values[] = { "heavy", "light", "downpour", NULL };
static const char *const rain_labels[] = { "Heavy", "Light", "Downpour", NULL };
static const char *const bolt_values[] = { "sometimes", "often", "rarely", NULL };
static const char *const bolt_labels[] = { "Now and then", "Often", "Rarely", NULL };
static const char *const snow_values[] = { "heavy", "light", "whiteout", NULL };
static const char *const snow_labels[] = { "Heavy", "Light", "Whiteout", NULL };

static const struct saver_option doomsday_options[] = {
	{ "kind", "Doomsday", "How the desktop ends", SAVER_CHOICE, kind_values, kind_labels,
		false, NULL },
	{ "hellfire_flash", "White flash", "The blast at the start; off for anyone sensitive to "
		"flashes", SAVER_TOGGLE, NULL, NULL, true, "kind=hellfire" },
	{ "hellfire_debris", "Falling pieces", "Pieces of the decoration break off and pile up at "
		"the bottom", SAVER_TOGGLE, NULL, NULL, true, "kind=hellfire" },
	{ "hellfire_scraps", "What is left", "Scraps of the title bars, borders and taskbar that "
		"smoulder on", SAVER_CHOICE, scraps_values, scraps_labels, false, "kind=hellfire" },
	{ "thunderstorm_rain", "Rain", NULL, SAVER_CHOICE, rain_values, rain_labels, false,
		"kind=thunderstorm" },
	{ "thunderstorm_lightning", "Lightning", NULL, SAVER_CHOICE, bolt_values, bolt_labels,
		false, "kind=thunderstorm" },
	{ "thunderstorm_strikes", "Strikes into windows", "Lightning hits the windows and the "
		"taskbar too", SAVER_TOGGLE, NULL, NULL, true, "kind=thunderstorm" },
	{ "blizzard_snow", "Snow", "How hard it snows and blows", SAVER_CHOICE, snow_values,
		snow_labels, false, "kind=blizzard" },
	{ "blizzard_frost", "Frost on the windows", "Ice flowers grow over the glass of the "
		"windows and the screen", SAVER_TOGGLE, NULL, NULL, true, "kind=blizzard" },
	{ "blizzard_bury", "Snowed in", "A drift rises up the screen and snow plasters the "
		"windows until all is buried", SAVER_TOGGLE, NULL, NULL, true, "kind=blizzard" },
	{ 0 },
};

const struct saver saver_doomsday = {
	.name = "doomsday",
	.title = "Doomsday",
	.description = "The end of the desktop: burnt down in Hellfire, drowned in a "
		"Thunderstorm, or buried in a Blizzard",
	.wants_desktop = true,
	.covers = true,
	.options = doomsday_options,
	.create = doomsday_create,
	.draw = doomsday_draw,
	.destroy = doomsday_destroy,
};
