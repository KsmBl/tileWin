/*
 * The shut down dialog in the look of the theme ("panel shutdown [logoff]"):
 *
 *   classic   Windows 95: "Shut Down Windows" with radio buttons over the
 *             dithered screen
 *   luna      Windows XP: "Turn off computer" with Stand By, Turn Off and
 *             Restart while the screen fades to gray; "Log Off Windows" with
 *             Switch User and Log Off for logoff
 *   security  Windows 7 / 10 / 11: the Ctrl+Alt+Del screen over the blurred
 *             desktop
 *   tiles     big buttons in a row over the blurred desktop, like wlogout
 *
 * shutdown.style in the theme picks one, otherwise the panel style does. The
 * commands come from the power entries of the startmenu block. The screen is
 * copied once (wlr-screencopy) before the dialog shows and only kept in memory.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "draw.h"
#include "log.h"
#include "panel.h"
#include "popup.h"

#define FADE_MS 1500
#define CAPTURE_TIMEOUT_MS 400

enum sd_style {
	SD_CLASSIC,
	SD_LUNA,
	SD_SECURITY,
	SD_TILES,
};

enum sd_action {
	ACT_LOCK,
	ACT_SWITCH_USER,
	ACT_LOGOUT,
	ACT_SUSPEND,
	ACT_HIBERNATE,
	ACT_HYBRID_SLEEP,
	ACT_RESTART,
	ACT_SHUTDOWN,
	ACT_COUNT,
};

enum {
	HS_ACTION = 1, // id: index into actions
	HS_OPTION,     // classic radio button
	HS_YES,
	HS_CANCEL,
};

struct shutdown {
	struct panel *panel;
	struct panel_output *output;
	struct popup *popup;
	enum sd_style style;
	bool logoff;
	enum sd_action actions[ACT_COUNT];
	int count;
	int selected; // -1: nothing chosen with the keyboard yet
	int hover_kind, press_kind;
	int64_t hover_id, press_id;

	struct zwlr_screencopy_frame_v1 *frame;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int buf_w, buf_h, stride;
	uint32_t format;
	bool have_format, y_invert;
	cairo_surface_t *shot; // luna: the screen in color
	cairo_surface_t *soft; // luna: in gray; security and tiles: small and blurred
	struct loop_timer *timer; // capture timeout, then the fade
	struct timespec fade_start;
	double fade;
};

static struct shutdown *capturing = NULL;

static void show_dialog(struct shutdown *sd);

/* ---------- colors and glyphs ---------- */

static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		double ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
		out |= (uint32_t)lround(ca + (cb - ca) * t) << shift;
	}
	return out;
}

static void set_rgba(cairo_pattern_t *pattern, double offset, uint32_t c) {
	cairo_pattern_add_color_stop_rgba(pattern, offset, (c >> 24 & 0xff) / 255.0,
		(c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void glyph_power(cairo_t *cr, double cx, double cy, double s, double lw) {
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy + s * 0.06, s * 0.36, -M_PI / 2 + 0.65, M_PI * 1.5 - 0.65);
	cairo_move_to(cr, cx, cy - s * 0.42);
	cairo_line_to(cr, cx, cy + s * 0.02);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
}

static void glyph_restart(cairo_t *cr, double cx, double cy, double s, double lw) {
	double r = s * 0.36, a1 = M_PI * 1.5 - 0.35;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, -M_PI / 2 + 0.25, a1);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
	double ex = cx + r * cos(a1), ey = cy + r * sin(a1), h = s * 0.2;
	cairo_move_to(cr, ex + h * 0.9, ey);
	cairo_line_to(cr, ex - h * 0.5, ey - h * 0.95);
	cairo_line_to(cr, ex - h * 0.5, ey + h * 0.95);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void glyph_moon(cairo_t *cr, double cx, double cy, double s) {
	cairo_new_path(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_arc(cr, cx, cy, s * 0.36, 0, 2 * M_PI);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx + s * 0.2, cy - s * 0.14, s * 0.3, 0, 2 * M_PI);
	cairo_clip(cr);
	cairo_arc(cr, cx, cy, s * 0.36, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_reset_clip(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

static void glyph_logout(cairo_t *cr, double cx, double cy, double s, double lw) {
	double x = cx - s * 0.36, y = cy - s * 0.38, w = s * 0.4, h = s * 0.76;
	cairo_new_path(cr);
	cairo_move_to(cr, x + w, y + h * 0.28);
	cairo_line_to(cr, x + w, y);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x, y + h);
	cairo_line_to(cr, x + w, y + h);
	cairo_line_to(cr, x + w, y + h * 0.72);
	cairo_move_to(cr, cx - s * 0.08, cy);
	cairo_line_to(cr, cx + s * 0.34, cy);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	double h2 = s * 0.14;
	cairo_move_to(cr, cx + s * 0.44, cy);
	cairo_line_to(cr, cx + s * 0.26, cy - h2);
	cairo_line_to(cr, cx + s * 0.26, cy + h2);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* Hibernate: a snowflake, the computer is cold. */
static void glyph_snowflake(cairo_t *cr, double cx, double cy, double s, double lw) {
	cairo_new_path(cr);
	double r = s * 0.38, b = s * 0.12;
	for (int i = 0; i < 6; i++) {
		double a = i * M_PI / 3 - M_PI / 2, ca = cos(a), sa = sin(a);
		cairo_move_to(cr, cx, cy);
		cairo_line_to(cr, cx + r * ca, cy + r * sa);
		// the little branches two thirds out
		double bx = cx + r * 0.62 * ca, by = cy + r * 0.62 * sa;
		for (int side = -1; side <= 1; side += 2) {
			double ba = a + side * M_PI / 4;
			cairo_move_to(cr, bx, by);
			cairo_line_to(cr, bx + b * cos(ba), by + b * sin(ba));
		}
	}
	cairo_set_line_width(cr, lw * 0.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
}

static void glyph_lock(cairo_t *cr, double cx, double cy, double s, double lw) {
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy - s * 0.08, s * 0.2, M_PI, 2 * M_PI);
	cairo_line_to(cr, cx + s * 0.2, cy);
	cairo_move_to(cr, cx - s * 0.2, cy);
	cairo_line_to(cr, cx - s * 0.2, cy - s * 0.08);
	cairo_set_line_width(cr, lw);
	cairo_stroke(cr);
	pd_rounded(cr, cx - s * 0.32, cy - s * 0.02, s * 0.64, s * 0.42, s * 0.06);
	cairo_fill(cr);
}

static void glyph_users(cairo_t *cr, double cx, double cy, double s) {
	for (int i = 1; i >= 0; i--) {
		double ox = cx + (i ? s * 0.16 : -s * 0.1), oy = cy + (i ? -s * 0.06 : 0);
		double sc = i ? 0.8 : 1.0;
		cairo_new_path(cr);
		cairo_arc(cr, ox, oy - s * 0.16 * sc, s * 0.14 * sc, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_new_path(cr);
		cairo_arc(cr, ox, oy + s * 0.3 * sc, s * 0.26 * sc, M_PI, 2 * M_PI);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
}

/* Draws the action's glyph in the current source color. */
static void glyph(cairo_t *cr, enum sd_action action, double cx, double cy, double s) {
	double lw = s * 0.11 < 1.5 ? 1.5 : s * 0.11;
	switch (action) {
	case ACT_LOCK:
		glyph_lock(cr, cx, cy, s, lw);
		break;
	case ACT_SWITCH_USER:
		glyph_users(cr, cx, cy, s);
		break;
	case ACT_LOGOUT:
		glyph_logout(cr, cx, cy, s, lw);
		break;
	case ACT_SUSPEND:
		glyph_moon(cr, cx, cy, s);
		break;
	case ACT_HIBERNATE:
		glyph_snowflake(cr, cx, cy, s, lw);
		break;
	case ACT_HYBRID_SLEEP:
		// both: the moon with a small snowflake
		glyph_moon(cr, cx - s * 0.1, cy - s * 0.08, s * 0.85);
		glyph_snowflake(cr, cx + s * 0.26, cy + s * 0.24, s * 0.5, lw);
		break;
	case ACT_RESTART:
		glyph_restart(cr, cx, cy, s, lw);
		break;
	case ACT_SHUTDOWN:
	case ACT_COUNT:
		glyph_power(cr, cx, cy, s, lw);
		break;
	}
}

/* ---------- actions ---------- */

static const char *action_label(struct shutdown *sd, enum sd_action action) {
	switch (sd->style) {
	case SD_CLASSIC: {
		static const char *const labels[ACT_COUNT] = { "Lock the computer?",
			"Switch the user?", "Close all programs and log on as a different user?",
			"Put the computer on stand by?", "Hibernate the computer?",
			"Put the computer into hybrid sleep?", "Restart the computer?",
			"Shut down the computer?" };
		return labels[action];
	}
	case SD_LUNA: {
		static const char *const labels[ACT_COUNT] = { "Lock", "Switch User", "Log Off",
			"Stand By", "Hibernate", "Hybrid Sleep", "Restart", "Turn Off" };
		return labels[action];
	}
	case SD_SECURITY:
	case SD_TILES:
		break;
	}
	static const char *const labels[ACT_COUNT] = { "Lock", "Switch user", "Sign out",
		"Sleep", "Hibernate", "Hybrid sleep", "Restart", "Shut down" };
	return labels[action];
}

/* The command of an action: a matching power entry of the start menu, or a default. */
static char *action_command(struct panel *panel, enum sd_action action) {
	static const char *const defaults[ACT_COUNT] = { "exec tilewin-lock -f",
		"exec tilewin-lock -f", "exit", "exec systemctl suspend",
		"exec systemctl hibernate", "exec systemctl hybrid-sleep",
		"exec systemctl reboot", "exec systemctl poweroff" };
	struct twconf_node *sm = panel->config ? panel->config->startmenu : NULL;
	for (int i = 0; sm && i < twconf_count(sm); i++) {
		struct twconf_node *child = twconf_at(sm, i);
		if (strcmp(child->name, "power") != 0 || child->argc < 2) {
			continue;
		}
		const char *cmd = child->argv[1];
		bool match = false;
		switch (action) {
		case ACT_LOCK:
		case ACT_SWITCH_USER:
			match = strstr(cmd, "lock") != NULL;
			break;
		case ACT_LOGOUT:
			match = strcmp(cmd, "exit") == 0;
			break;
		case ACT_SUSPEND:
			match = strstr(cmd, "suspend") && !strstr(cmd, "hibernate");
			break;
		case ACT_HIBERNATE:
			match = strstr(cmd, "hibernate") && !strstr(cmd, "hybrid");
			break;
		case ACT_HYBRID_SLEEP:
			match = strstr(cmd, "hybrid-sleep") != NULL;
			break;
		case ACT_RESTART:
			match = strstr(cmd, "reboot") != NULL;
			break;
		case ACT_SHUTDOWN:
			match = strstr(cmd, "poweroff") || strstr(cmd, "shutdown");
			break;
		case ACT_COUNT:
			break;
		}
		if (match) {
			return strdup(cmd);
		}
	}
	return strdup(defaults[action]);
}

struct delayed_command {
	struct panel *panel;
	char *command;
};

static void run_later(void *data) {
	struct delayed_command *d = data;
	bar_run_command(d->panel, d->command, NULL);
	free(d->command);
	free(d);
}

static void run_action(struct shutdown *sd, enum sd_action action) {
	struct delayed_command *d = calloc(1, sizeof(*d));
	d->panel = sd->panel;
	d->command = action_command(sd->panel, action);
	popup_close_later(sd->panel);
	// after the dialog is gone, so a lock screen or sleep does not show it
	loop_add_timer(sd->panel->loop, 80, run_later, d);
}

/* ---------- screen copy ---------- */

static void free_capture(struct shutdown *sd) {
	if (sd->frame) {
		zwlr_screencopy_frame_v1_destroy(sd->frame);
		sd->frame = NULL;
	}
	if (sd->buffer) {
		wl_buffer_destroy(sd->buffer);
		sd->buffer = NULL;
	}
	if (sd->data) {
		munmap(sd->data, sd->size);
		sd->data = NULL;
	}
}

static void free_shutdown(struct shutdown *sd) {
	if (sd->timer) {
		loop_remove_timer(sd->panel->loop, sd->timer);
	}
	free_capture(sd);
	if (sd->shot) {
		cairo_surface_destroy(sd->shot);
	}
	if (sd->soft) {
		cairo_surface_destroy(sd->soft);
	}
	if (capturing == sd) {
		capturing = NULL;
	}
	free(sd);
}

/* Box blur of an RGB24 surface, horizontal and vertical. */
static void box_blur(cairo_surface_t *surface, int radius) {
	cairo_surface_flush(surface);
	int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
	int stride = cairo_image_surface_get_stride(surface);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(surface);
	int n = w > h ? w : h;
	uint32_t *line = malloc(n * sizeof(uint32_t));
	for (int pass = 0; pass < 2; pass++) {
		bool horizontal = pass == 0;
		int outer = horizontal ? h : w, inner = horizontal ? w : h;
		for (int o = 0; o < outer; o++) {
			for (int i = 0; i < inner; i++) {
				line[i] = horizontal ? px[o * (stride / 4) + i] : px[i * (stride / 4) + o];
			}
			for (int i = 0; i < inner; i++) {
				unsigned r = 0, g = 0, b = 0, count = 0;
				for (int k = i - radius; k <= i + radius; k++) {
					uint32_t c = line[k < 0 ? 0 : k >= inner ? inner - 1 : k];
					r += c >> 16 & 0xff;
					g += c >> 8 & 0xff;
					b += c & 0xff;
					count++;
				}
				uint32_t c = 0xff000000 | (r / count) << 16 | (g / count) << 8 | (b / count);
				if (horizontal) {
					px[o * (stride / 4) + i] = c;
				} else {
					px[i * (stride / 4) + o] = c;
				}
			}
		}
	}
	free(line);
	cairo_surface_mark_dirty(surface);
}

static cairo_surface_t *scaled_copy(cairo_surface_t *src, int w, int h) {
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(out);
	cairo_scale(cr, (double)w / cairo_image_surface_get_width(src),
		(double)h / cairo_image_surface_get_height(src));
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

static void make_surfaces(struct shutdown *sd) {
	int w = sd->buf_w, h = sd->buf_h;
	cairo_surface_t *shot = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	if (cairo_surface_status(shot) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(shot);
		return;
	}
	int stride = cairo_image_surface_get_stride(shot);
	uint8_t *dst = cairo_image_surface_get_data(shot);
	bool bgr = sd->format == WL_SHM_FORMAT_XBGR8888 || sd->format == WL_SHM_FORMAT_ABGR8888;
	for (int y = 0; y < h; y++) {
		uint8_t *src = (uint8_t *)sd->data + (sd->y_invert ? h - 1 - y : y) * sd->stride;
		for (int x = 0; x < w; x++) {
			uint32_t c;
			memcpy(&c, src + x * 4, 4);
			if (bgr) {
				c = (c & 0xff00ff00) | (c >> 16 & 0xff) | (c & 0xff) << 16;
			}
			c |= 0xff000000;
			memcpy(dst + y * stride + x * 4, &c, 4);
		}
	}
	cairo_surface_mark_dirty(shot);

	if (sd->style == SD_LUNA) {
		cairo_surface_t *gray = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
		uint8_t *g = cairo_image_surface_get_data(gray);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint32_t c;
				memcpy(&c, dst + y * stride + x * 4, 4);
				uint32_t l = ((c >> 16 & 0xff) * 77 + (c >> 8 & 0xff) * 151 + (c & 0xff) * 28) >> 8;
				c = 0xff000000 | l << 16 | l << 8 | l;
				memcpy(g + y * stride + x * 4, &c, 4);
			}
		}
		cairo_surface_mark_dirty(gray);
		sd->shot = shot;
		sd->soft = gray;
	} else {
		int sw = w / 6 > 1 ? w / 6 : 1, sh = h / 6 > 1 ? h / 6 : 1;
		sd->soft = scaled_copy(shot, sw, sh);
		box_blur(sd->soft, 5);
		box_blur(sd->soft, 5);
		cairo_surface_destroy(shot);
	}
}

static void capture_finished(struct shutdown *sd, bool ok) {
	if (sd->timer) {
		loop_remove_timer(sd->panel->loop, sd->timer);
		sd->timer = NULL;
	}
	if (ok && sd->data) {
		make_surfaces(sd);
	}
	free_capture(sd);
	capturing = NULL;
	show_dialog(sd);
}

static bool supported_format(uint32_t format) {
	return format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888 ||
		format == WL_SHM_FORMAT_XBGR8888 || format == WL_SHM_FORMAT_ABGR8888;
}

static void copy_frame(struct shutdown *sd) {
	if (!sd->have_format || sd->buffer) {
		if (!sd->have_format) {
			capture_finished(sd, false);
		}
		return;
	}
	sd->size = (size_t)sd->stride * sd->buf_h;
	int fd = memfd_create("tilewin-shutdown", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, sd->size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		capture_finished(sd, false);
		return;
	}
	sd->data = mmap(NULL, sd->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (sd->data == MAP_FAILED) {
		sd->data = NULL;
		close(fd);
		capture_finished(sd, false);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(sd->panel->shm, fd, sd->size);
	sd->buffer = wl_shm_pool_create_buffer(pool, 0, sd->buf_w, sd->buf_h, sd->stride,
		sd->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(sd->frame, sd->buffer);
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct shutdown *sd = data;
	if (!sd->have_format && supported_format(format) && stride >= width * 4) {
		sd->have_format = true;
		sd->format = format;
		sd->buf_w = width;
		sd->buf_h = height;
		sd->stride = stride;
	}
	if (sd->panel->screencopy_version < 3) {
		copy_frame(sd);
	}
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct shutdown *sd = data;
	sd->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
	capture_finished(data, true);
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	capture_finished(data, false);
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	copy_frame(data);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

static void capture_timeout(void *data) {
	struct shutdown *sd = data;
	sd->timer = NULL;
	sway_log(SWAY_DEBUG, "Screen copy for the shut down dialog timed out");
	capture_finished(sd, false);
}

/* ---------- drawing ---------- */

static bool hot(struct shutdown *sd, int kind, int64_t id) {
	return sd->hover_kind == kind && sd->hover_id == id;
}

static bool pressed(struct shutdown *sd, int kind, int64_t id) {
	return sd->press_kind == kind && sd->press_id == id && hot(sd, kind, id);
}

static void paint_scaled(cairo_t *cr, cairo_surface_t *s, double w, double h, double alpha) {
	cairo_save(cr);
	cairo_scale(cr, w / cairo_image_surface_get_width(s), h / cairo_image_surface_get_height(s));
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

/* Faint noise, one texel per device pixel: hides the bands of a smooth dark blur. */
static void paint_noise(cairo_t *cr, double W, double H, int scale) {
	static cairo_surface_t *noise = NULL;
	if (!noise) {
		noise = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 96);
		cairo_surface_flush(noise);
		uint32_t *px = (uint32_t *)cairo_image_surface_get_data(noise);
		int row = cairo_image_surface_get_stride(noise) / 4;
		uint32_t seed = 0x2545f491;
		for (int y = 0; y < 96; y++) {
			for (int x = 0; x < 96; x++) {
				seed = seed * 1664525 + 1013904223;
				uint32_t a = (seed >> 24) % 7; // premultiplied white or black, alpha 0..6
				px[y * row + x] = a << 24 | ((seed >> 8) & 1 ? a * 0x010101 : 0);
			}
		}
		cairo_surface_mark_dirty(noise);
	}
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(noise);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_matrix_t matrix;
	cairo_matrix_init_scale(&matrix, scale, scale);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_rectangle(cr, 0, 0, W, H);
	cairo_set_source(cr, pattern);
	cairo_fill(cr);
	cairo_pattern_destroy(pattern);
}

/* Blurred desktop with a tint, or a plain color without a screen copy. */
static void paint_soft_background(struct shutdown *sd, cairo_t *cr, double W, double H,
		uint32_t tint, uint32_t plain) {
	const struct tw_theme *t = sd->panel->theme;
	if (sd->soft) {
		paint_scaled(cr, sd->soft, W, H, 1);
		pd_rect(cr, 0, 0, W, H, tw_theme_color(t, "shutdown.tint", tint));
		paint_noise(cr, W, H, sd->popup->surface->scale);
	} else {
		cairo_rectangle(cr, 0, 0, W, H);
		pd_fill(cr, t, "shutdown.bg", 0, H, plain);
	}
}

static void classic_button(struct shutdown *sd, cairo_t *cr, double x, double y, double w,
		double h, const char *label, int kind, bool is_default) {
	struct panel *panel = sd->panel;
	bool down = pressed(sd, kind, 0);
	if (is_default) {
		pd_rect(cr, x - 1, y - 1, w + 2, h + 2, 0x000000ff);
	}
	pd_rect(cr, x, y, w, h, 0xc0c0c0ff);
	pd_bevel(cr, x, y, w, h, down);
	pd_text(cr, bar_font(panel), label, x + (down ? 1 : 0), y + (down ? 1 : 0), w, h,
		0x000000ff, PD_CENTER);
	psurface_add_hotspot(sd->popup->surface, x, y, w, h, NULL, kind, 0, NULL);
}

static void render_classic(struct shutdown *sd, cairo_t *cr, double W, double H) {
	struct panel *panel = sd->panel;
	const struct tw_theme *t = panel->theme;
	int scale = sd->popup->surface->scale;

	// every second pixel of the screen turns black, like on Windows 95
	cairo_surface_t *dots = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
	cairo_surface_flush(dots);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(dots);
	int row = cairo_image_surface_get_stride(dots) / 4;
	px[0] = px[row + 1] = 0xff000000;
	px[1] = px[row] = 0;
	cairo_surface_mark_dirty(dots);
	cairo_pattern_t *pattern = cairo_pattern_create_for_surface(dots);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_matrix_t matrix;
	cairo_matrix_init_scale(&matrix, scale, scale);
	cairo_pattern_set_matrix(pattern, &matrix);
	cairo_set_source(cr, pattern);
	cairo_paint(cr);
	cairo_pattern_destroy(pattern);
	cairo_surface_destroy(dots);

	const char *font = bar_font(panel);
	double row_h = 26, dw = 440, dh = 22 + 44 + sd->count * row_h + 18 + 26 + 16;
	double dx = round((W - dw) / 2), dy = round((H - dh) / 3);
	pd_rect(cr, dx, dy, dw, dh, 0xc0c0c0ff);
	pd_bevel(cr, dx, dy, dw, dh, false);
	cairo_rectangle(cr, dx + 3, dy + 3, dw - 6, 20);
	pd_fill(cr, t, "shutdown.title_bg", dy + 3, 20, 0x000080ff);
	pd_text(cr, bar_bold_font(panel), "Shut Down Windows", dx + 8, dy + 3, dw - 40, 20,
		0xffffffff, PD_LEFT);
	// close button
	double cbx = dx + dw - 23, cby = dy + 5;
	bool close_down = pressed(sd, HS_CANCEL, 1);
	pd_rect(cr, cbx, cby, 17, 15, 0xc0c0c0ff);
	pd_bevel(cr, cbx, cby, 17, 15, close_down);
	double o = close_down ? 1 : 0;
	cairo_move_to(cr, cbx + 5 + o, cby + 4 + o);
	cairo_line_to(cr, cbx + 11 + o, cby + 10 + o);
	cairo_move_to(cr, cbx + 11 + o, cby + 4 + o);
	cairo_line_to(cr, cbx + 5 + o, cby + 10 + o);
	pd_color(cr, 0x000000ff);
	cairo_set_line_width(cr, 1.8);
	cairo_stroke(cr);
	psurface_add_hotspot(sd->popup->surface, cbx, cby, 17, 15, NULL, HS_CANCEL, 1, NULL);

	cairo_surface_t *icon = apps_icon(panel, "computer", 32 * scale);
	if (icon) {
		pd_icon(cr, icon, dx + 18, dy + 38, 32);
	}
	pd_text(cr, font, "Are you sure you want to:", dx + 66, dy + 32, dw - 80, 24,
		0x000000ff, PD_LEFT);
	for (int i = 0; i < sd->count; i++) {
		double oy = dy + 22 + 44 + i * row_h, rx = dx + 76, ry = oy + row_h / 2;
		// sunken round radio button
		cairo_new_path(cr);
		cairo_arc(cr, rx, ry, 6, 0, 2 * M_PI);
		pd_color(cr, 0xffffffff);
		cairo_fill(cr);
		cairo_set_line_width(cr, 1);
		cairo_new_path(cr);
		cairo_arc(cr, rx, ry, 5.5, M_PI * 0.75, M_PI * 1.75);
		pd_color(cr, 0x808080ff);
		cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_arc(cr, rx, ry, 4.5, M_PI * 0.75, M_PI * 1.75);
		pd_color(cr, 0x000000ff);
		cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_arc(cr, rx, ry, 5.5, M_PI * 1.75, M_PI * 2.75);
		pd_color(cr, 0xffffffff);
		cairo_stroke(cr);
		if (i == sd->selected) {
			cairo_new_path(cr);
			cairo_arc(cr, rx, ry, 2.2, 0, 2 * M_PI);
			pd_color(cr, 0x000000ff);
			cairo_fill(cr);
		}
		const char *label = action_label(sd, sd->actions[i]);
		pd_text(cr, font, label, rx + 14, oy, dw - 110, row_h, 0x000000ff, PD_LEFT);
		if (i == sd->selected) {
			int tw = 0;
			pd_text_size(cr, font, label, &tw, NULL);
			cairo_rectangle(cr, rx + 11.5, oy + 3.5, tw + 6, row_h - 7);
			cairo_set_dash(cr, (double[]){ 1, 1 }, 2, 0);
			pd_color(cr, 0x000000ff);
			cairo_stroke(cr);
			cairo_set_dash(cr, NULL, 0, 0);
		}
		psurface_add_hotspot(sd->popup->surface, rx - 10, oy, dw - 90, row_h, NULL,
			HS_OPTION, i, NULL);
	}
	double bw = 88, bh = 26, by = dy + dh - 16 - bh, bx = dx + (dw - 2 * bw - 10) / 2;
	classic_button(sd, cr, bx, by, bw, bh, "Yes", HS_YES, true);
	classic_button(sd, cr, bx + bw + 10, by, bw, bh, "No", HS_CANCEL, false);
}

static void luna_button(struct shutdown *sd, cairo_t *cr, int index, double cx, double cy,
		double k, double dx, double dy) {
	enum sd_action action = sd->actions[index];
	uint32_t color = action == ACT_SHUTDOWN ? 0xd8431fff :
		action == ACT_RESTART || action == ACT_SWITCH_USER ? 0x3c9a2cff :
		action == ACT_LOGOUT ? 0xe28a1dff : 0xe5a91dff;
	bool on = hot(sd, HS_ACTION, index) || sd->selected == index;
	if (on) {
		color = mix(color, 0xffffffff, 0.18);
	}
	double r = 16;
	// light edge
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r + 1.6, 0, 2 * M_PI);
	pd_color(cr, on ? 0xffffffff : 0xdfe8f8e0);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_pattern_t *ball = cairo_pattern_create_radial(cx - r * 0.35, cy - r * 0.45, r * 0.1,
		cx, cy, r * 1.05);
	set_rgba(ball, 0, mix(color, 0xffffffff, 0.55));
	set_rgba(ball, 0.55, color);
	set_rgba(ball, 1, mix(color, 0x000000ff, 0.35));
	cairo_set_source(cr, ball);
	cairo_fill(cr);
	cairo_pattern_destroy(ball);
	// gloss on the upper half
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_translate(cr, cx, cy - r * 0.42);
	cairo_scale(cr, 1, 0.58);
	cairo_arc(cr, 0, 0, r * 0.78, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_pattern_t *gloss = pd_gradient("0:#ffffffb0 1:#ffffff10", 0, cy - r, 0, cy);
	cairo_set_source(cr, gloss);
	cairo_fill(cr);
	cairo_pattern_destroy(gloss);
	pd_color(cr, 0xffffffff);
	glyph(cr, action, cx, cy + 0.5, r * 1.15);
	pd_text(cr, "Tahoma, Noto Sans 8.5", action_label(sd, action), cx - 45, cy + r + 4, 90, 16,
		0xffffffff, PD_CENTER);
	psurface_add_hotspot(sd->popup->surface, dx + (cx - 40) * k, dy + (cy - r - 4) * k,
		80 * k, (2 * r + 26) * k, NULL, HS_ACTION, index, NULL);
}

static void render_luna(struct shutdown *sd, cairo_t *cr, double W, double H) {
	const struct tw_theme *t = sd->panel->theme;
	if (sd->shot) {
		paint_scaled(cr, sd->shot, W, H, 1);
		paint_scaled(cr, sd->soft, W, H, sd->fade);
	} else {
		pd_rect(cr, 0, 0, W, H, 0x00000000 | (uint32_t)lround(0x50 * sd->fade));
	}

	// Windows XP's dialog is 314 x 200, wider for more than three buttons;
	// shutdown.scale enlarges it
	double k = tw_theme_double(t, "shutdown.scale", 1.25);
	double dw = sd->count * 79 + 40 > 314 ? sd->count * 79 + 40 : 314, dh = 200;
	double dx = round((W - dw * k) / 2), dy = round((H - dh * k) / 3);
	cairo_save(cr);
	cairo_translate(cr, dx, dy);
	cairo_scale(cr, k, k);

	cairo_rectangle(cr, 0, 0, dw, 44);
	pd_fill(cr, t, "shutdown.header_bg", 0, 44, 0x0a2f9cff);
	pd_text(cr, "Franklin Gothic Medium, Trebuchet MS, Noto Sans 14",
		sd->logoff ? "Log Off Windows" : "Turn off computer", 12, 0, dw - 74, 44, 0xffffffff,
		PD_LEFT);
	pd_glyph_windows(cr, dw - 42, 8, 28, 0xf35325ff, 0x81bc06ff, 0x05a6f0ff, 0xffba08ff, true);
	cairo_pattern_t *line = pd_gradient("0:#6f95e8 0.35:#c9d8f8 1:#4b73d6", 0, 0, dw, 0);
	cairo_rectangle(cr, 0, 44, dw, 2);
	cairo_set_source(cr, line);
	cairo_fill(cr);
	cairo_pattern_destroy(line);

	cairo_pattern_t *middle = pd_gradient("0:#8eaef1 0.45:#6b8fe4 1:#4d72d8", 0, 46, dw, 156);
	cairo_rectangle(cr, 0, 46, dw, 110);
	cairo_set_source(cr, middle);
	cairo_fill(cr);
	cairo_pattern_destroy(middle);

	cairo_rectangle(cr, 0, 156, dw, 44);
	pd_fill(cr, t, "shutdown.footer_bg", 156, 44, 0x0a2f9cff);

	for (int i = 0; i < sd->count; i++) {
		luna_button(sd, cr, i, dw / 2 + (i - (sd->count - 1) / 2.0) * 79, 93, k, dx, dy);
	}

	double bx = dw - 74, by = 167, bw = 62, bh = 22;
	bool cancel_hot = hot(sd, HS_CANCEL, 0);
	pd_rounded(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 3);
	cairo_pattern_t *face = pd_gradient(pressed(sd, HS_CANCEL, 0) ?
		"0:#e3e2da 1:#f6f6f3" : "0:#ffffff 0.85:#ecebe6 1:#d6d0c5", 0, by, 0, by + bh);
	cairo_set_source(cr, face);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(face);
	pd_color(cr, 0x003c74ff);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (cancel_hot) {
		pd_rounded(cr, bx + 2, by + 2, bw - 4, bh - 4, 2);
		pd_color(cr, 0xf8b33cff);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
	}
	pd_text(cr, "Tahoma, Noto Sans 8.5", "Cancel", bx, by, bw, bh, 0x000000ff, PD_CENTER);
	cairo_restore(cr);
	psurface_add_hotspot(sd->popup->surface, dx + bx * k, dy + by * k, bw * k, bh * k, NULL,
		HS_CANCEL, 0, NULL);
}

static void render_security(struct shutdown *sd, cairo_t *cr, double W, double H) {
	struct panel *panel = sd->panel;
	const struct tw_theme *t = panel->theme;
	enum pstyle style = panel_style(panel);
	paint_soft_background(sd, cr, W, H, style == PS_AERO ? 0x0c3a78b0 : 0x00000080,
		style == PS_AERO ? 0x1c5b9eff : style == PS_FLAT ? 0x0f3f8cff : 0x202020ff);
	uint32_t fg = tw_theme_color(t, "shutdown.fg", 0xffffffff);
	double radius = tw_theme_double(t, "shutdown.radius", style == PS_FLAT ? 0 : 6);
	const char *font = tw_theme_str(t, "shutdown.font", "Segoe UI, Noto Sans 15");

	double iw = 300, ih = 48, gap = 4;
	double total = sd->count * (ih + gap) + 40 + 40;
	double x = round((W - iw) / 2), y = round((H - total) / 2);
	for (int i = 0; i < sd->count; i++) {
		double iy = y + i * (ih + gap);
		if (hot(sd, HS_ACTION, i)) {
			pd_rounded(cr, x, iy, iw, ih, radius);
			pd_color(cr, 0xffffff26);
			cairo_fill(cr);
		}
		if (i == sd->selected) {
			pd_rounded(cr, x + 1, iy + 1, iw - 2, ih - 2, radius);
			pd_color(cr, fg);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
		pd_color(cr, fg);
		glyph(cr, sd->actions[i], x + 34, iy + ih / 2, 22);
		pd_text(cr, font, action_label(sd, sd->actions[i]), x + 64, iy, iw - 80, ih, fg,
			PD_LEFT);
		psurface_add_hotspot(sd->popup->surface, x, iy, iw, ih, NULL, HS_ACTION, i, NULL);
	}
	double bw = 140, bh = 38, bx = round((W - bw) / 2), by = y + total - bh;
	pd_rounded(cr, bx + 1, by + 1, bw - 2, bh - 2, radius);
	pd_color(cr, hot(sd, HS_CANCEL, 0) ? 0xffffff40 : 0xffffff1a);
	cairo_fill_preserve(cr);
	pd_color(cr, 0xffffff80);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);
	pd_text(cr, font, "Cancel", bx, by, bw, bh, fg, PD_CENTER);
	psurface_add_hotspot(sd->popup->surface, bx, by, bw, bh, NULL, HS_CANCEL, 0, NULL);
}

static void render_tiles(struct shutdown *sd, cairo_t *cr, double W, double H) {
	const struct tw_theme *t = sd->panel->theme;
	paint_soft_background(sd, cr, W, H, 0x0e1120b8, 0x0e1120ee);
	uint32_t fg = tw_theme_color(t, "shutdown.fg", 0xdde3f5ff);
	uint32_t accent = tw_theme_color(t, "shutdown.accent", 0x7287fdff);
	uint32_t button_bg = tw_theme_color(t, "shutdown.button_bg", 0x1e1e2ed0);
	uint32_t hover_bg = tw_theme_color(t, "shutdown.hover_bg", 0x5b8fff38);
	uint32_t border = tw_theme_color(t, "shutdown.border", 0x5b8fffa0);
	double radius = tw_theme_double(t, "shutdown.radius", 14);
	const char *font = tw_theme_str(t, "shutdown.font", bar_font(sd->panel));

	double gap = 24, size = 160;
	double fit = (W - 80 - gap * (sd->count - 1)) / sd->count;
	size = fit < size ? fit : size;
	double x0 = round((W - sd->count * size - (sd->count - 1) * gap) / 2);
	double y0 = round((H - size) / 2);
	for (int i = 0; i < sd->count; i++) {
		double x = x0 + i * (size + gap);
		bool on = hot(sd, HS_ACTION, i) || sd->selected == i;
		pd_rounded(cr, x, y0, size, size, radius);
		pd_color(cr, on ? hover_bg : button_bg);
		cairo_fill(cr);
		if (on) {
			pd_rounded(cr, x + 1, y0 + 1, size - 2, size - 2, radius);
			pd_color(cr, border);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
		pd_color(cr, on ? accent : fg);
		glyph(cr, sd->actions[i], x + size / 2, y0 + size * 0.42, size * 0.34);
		pd_text(cr, font, action_label(sd, sd->actions[i]), x, y0 + size * 0.68, size,
			size * 0.2, on ? accent : fg, PD_CENTER);
		psurface_add_hotspot(sd->popup->surface, x, y0, size, size, NULL, HS_ACTION, i, NULL);
	}
}

static void sd_render(struct popup *p, cairo_t *cr) {
	struct shutdown *sd = p->data;
	double W = p->surface->width, H = p->surface->height;
	switch (sd->style) {
	case SD_CLASSIC:
		render_classic(sd, cr, W, H);
		break;
	case SD_LUNA:
		render_luna(sd, cr, W, H);
		break;
	case SD_SECURITY:
		render_security(sd, cr, W, H);
		break;
	case SD_TILES:
		render_tiles(sd, cr, W, H);
		break;
	}
}

/* ---------- input ---------- */

static void fade_step(void *data) {
	struct shutdown *sd = data;
	sd->timer = NULL;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double ms = (now.tv_sec - sd->fade_start.tv_sec) * 1000.0 +
		(now.tv_nsec - sd->fade_start.tv_nsec) / 1e6;
	sd->fade = ms >= FADE_MS ? 1 : ms / FADE_MS;
	popup_set_dirty(sd->popup);
	if (sd->fade < 1) {
		sd->timer = loop_add_timer(sd->panel->loop, 50, fade_step, sd);
	}
}

/* Takes kind and id, not the hotspot: redrawing frees the hotspots. */
static void set_hover(struct popup *p, int kind, int64_t id) {
	struct shutdown *sd = p->data;
	if (kind != sd->hover_kind || id != sd->hover_id) {
		sd->hover_kind = kind;
		sd->hover_id = id;
		popup_set_dirty(p);
	}
}

static void sd_motion(struct popup *p, double x, double y) {
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	set_hover(p, hs ? hs->kind : 0, hs ? hs->id : -1);
}

static void sd_leave(struct popup *p) {
	set_hover(p, 0, -1);
}

static void sd_button(struct popup *p, double x, double y, uint32_t button, bool is_pressed) {
	struct shutdown *sd = p->data;
	if (button != BTN_LEFT) {
		return;
	}
	struct hotspot *hs = psurface_hotspot_at(p->surface, x, y);
	int hit_kind = hs ? hs->kind : 0;
	int64_t hit_id = hs ? hs->id : -1;
	set_hover(p, hit_kind, hit_id);
	if (is_pressed) {
		sd->press_kind = hit_kind;
		sd->press_id = hit_id;
		popup_set_dirty(p);
		return;
	}
	int kind = sd->press_kind;
	int64_t id = sd->press_id;
	sd->press_kind = 0;
	popup_set_dirty(p);
	if (hit_kind == 0) {
		// a click next to the buttons of the full screen looks closes them
		if (kind == 0 && (sd->style == SD_SECURITY || sd->style == SD_TILES)) {
			popup_close_later(p->panel);
		}
		return;
	}
	if (hit_kind != kind || hit_id != id) {
		return;
	}
	switch (kind) {
	case HS_ACTION:
		run_action(sd, sd->actions[id]);
		break;
	case HS_OPTION:
		sd->selected = id;
		break;
	case HS_YES:
		if (sd->selected >= 0) {
			run_action(sd, sd->actions[sd->selected]);
		}
		break;
	case HS_CANCEL:
		popup_close_later(p->panel);
		break;
	}
}

static void sd_key(struct popup *p, xkb_keysym_t sym, const char *utf8, uint32_t mods) {
	struct shutdown *sd = p->data;
	switch (sym) {
	case XKB_KEY_Escape:
		popup_close_later(p->panel);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		if (sd->selected >= 0) {
			run_action(sd, sd->actions[sd->selected]);
		}
		return;
	case XKB_KEY_Left:
	case XKB_KEY_Up:
	case XKB_KEY_ISO_Left_Tab:
		sd->selected = sd->selected <= 0 ? sd->count - 1 : sd->selected - 1;
		popup_set_dirty(p);
		return;
	case XKB_KEY_Right:
	case XKB_KEY_Down:
	case XKB_KEY_Tab:
		sd->selected = (sd->selected + 1) % sd->count;
		popup_set_dirty(p);
		return;
	}
	if (sd->style == SD_CLASSIC) {
		if (sym == XKB_KEY_y || sym == XKB_KEY_Y) {
			run_action(sd, sd->actions[sd->selected < 0 ? 0 : sd->selected]);
		} else if (sym == XKB_KEY_n || sym == XKB_KEY_N) {
			popup_close_later(p->panel);
		}
		return;
	}
	// letters of the actions: Lock, sign out (E), Sleep/Stand by, Hibernate, hYbrid sleep,
	// Restart, Turn off (U), Switch user (W)
	static const struct {
		xkb_keysym_t sym;
		enum sd_action action;
	} letters[] = {
		{ XKB_KEY_k, ACT_LOCK }, { XKB_KEY_e, ACT_LOGOUT }, { XKB_KEY_s, ACT_SUSPEND },
		{ XKB_KEY_h, ACT_HIBERNATE }, { XKB_KEY_y, ACT_HYBRID_SLEEP }, { XKB_KEY_r, ACT_RESTART }, { XKB_KEY_u, ACT_SHUTDOWN }, { XKB_KEY_w, ACT_SWITCH_USER },
		{ XKB_KEY_l, ACT_LOGOUT }, { XKB_KEY_l, ACT_LOCK },
	};
	xkb_keysym_t lower = xkb_keysym_to_lower(sym);
	for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
		if (letters[i].sym != lower) {
			continue;
		}
		for (int j = 0; j < sd->count; j++) {
			if (sd->actions[j] == letters[i].action) {
				run_action(sd, letters[i].action);
				return;
			}
		}
	}
}

static void sd_destroy(struct popup *p) {
	free_shutdown(p->data);
}

static const struct popup_vtable shutdown_vtable = {
	.render = sd_render,
	.motion = sd_motion,
	.leave = sd_leave,
	.button = sd_button,
	.key = sd_key,
	.destroy = sd_destroy,
};

/* ---------- opening ---------- */

static void show_dialog(struct shutdown *sd) {
	struct panel_output *output, *found = NULL;
	wl_list_for_each(output, &sd->panel->outputs, link) {
		if (output == sd->output) {
			found = output;
		}
	}
	if (!found) {
		free_shutdown(sd);
		return;
	}
	sd->popup = popup_create(sd->panel, POPUP_SHUTDOWN, NULL, found, 0, 0, found->width,
		found->height, &shutdown_vtable, sd);
	if (!sd->popup) {
		free_shutdown(sd);
		return;
	}
	if (sd->style == SD_LUNA) {
		clock_gettime(CLOCK_MONOTONIC, &sd->fade_start);
		sd->timer = loop_add_timer(sd->panel->loop, 50, fade_step, sd);
	} else {
		sd->fade = 1;
	}
}

static enum sd_style theme_style(struct panel *panel) {
	const char *name = tw_theme_str(panel->theme, "shutdown.style", NULL);
	if (name) {
		if (strcmp(name, "classic") == 0) {
			return SD_CLASSIC;
		} else if (strcmp(name, "luna") == 0) {
			return SD_LUNA;
		} else if (strcmp(name, "tiles") == 0) {
			return SD_TILES;
		} else if (strcmp(name, "security") == 0) {
			return SD_SECURITY;
		}
		sway_log(SWAY_ERROR, "Unknown shutdown.style '%s'", name);
	}
	switch (panel_style(panel)) {
	case PS_CLASSIC:
		return SD_CLASSIC;
	case PS_LUNA:
		return SD_LUNA;
	case PS_AERO:
	case PS_FLAT:
	case PS_FLUENT:
		break;
	}
	return SD_SECURITY;
}

void shutdown_dialog_open(struct panel *panel, struct panel_output *output, bool logoff) {
	if (!output) {
		output = panel_focused_output(panel);
	}
	if (!output || capturing) {
		return;
	}
	if (popup_is_open(panel, POPUP_SHUTDOWN)) {
		popup_close_all(panel);
		return;
	}
	popup_close_all(panel);

	struct shutdown *sd = calloc(1, sizeof(*sd));
	sd->panel = panel;
	sd->output = output;
	sd->style = theme_style(panel);
	sd->logoff = logoff;
	sd->selected = -1;
	sd->hover_id = sd->press_id = -1;
	static const enum sd_action classic[] = { ACT_SHUTDOWN, ACT_RESTART, ACT_SUSPEND,
		ACT_HIBERNATE, ACT_HYBRID_SLEEP, ACT_LOGOUT };
	static const enum sd_action luna[] = { ACT_SUSPEND, ACT_HIBERNATE, ACT_HYBRID_SLEEP,
		ACT_SHUTDOWN, ACT_RESTART };
	static const enum sd_action luna_logoff[] = { ACT_SWITCH_USER, ACT_LOGOUT };
	static const enum sd_action full[] = { ACT_LOCK, ACT_LOGOUT, ACT_SUSPEND, ACT_HIBERNATE,
		ACT_HYBRID_SLEEP, ACT_RESTART, ACT_SHUTDOWN };
	const enum sd_action *list = full;
	int count = sizeof(full) / sizeof(full[0]);
	switch (sd->style) {
	case SD_CLASSIC:
		list = classic;
		count = sizeof(classic) / sizeof(classic[0]);
		break;
	case SD_LUNA:
		list = logoff ? luna_logoff : luna;
		count = logoff ? 2 : sizeof(luna) / sizeof(luna[0]);
		break;
	case SD_SECURITY:
	case SD_TILES:
		break;
	}
	// leave out the kinds of sleep this computer cannot do
	for (int i = 0; i < count; i++) {
		char *cmd = action_command(panel, list[i]);
		if (power_command_available(cmd)) {
			sd->actions[sd->count++] = list[i];
		}
		free(cmd);
	}
	if (sd->style == SD_CLASSIC && logoff) {
		sd->selected = sd->count - 1;
	} else if (sd->style == SD_CLASSIC) {
		sd->selected = 0;
	}

	// the Windows 95 look dithers the live screen, the others need a copy of it
	if (sd->style == SD_CLASSIC || !panel->screencopy) {
		show_dialog(sd);
		return;
	}
	capturing = sd;
	sd->frame = zwlr_screencopy_manager_v1_capture_output(panel->screencopy, 0,
		output->wl_output);
	zwlr_screencopy_frame_v1_add_listener(sd->frame, &frame_listener, sd);
	sd->timer = loop_add_timer(panel->loop, CAPTURE_TIMEOUT_MS, capture_timeout, sd);
}
