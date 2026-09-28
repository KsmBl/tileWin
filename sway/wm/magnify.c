#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "sway/config.h"
#include "sway/input/cursor.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tilewin.h"
#include "sway/tree/root.h"

/*
 * The magnifier: with Alt held ("magnifier <modifier>"), the scroll wheel
 * zooms the screen under the pointer in and out, and the pointer stays in the
 * middle of what is shown: moving the mouse moves the picture under it. Past
 * the edges of the screen it is dark. Moving the mouse moves the pointer by
 * less the more it is zoomed in, so pointing stays as precise as the picture.
 *
 * While zoomed, the screen is first drawn as usual into a picture of its own
 * (with the pointer in it) and the part around the pointer is then drawn
 * enlarged onto the screen. Nothing changes while not zoomed.
 */

#define LEVEL_MAX 32.0
#define STEP 1.25     // one notch of the wheel
#define EASE_S 0.07   // how quickly the zoom follows the wheel
#define FRAME_MS 8

static struct {
	double level, target; // 1 is not zoomed
	struct sway_output *output; // the zoomed screen
	struct wlr_swapchain *swapchain; // its picture before zooming
	bool locked; // its pointer is drawn with the picture
	struct wl_event_source *timer;
	int64_t last_ms;
} mag = { .level = 1, .target = 1 };

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

double tw_magnify_level(void) {
	return mag.level;
}

static void redraw_whole(struct sway_output *output) {
	if (output && output->scene_output && output->wlr_output && output->wlr_output->enabled) {
		wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
		wlr_output_schedule_frame(output->wlr_output);
	}
}

/* The zoomed screen goes back to normal. */
static void release(void) {
	struct sway_output *output = mag.output;
	if (!output) {
		return;
	}
	if (mag.locked && output->wlr_output) {
		wlr_output_lock_software_cursors(output->wlr_output, false);
	}
	mag.locked = false;
	if (mag.swapchain) {
		wlr_swapchain_destroy(mag.swapchain);
		mag.swapchain = NULL;
	}
	mag.output = NULL;
	redraw_whole(output); // its buffers hold zoomed pictures
}

static struct sway_output *pointer_output(void) {
	struct sway_seat *seat = input_manager_current_seat();
	if (!seat || !seat->cursor) {
		return NULL;
	}
	struct wlr_output *wo = wlr_output_layout_output_at(root->output_layout,
		seat->cursor->cursor->x, seat->cursor->cursor->y);
	return wo ? wo->data : NULL;
}

static void follow_pointer(void) {
	struct sway_output *output = mag.target > 1 || mag.level > 1 ? pointer_output() : NULL;
	if (output != mag.output) {
		release();
		mag.output = output;
		if (output && output->wlr_output) {
			wlr_output_lock_software_cursors(output->wlr_output, true);
			mag.locked = true;
			redraw_whole(output);
		}
	}
	if (mag.output) {
		wlr_output_schedule_frame(mag.output->wlr_output);
	}
}

static int tick(void *data) {
	int64_t now = now_ms();
	double dt = (now - mag.last_ms) / 1000.0;
	mag.last_ms = now;
	double k = 1 - exp(-dt / EASE_S);
	// eased in the logarithm, so each notch takes the same time
	double l = log(mag.level), t = log(mag.target);
	l += (t - l) * k;
	mag.level = exp(l);
	if (fabs(mag.level - mag.target) < 0.002 * mag.target) {
		mag.level = mag.target;
	}
	if (mag.level <= 1.0) {
		mag.level = 1;
		release();
		return 0;
	}
	follow_pointer();
	if (mag.level != mag.target) {
		wl_event_source_timer_update(mag.timer, FRAME_MS);
	}
	return 0;
}

static void set_target(double target) {
	if (target < 1.02) {
		target = 1;
	}
	if (target > LEVEL_MAX) {
		target = LEVEL_MAX;
	}
	if (target == mag.target && target == mag.level) {
		return;
	}
	mag.target = target;
	if (!mag.timer) {
		mag.timer = wl_event_loop_add_timer(server.wl_event_loop, tick, NULL);
	}
	if (target > 1 && !mag.output) {
		follow_pointer();
	}
	mag.last_ms = now_ms();
	if (mag.timer) {
		wl_event_source_timer_update(mag.timer, 1);
	}
}

bool tw_magnify_axis(uint32_t modifiers, int orientation, double delta,
		int32_t delta_discrete) {
	uint32_t mask = config ? config->tw_magnifier_modifier : 0;
	modifiers &= ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2); // Caps Lock and Num Lock
	if (!mask || modifiers != mask || orientation != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return false;
	}
	// a wheel notch is 120 of delta_discrete; a touchpad scrolls about 15 per notch
	double notches = delta_discrete != 0 ? -delta_discrete / 120.0 : -delta / 15.0;
	set_target(mag.target * pow(STEP, notches));
	return true;
}

bool tw_magnify_command(const char *arg, char **error) {
	if (strcasecmp(arg, "in") == 0) {
		set_target(mag.target * 1.5);
	} else if (strcasecmp(arg, "out") == 0) {
		set_target(mag.target / 1.5);
	} else if (strcasecmp(arg, "off") == 0 || strcasecmp(arg, "reset") == 0) {
		set_target(1);
	} else {
		char *end;
		double v = strtod(arg, &end);
		if (end == arg || *end || !(v >= 1) || v > LEVEL_MAX) {
			*error = strdup("Expected 'magnify in|out|off|<factor from 1 to 32>'");
			return false;
		}
		set_target(v);
	}
	return true;
}

void tw_magnify_scale_motion(double *dx, double *dy) {
	if (mag.level > 1) {
		*dx /= mag.level;
		*dy /= mag.level;
	}
}

void tw_magnify_pointer_moved(void) {
	if (mag.output || mag.target > 1) {
		follow_pointer();
	}
}

bool tw_magnify_active(struct sway_output *output) {
	return output && output == mag.output && mag.level > 1;
}

void tw_magnify_output_destroyed(struct sway_output *output) {
	if (output == mag.output) {
		if (mag.swapchain) {
			wlr_swapchain_destroy(mag.swapchain);
			mag.swapchain = NULL;
		}
		mag.locked = false; // (the output goes with its locks)
		mag.output = NULL;
	}
}

bool tw_magnify_render(struct sway_output *output, struct wlr_output_state *state,
		const struct wlr_scene_output_state_options *options) {
	struct wlr_output *wo = output->wlr_output;
	if (!wlr_output_configure_primary_swapchain(wo, state, &wo->swapchain)) {
		return false;
	}
	int width = wo->swapchain->width, height = wo->swapchain->height;
	if (mag.swapchain && (mag.swapchain->width != width || mag.swapchain->height != height)) {
		wlr_swapchain_destroy(mag.swapchain);
		mag.swapchain = NULL;
	}
	if (!mag.swapchain) {
		mag.swapchain = wlr_swapchain_create(wo->allocator, width, height,
			&wo->swapchain->format);
		if (!mag.swapchain) {
			return false;
		}
		wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
	}

	// the screen as usual, into a picture of its own
	struct wlr_scene_output_state_options own = *options;
	own.swapchain = mag.swapchain;
	struct wlr_output_state picture;
	wlr_output_state_init(&picture);
	if (!wlr_scene_output_build_state(output->scene_output, &picture, &own) ||
			!(picture.committed & WLR_OUTPUT_STATE_BUFFER) || !picture.buffer) {
		wlr_output_state_finish(&picture);
		return false;
	}
	struct wlr_texture *texture = wlr_texture_from_buffer(wo->renderer, picture.buffer);
	if (!texture) {
		wlr_output_state_finish(&picture);
		return false;
	}

	// the part around the pointer, enlarged
	struct wlr_box ob;
	wlr_output_layout_get_box(root->output_layout, wo, &ob);
	struct sway_seat *seat = input_manager_current_seat();
	double px = (seat->cursor->cursor->x - ob.x) * wo->scale;
	double py = (seat->cursor->cursor->y - ob.y) * wo->scale;
	if (wo->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		px = width / 2.0; // (turned screens zoom around their middle)
		py = height / 2.0;
	}
	double z = mag.level;
	double sw = width / z, sh = height / z;
	double sx = px - sw / 2, sy = py - sh / 2;
	double x0 = fmax(sx, 0), y0 = fmax(sy, 0);
	double x1 = fmin(sx + sw, width), y1 = fmin(sy + sh, height);

	struct wlr_render_pass *pass = wlr_output_begin_render_pass(wo, state, NULL);
	if (!pass) {
		wlr_texture_destroy(texture);
		wlr_output_state_finish(&picture);
		return false;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { 0, 0, width, height },
		.color = { 0.02f, 0.02f, 0.03f, 1.0f },
	});
	if (x1 > x0 && y1 > y0) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = { x0, y0, x1 - x0, y1 - y0 },
			.dst_box = {
				(int)lround((x0 - sx) * z), (int)lround((y0 - sy) * z),
				(int)lround((x1 - x0) * z), (int)lround((y1 - y0) * z),
			},
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		});
	}
	bool ok = wlr_render_pass_submit(pass);
	wlr_texture_destroy(texture);
	wlr_output_state_finish(&picture);
	return ok;
}
