/*
 * "panel identify HDMI-A-1=1 DP-2=2": the number the settings give each
 * screen, shown big on the screen itself for a few seconds, like Windows'
 * Identify, so the boxes in the arrangement can be told apart.
 */
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "panel.h"

#define SHOW_MS 3000
#define SIZE 220

struct shown {
	char *number, *name;
};

static list_t *surfaces; // struct psurface *
static struct loop_timer *timer;

static void render(struct psurface *s, cairo_t *cr) {
	struct shown *sh = s->data;
	pd_rounded(cr, 0, 0, s->width, s->height, 12);
	pd_color(cr, 0x000000c0);
	cairo_fill(cr);
	char font[64];
	snprintf(font, sizeof(font), "Sans Bold %d", SIZE / 2);
	pd_text(cr, font, sh->number, 0, 10, s->width, s->height * 0.72, 0xffffffff, PD_CENTER);
	pd_text(cr, bar_font(s->panel), sh->name, 0, s->height * 0.72, s->width, s->height * 0.22,
		0xffffffcc, PD_CENTER);
}

static void closed(struct psurface *s) {
	int i = surfaces ? list_find(surfaces, s) : -1;
	if (i >= 0) {
		list_del(surfaces, i);
	}
	struct shown *sh = s->data;
	free(sh->number);
	free(sh->name);
	free(sh);
	psurface_destroy(s);
}

static const struct psurface_impl impl = {
	.render = render,
	.closed = closed,
};

static void hide(void *data) {
	timer = NULL;
	while (surfaces && surfaces->length) {
		closed(surfaces->items[surfaces->length - 1]);
	}
}

void identify_screens(struct panel *panel, int argc, char **argv) {
	if (timer) {
		loop_remove_timer(panel->loop, timer);
		timer = NULL;
	}
	hide(NULL);
	if (!surfaces) {
		surfaces = create_list();
	}
	for (int i = 0; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq) {
			continue;
		}
		struct panel_output *output, *found = NULL;
		wl_list_for_each(output, &panel->outputs, link) {
			if (output->name && strncmp(output->name, argv[i], eq - argv[i]) == 0 &&
					output->name[eq - argv[i]] == '\0') {
				found = output;
			}
		}
		if (!found) {
			continue;
		}
		struct shown *sh = calloc(1, sizeof(*sh));
		sh->number = strdup(eq + 1);
		sh->name = strdup(found->name);
		struct psurface *s = psurface_create(panel, found, &impl, sh,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "tilewin-identify");
		zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
		psurface_set_size(s, SIZE, SIZE);
		struct wl_region *region = wl_compositor_create_region(panel->compositor);
		wl_surface_set_input_region(s->surface, region); // clicks go through
		wl_region_destroy(region);
		wl_surface_commit(s->surface);
		list_add(surfaces, s);
	}
	timer = loop_add_timer(panel->loop, SHOW_MS, hide, NULL);
}
