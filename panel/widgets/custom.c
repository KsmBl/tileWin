#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "stringop.h"

/*
 * widget custom:<name> {
 *     exec "command"          # run every <interval> seconds, last line is shown
 *     interval 10
 *     exec_listen "command"   # long-running, every output line updates the widget
 *     format "{}"             # {} is replaced by the text
 *     icon <icon-name>
 *     on_click / on_right_click / on_middle_click / on_scroll_up / on_scroll_down
 * }
 *
 * Output lines may also be JSON: {"text": "...", "tooltip": "...", "icon": "..."}.
 */

struct custom_data {
	struct proc *proc;
	struct loop_timer *timer;
	char *text;
	char *tooltip;
	char *icon;
	bool listen;
};

static void apply_line(struct widget *w, const char *line) {
	struct custom_data *d = w->data;
	while (*line == ' ' || *line == '\t') {
		line++;
	}
	if (line[0] == '{') {
		json_object *obj = json_tokener_parse(line);
		if (obj) {
			json_object *v;
			if (json_object_object_get_ex(obj, "text", &v)) {
				free(d->text);
				d->text = strdup(json_object_get_string(v));
			}
			if (json_object_object_get_ex(obj, "tooltip", &v)) {
				free(d->tooltip);
				d->tooltip = strdup(json_object_get_string(v));
			}
			if (json_object_object_get_ex(obj, "icon", &v)) {
				free(d->icon);
				d->icon = strdup(json_object_get_string(v));
			}
			json_object_put(obj);
			widget_set_dirty(w);
			return;
		}
	}
	free(d->text);
	d->text = strdup(line);
	widget_set_dirty(w);
}

static void on_line(void *data, const char *line) {
	apply_line(data, line);
}

static void schedule(struct widget *w);

static void on_done(void *data, const char *output) {
	struct widget *w = data;
	struct custom_data *d = w->data;
	d->proc = NULL;
	if (!d->listen) {
		// use the last non-empty line
		char *copy = strdup(output);
		char *last = NULL, *save = NULL;
		for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			last = line;
		}
		if (last) {
			apply_line(w, last);
		}
		free(copy);
		schedule(w);
	}
}

static void run(void *data) {
	struct widget *w = data;
	struct custom_data *d = w->data;
	d->timer = NULL;
	if (!w->active || d->proc) {
		return;
	}
	const char *listen = widget_conf(w, "exec_listen", NULL);
	const char *exec = widget_conf(w, "exec", NULL);
	d->listen = listen != NULL;
	if (listen) {
		d->proc = proc_run(w->panel, listen, true, on_line, on_done, w);
	} else if (exec) {
		d->proc = proc_run(w->panel, exec, false, NULL, on_done, w);
	}
}

static void schedule(struct widget *w) {
	struct custom_data *d = w->data;
	if (!w->active || d->timer || d->listen) {
		return;
	}
	int interval = widget_conf_int(w, "interval", 0);
	if (interval > 0) {
		d->timer = loop_add_timer(w->panel->loop, interval * 1000, run, w);
	}
}

static void custom_init(struct widget *w) {
	struct custom_data *d = calloc(1, sizeof(*d));
	const char *icon = widget_conf(w, "icon", NULL);
	d->icon = icon ? strdup(icon) : NULL;
	w->data = d;
}

static void custom_set_active(struct widget *w, bool active) {
	struct custom_data *d = w->data;
	if (active) {
		if (!d->proc && !d->timer) {
			run(w);
		}
	} else {
		proc_cancel(d->proc);
		d->proc = NULL;
		if (d->timer) {
			loop_remove_timer(w->panel->loop, d->timer);
			d->timer = NULL;
		}
	}
}

static void custom_destroy(struct widget *w) {
	custom_set_active(w, false);
	struct custom_data *d = w->data;
	free(d->text);
	free(d->tooltip);
	free(d->icon);
	free(d);
}

static char *display_text(struct widget *w) {
	struct custom_data *d = w->data;
	if (!d->text) {
		return NULL;
	}
	const char *format = widget_conf(w, "format", "{}");
	char *out = strdup(format);
	char *pos;
	while ((pos = strstr(out, "{}"))) {
		char *next = format_str("%.*s%s%s", (int)(pos - out), out, d->text, pos + 2);
		free(out);
		out = next;
	}
	return out;
}

static int custom_measure(struct widget *w, struct render_ctx *ctx) {
	struct custom_data *d = w->data;
	char *text = display_text(w);
	if (!text && !d->icon) {
		return 0;
	}
	int width = 12;
	if (d->icon) {
		width += 18 + (text && *text ? 4 : 0);
	}
	if (text) {
		width += render_text_width(ctx, bar_font(ctx->panel), text);
		free(text);
	}
	return width;
}

static void custom_render(struct widget *w, struct render_ctx *ctx, struct pbox b) {
	struct custom_data *d = w->data;
	if (ctx->style != PSV_CLASSIC && ctx->style != PSV_LUNA) {
		render_item_bg(ctx, b, false, render_hover(ctx, b), render_pressed(ctx, b));
	}
	double x = b.x + 6;
	if (d->icon) {
		cairo_surface_t *icon = apps_icon(ctx->panel, d->icon, 16 * ctx->surface->scale);
		pd_icon(ctx->cairo, icon, x, b.y + (b.height - 16) / 2.0, 16);
		x += 20;
	}
	char *text = display_text(w);
	if (text) {
		pd_text(ctx->cairo, bar_font(ctx->panel), text, x, b.y, b.x + b.width - x - 6,
			b.height, bar_fg(ctx->panel), PD_LEFT);
		free(text);
	}
	psurface_add_hotspot(ctx->surface, b.x, b.y, b.width, b.height, w, 0, 0, NULL);
}

static char *custom_tooltip(struct widget *w, struct hotspot *hs) {
	struct custom_data *d = w->data;
	return d->tooltip ? strdup(d->tooltip) : NULL;
}

void custom_run_now(struct widget *w) {
	struct custom_data *d = w->data;
	if (!d->listen && !d->proc) {
		run(w);
	}
}

const char *custom_output(struct widget *w) {
	struct custom_data *d = w->data;
	return d->text;
}

static bool custom_click(struct widget *w, struct psurface *s, struct hotspot *hs,
		uint32_t button, double x, double y) {
	if (button != BTN_LEFT) {
		return false;
	}
	// the output in full, and a way to run the script again
	struct popup_anchor anchor = popup_anchor_for_bar(s, hs->box.x + hs->box.width, 0);
	anchor.right_align = true;
	info_flyout_toggle(w, anchor);
	return true;
}

const struct widget_impl widget_custom = {
	.type = "custom",
	.init = custom_init,
	.destroy = custom_destroy,
	.measure = custom_measure,
	.render = custom_render,
	.click = custom_click,
	.tooltip = custom_tooltip,
	.set_active = custom_set_active,
};
