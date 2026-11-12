/*
 * The login screens of the old days: each theme looks as that Windows did when it
 * asked who you are. Included by greeter.c, for its drawing helpers. Everything
 * is drawn here, without any of Microsoft's logos or pictures.
 *
 *   Windows 10 and 11  a picture, the user in the middle, others bottom left
 *   Windows 8          the user beside the field, on a calm background
 *   Windows 7, Vista   a framed picture, a glass arrow, a red power button
 *   Windows XP         the blue Welcome screen, with a tile for each user
 *   Windows 95, 2000   a dialog on the wallpaper
 *   Windows 1, 3       the same in the plain frame of those days
 *   sway               the ring of swaylock
 */
#define POWER_POWEROFF 3
#define POWER_REBOOT 2
#define POWER_SUSPEND 1
#define POWER_HIBERNATE 0

static const char *const power_allow[] = { "allow_hibernate", "allow_suspend", "allow_reboot",
	"allow_poweroff" };

static bool power_on(int index) {
	return dm_config_bool(g.config, "show_power") && dm_config_bool(g.config, power_allow[index]) &&
		g.stage != STAGE_DONE;
}

static const char *user_shown(int i) {
	if (i >= g.user_count) {
		return "Another user";
	}
	return g.users[i].full && *g.users[i].full ? g.users[i].full : g.users[i].name;
}

static const char *shown_name(void) {
	if (!typing_name()) {
		return user_shown(g.user_sel);
	}
	return g.name_len ? g.name : "Log in";
}

static void square_avatar(cairo_t *cr, const struct user *u, double x, double y, double size) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, size, size);
	cairo_clip(cr);
	if (u && u->avatar) {
		double iw = cairo_image_surface_get_width(u->avatar);
		double ih = cairo_image_surface_get_height(u->avatar);
		double s = size / (iw < ih ? iw : ih);
		cairo_translate(cr, x + (size - iw * s) / 2, y + (size - ih * s) / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, u->avatar, 0, 0);
		cairo_paint(cr);
	} else {
		cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + size);
		cairo_pattern_add_color_stop_rgb(p, 0, 0.72, 0.82, 0.93);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.38, 0.55, 0.78);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
		cairo_new_path(cr);
		cairo_arc(cr, x + size / 2, y + size * 0.38, size * 0.2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, x + size / 2, y + size * 1.05, size * 0.42, M_PI, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* An arrow to the right, the sign of going on on all of them. */
static void arrow(cairo_t *cr, double cx, double cy, double s, uint32_t c) {
	color(cr, c);
	cairo_set_line_width(cr, s * 0.16);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_path(cr);
	cairo_move_to(cr, cx - s * 0.45, cy);
	cairo_line_to(cr, cx + s * 0.4, cy);
	cairo_move_to(cr, cx + s * 0.05, cy - s * 0.35);
	cairo_line_to(cr, cx + s * 0.4, cy);
	cairo_line_to(cr, cx + s * 0.05, cy + s * 0.35);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void power_glyph(cairo_t *cr, int index, double cx, double cy, double s, uint32_t c) {
	color(cr, c);
	cairo_set_line_width(cr, s * 0.1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_path(cr);
	if (index == POWER_POWEROFF) {
		cairo_arc(cr, cx, cy, s * 0.36, -M_PI / 2 + 0.6, 1.5 * M_PI - 0.6);
		cairo_stroke(cr);
		cairo_move_to(cr, cx, cy - s * 0.46);
		cairo_line_to(cr, cx, cy - s * 0.06);
		cairo_stroke(cr);
	} else if (index == POWER_REBOOT) {
		cairo_arc(cr, cx, cy, s * 0.36, -M_PI / 2 + 0.5, 1.5 * M_PI - 0.2);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - s * 0.05, cy - s * 0.52);
		cairo_line_to(cr, cx + s * 0.13, cy - s * 0.36);
		cairo_line_to(cr, cx - s * 0.05, cy - s * 0.2);
		cairo_stroke(cr);
	} else {
		// a moon; a little house of sleep for hibernating would say no more
		cairo_arc(cr, cx, cy, s * 0.36, 0.5, 2 * M_PI - 2.2);
		cairo_curve_to(cr, cx - s * 0.1, cy - s * 0.2, cx, cy + s * 0.2, cx + s * 0.32, cy + s * 0.17);
		cairo_stroke(cr);
	}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

/* The field of the password or of what PAM asks, with the arrow inside it or not. */
static void password_field(struct output *o, cairo_t *cr, double x, double y, double w, double h,
		double radius, bool arrow_inside) {
	double right = arrow_inside ? h : 0;
	box_path(cr, x, y, w, h, radius);
	color(cr, g.stage == STAGE_CHECKING || g.stage == STAGE_DONE ?
		(g.field_bg & 0xffffff00) | 0x90 : g.field_bg);
	cairo_fill(cr);
	if (g.focus == AREA_PASSWORD) {
		box_path(cr, x + 0.5, y + 0.5, w - 1, h - 1, radius);
		color(cr, g.accent);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
	}
	double inner = w - right - 20;
	bool reveal = dm_config_bool(g.config, "password_reveal") && g.password_len > 0 &&
		(g.stage != STAGE_PROMPT || g.prompt_secret);
	if (g.password_len == 0) {
		const char *hint = g.stage == STAGE_PROMPT ? g.prompt : "Password";
		text_in(cr, g.font, hint, x + 10, y, inner, h, (g.field_fg & 0xffffff00) | 0x88, LEFT);
	} else {
		draw_secret(cr, x + 10, y, inner - (reveal ? 24 : 0), h);
	}
	add_hit(o, x, y, w - right, h, AREA_PASSWORD, 0);
	if (reveal) {
		draw_eye(cr, x + w - right - 18, y + h / 2, (g.field_fg & 0xffffff00) | 0xa0, g.reveal);
		add_hit(o, x + w - right - 34, y, 34, h, AREA_REVEAL, 0);
	}
	if (arrow_inside) {
		box_path(cr, x + w - h, y, h, h, radius);
		color(cr, g.accent);
		cairo_fill(cr);
		arrow(cr, x + w - h / 2, y + h / 2, h * 0.5, 0xffffffff);
		add_hit(o, x + w - h, y, h, h, AREA_LOGIN, 0);
	}
}

static void name_field(struct output *o, cairo_t *cr, double x, double y, double w, double h,
		double radius) {
	box_path(cr, x, y, w, h, radius);
	color(cr, g.field_bg);
	cairo_fill(cr);
	if (g.focus == AREA_NAME) {
		box_path(cr, x + 0.5, y + 0.5, w - 1, h - 1, radius);
		color(cr, g.accent);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
	}
	text_in(cr, g.font, g.name_len ? g.name : "User name", x + 10, y, w - 20, h,
		g.name_len ? g.field_fg : (g.field_fg & 0xffffff00) | 0x88, LEFT);
	add_hit(o, x, y, w, h, AREA_NAME, 0);
}

static bool sessions_shown(void) {
	return dm_config_bool(g.config, "show_sessions") && g.sessions->count > 1;
}

static void session_row(struct output *o, cairo_t *cr, double x, double y, double w, double h,
		uint32_t c) {
	text_in(cr, g.bold, "\xe2\x80\xb9", x, y, 24, h, c, MIDDLE);
	text_in(cr, g.font, g.sessions->items[g.session_sel].name, x + 24, y, w - 48, h, c, MIDDLE);
	text_in(cr, g.bold, "\xe2\x80\xba", x + w - 24, y, 24, h, c, MIDDLE);
	if (g.focus == AREA_SESSION_NEXT) {
		box_path(cr, x + 0.5, y + 0.5, w - 1, h - 1, 3);
		color(cr, c);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	add_hit(o, x, y, w / 2, h, AREA_SESSION_PREV, 0);
	add_hit(o, x + w / 2, y, w / 2, h, AREA_SESSION_NEXT, 0);
}

static void layout_row(struct output *o, cairo_t *cr, double x, double y, double w, double h,
		uint32_t c) {
	const char *name = xkb_keymap_layout_get_name(g.keymap, g.group);
	char text[160];
	snprintf(text, sizeof(text), "Keyboard: %s", name ? name : "?");
	text_in(cr, g.font, text, x, y, w, h, c, MIDDLE);
	add_hit(o, x, y, w, h, AREA_LAYOUT, 0);
}

/* What happened, or Caps Lock. */
static const char *status_line(void) {
	if (g.stage == STAGE_CHECKING) {
		return "Checking...";
	}
	if (g.message[0]) {
		return g.message;
	}
	if (dm_config_bool(g.config, "show_caps_lock") && g.state &&
			xkb_state_mod_name_is_active(g.state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0) {
		return "Caps Lock is on";
	}
	return "";
}

/* Where the column stands: card_position counts here too. */
static double column_x(double W, double w) {
	const char *place = dm_config_str(g.config, "card_position");
	return strcmp(place, "left") == 0 ? W * 0.07 : strcmp(place, "right") == 0 ? W * 0.93 - w :
		(W - w) / 2;
}

/* ---------- Windows 10 and 11, and 8 ---------- */

/* The other users, bottom left. */
static void user_corner(struct output *o, cairo_t *cr, double H, double rowh) {
	if (g.user_count == 0) {
		return;
	}
	int total = g.user_count + 1, rows = total < 5 ? total : 5;
	int first = g.user_sel - rows / 2;
	first = first > total - rows ? total - rows : first;
	first = first < 0 ? 0 : first;
	double y = H - 22 - rows * rowh, x = 18, w = 260;
	for (int i = first; i < first + rows && i < total; i++) {
		if (i == g.user_sel) {
			box_path(cr, x, y, w, rowh - 2, g.look == LOOK_WIN11 ? 5 : 0);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
			cairo_fill(cr);
		}
		draw_avatar(cr, i < g.user_count ? &g.users[i] : NULL, x + rowh / 2, y + rowh / 2 - 1, rowh * 0.36);
		text_on_wallpaper(cr, g.font, user_shown(i), x + rowh + 2, y, w - rowh - 8, rowh - 2, LEFT);
		add_hit(o, x, y, w, rowh - 2, AREA_USER, i);
		y += rowh;
	}
}

static void power_corner(struct output *o, cairo_t *cr, double W, double H, double size) {
	double x = W - 18 - size;
	for (int i = POWER_POWEROFF; i >= POWER_HIBERNATE; i--) {
		if (power_on(i)) {
			power_glyph(cr, i, x + size / 2, H - 18 - size / 2, size * 0.6, 0xffffffff);
			add_hit(o, x, H - 18 - size, size, size, AREA_POWER, i);
			x -= size + 4;
		}
	}
}

static void draw_modern(struct output *o, cairo_t *cr, double W, double H) {
	bool eleven = g.look == LOOK_WIN11;
	double field_w = 296, field_h = eleven ? 34 : 36, radius = eleven ? 5 : 0;
	double r = H * 0.1 < 60 ? 60 : H * 0.1 > 96 ? 96 : H * 0.1;
	double x = column_x(W, field_w), cx = x + field_w / 2;
	double y = H * (eleven ? 0.3 : 0.24);
	const struct user *u = typing_name() ? NULL : &g.users[g.user_sel];
	draw_avatar(cr, u, cx, y + r, r);
	y += 2 * r + 12;
	text_on_wallpaper(cr, "Segoe UI Light, Noto Sans Light, Sans 26", shown_name(), x - 150, y,
		field_w + 300, 46, MIDDLE);
	y += 56;
	if (g.stage == STAGE_DONE) {
		text_on_wallpaper(cr, "Segoe UI Light, Noto Sans Light, Sans 16", "Welcome", x, y, field_w, 34, MIDDLE);
		return;
	}
	if (typing_name()) {
		name_field(o, cr, x, y, field_w, field_h, radius);
		y += field_h + 8;
	}
	password_field(o, cr, x, y, field_w, field_h, radius, true);
	y += field_h + 10;
	if (*status_line()) {
		text_on_wallpaper(cr, g.font, status_line(), x - 80, y, field_w + 160, 24, MIDDLE);
	}
	y += 30;
	if (sessions_shown()) {
		text_on_wallpaper(cr, g.font, "", x, y, field_w, 26, MIDDLE);
		session_row(o, cr, x, y, field_w, 26, 0xffffffff);
		y += 30;
	}
	if (has_layouts()) {
		layout_row(o, cr, x, y, field_w, 24, 0xffffffff);
	}
	user_corner(o, cr, H, 52);
	power_corner(o, cr, W, H, 38);
}

static void draw_win8(struct output *o, cairo_t *cr, double W, double H) {
	// the user beside the field, and only flat shapes
	double pic = 130, gap = 18, field_w = 300, field_h = 34;
	double total = pic + gap + field_w;
	double x = column_x(W, total), y = H * 0.36;
	const struct user *u = typing_name() ? NULL : &g.users[g.user_sel];
	square_avatar(cr, u, x, y, pic);
	double tx = x + pic + gap;
	text_on_wallpaper(cr, "Segoe UI Light, Noto Sans Light, Sans 22", shown_name(), tx, y, field_w, 44, LEFT);
	double fy = y + 50;
	if (g.stage == STAGE_DONE) {
		text_on_wallpaper(cr, g.font, "Welcome", tx, fy, field_w, field_h, LEFT);
		return;
	}
	if (typing_name()) {
		name_field(o, cr, tx, fy, field_w, field_h, 0);
		fy += field_h + 6;
	}
	password_field(o, cr, tx, fy, field_w, field_h, 0, true);
	fy += field_h + 6;
	if (*status_line()) {
		text_on_wallpaper(cr, g.font, status_line(), tx, fy, field_w, 22, LEFT);
	}
	fy += 26;
	if (sessions_shown()) {
		session_row(o, cr, tx, fy, field_w, 26, 0xffffffff);
		fy += 28;
	}
	if (has_layouts()) {
		layout_row(o, cr, tx, fy, field_w, 24, 0xffffffff);
	}
	user_corner(o, cr, H, 50);
	power_corner(o, cr, W, H, 38);
}

/* ---------- Windows 7 and Vista ---------- */

static void glass_frame(cairo_t *cr, double x, double y, double size) {
	box_path(cr, x - 9, y - 9, size + 18, size + 18, 6);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void glass_arrow(struct output *o, cairo_t *cr, double x, double y, double size) {
	cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + size);
	cairo_pattern_add_color_stop_rgb(p, 0, 0.55, 0.78, 0.98);
	cairo_pattern_add_color_stop_rgb(p, 0.5, 0.15, 0.45, 0.82);
	cairo_pattern_add_color_stop_rgb(p, 1, 0.1, 0.32, 0.65);
	cairo_new_path(cr);
	cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
	cairo_set_source(cr, p);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	arrow(cr, x + size / 2, y + size / 2, size * 0.52, 0xffffffff);
	add_hit(o, x, y, size, size, AREA_LOGIN, 0);
}

/* The red button with the sign of power, and smaller ones beside it. */
static void red_power(struct output *o, cairo_t *cr, double W, double H) {
	double x = W - 28, y = H - 54, h = 28;
	if (power_on(POWER_POWEROFF)) {
		x -= 44;
		cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
		cairo_pattern_add_color_stop_rgb(p, 0, 0.93, 0.48, 0.38);
		cairo_pattern_add_color_stop_rgb(p, 0.5, 0.79, 0.2, 0.13);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.55, 0.08, 0.05);
		box_path(cr, x, y, 44, h, 3);
		cairo_set_source(cr, p);
		cairo_fill_preserve(cr);
		cairo_pattern_destroy(p);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		power_glyph(cr, POWER_POWEROFF, x + 22, y + h / 2, 18, 0xffffffff);
		add_hit(o, x, y, 44, h, AREA_POWER, POWER_POWEROFF);
	}
	for (int i = POWER_REBOOT; i >= POWER_HIBERNATE; i--) {
		if (power_on(i)) {
			x -= 40;
			box_path(cr, x, y, 34, h, 3);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			power_glyph(cr, i, x + 17, y + h / 2, 17, 0xffffffff);
			add_hit(o, x, y, 34, h, AREA_POWER, i);
		}
	}
}

static void draw_aero(struct output *o, cairo_t *cr, double W, double H) {
	double pic = 128, field_w = 226, field_h = 26, arrow_size = 30;
	double x = column_x(W, field_w + arrow_size + 8), cx = x + field_w / 2;
	double y = H * 0.27;
	const struct user *u = typing_name() ? NULL : &g.users[g.user_sel];
	glass_frame(cr, cx - pic / 2, y, pic);
	square_avatar(cr, u, cx - pic / 2, y, pic);
	y += pic + 18;
	text_on_wallpaper(cr, "Segoe UI, Noto Sans, Sans 15", shown_name(), x - 100, y, field_w + 200, 30, MIDDLE);
	y += 36;
	if (g.stage == STAGE_DONE) {
		text_on_wallpaper(cr, "Segoe UI, Noto Sans, Sans 14", "Welcome", x - 100, y, field_w + 200, 30, MIDDLE);
		return;
	}
	if (typing_name()) {
		name_field(o, cr, x, y, field_w, field_h, 2);
		y += field_h + 6;
	}
	password_field(o, cr, x, y, field_w, field_h, 2, false);
	glass_arrow(o, cr, x + field_w + 8, y + field_h / 2 - arrow_size / 2, arrow_size);
	y += field_h + 10;
	if (*status_line()) {
		text_on_wallpaper(cr, g.font, status_line(), x - 80, y, field_w + 160, 22, MIDDLE);
	}
	y += 26;
	if (g.user_count > 0) {
		// Switch User, as a plain button under the field
		double bw = 110, bx = cx - bw / 2;
		box_path(cr, bx, y, bw, 24, 3);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		text_in(cr, g.font, "Switch user", bx, y, bw, 24, 0xffffffff, MIDDLE);
		add_hit(o, bx, y, bw, 24, AREA_USER, (g.user_sel + 1) % (g.user_count + 1));
		y += 32;
	}
	if (sessions_shown()) {
		session_row(o, cr, x - 20, y, field_w + 40, 24, 0xffffffff);
		y += 28;
	}
	if (has_layouts()) {
		layout_row(o, cr, x, y, field_w, 22, 0xffffffff);
	}
	// the name of the system at the bottom, in the middle of the screen
	char system[128];
	snprintf(system, sizeof(system), "tileWin");
	text_on_wallpaper(cr, "Segoe UI Light, Noto Sans Light, Sans 19", system, 0, H - 62, W, 40, MIDDLE);
	red_power(o, cr, W, H);
}

/* ---------- Windows XP ---------- */

static void xp_band(cairo_t *cr, double y, double h, double r, double gr, double b) {
	cairo_rectangle(cr, 0, y, 100000, h);
	cairo_set_source_rgb(cr, r, gr, b);
	cairo_fill(cr);
}

/* how tall the tile of user i is: the chosen one is taller, for its password */
static double xp_tile_h(int i) {
	if (i == g.user_sel || g.user_count == 0) {
		return typing_name() ? 138 : 104;
	}
	return 70;
}

static void draw_xp(struct output *o, cairo_t *cr, double W, double H) {
	bool dark = g.theme && g.theme->dark;
	const char *wallpaper = dm_config_str(g.config, "wallpaper");
	double band = H * 0.135, mid_y = band, mid_h = H - 2 * band;
	if (strcmp(wallpaper, "theme") == 0) {
		// the blue of the Welcome screen, lighter in the upper left
		cairo_pattern_t *p = cairo_pattern_create_radial(W * 0.2, H * 0.25, 10, W * 0.2, H * 0.25, W * 0.8);
		if (dark) {
			cairo_pattern_add_color_stop_rgb(p, 0, 0.26, 0.28, 0.33);
			cairo_pattern_add_color_stop_rgb(p, 1, 0.12, 0.13, 0.16);
		} else {
			cairo_pattern_add_color_stop_rgb(p, 0, 0.56, 0.69, 0.92);
			cairo_pattern_add_color_stop_rgb(p, 1, 0.33, 0.47, 0.82);
		}
		cairo_rectangle(cr, 0, mid_y, W, mid_h);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
	}
	double tr = dark ? 0.08 : 0.0, tg = dark ? 0.09 : 0.19, tb = dark ? 0.11 : 0.6;
	xp_band(cr, 0, band, tr, tg, tb);
	xp_band(cr, H - band, band, tr, tg, tb);
	// the thin lines at the edges of the bands: white on top, orange below
	cairo_pattern_t *line = cairo_pattern_create_linear(0, 0, W, 0);
	cairo_pattern_add_color_stop_rgba(line, 0, 1, 1, 1, 0.1);
	cairo_pattern_add_color_stop_rgba(line, 0.5, 1, 1, 1, 0.75);
	cairo_pattern_add_color_stop_rgba(line, 1, 1, 1, 1, 0.1);
	cairo_rectangle(cr, 0, band - 2, W, 2);
	cairo_set_source(cr, line);
	cairo_fill(cr);
	cairo_pattern_destroy(line);
	line = cairo_pattern_create_linear(0, 0, W, 0);
	cairo_pattern_add_color_stop_rgba(line, 0, 0.95, 0.55, 0.15, 0.1);
	cairo_pattern_add_color_stop_rgba(line, 0.5, 1, 0.68, 0.25, 1);
	cairo_pattern_add_color_stop_rgba(line, 1, 0.95, 0.55, 0.15, 0.1);
	cairo_rectangle(cr, 0, H - band, W, 2);
	cairo_set_source(cr, line);
	cairo_fill(cr);
	cairo_pattern_destroy(line);
	// the line down the middle, fading at both ends
	line = cairo_pattern_create_linear(0, mid_y, 0, mid_y + mid_h);
	cairo_pattern_add_color_stop_rgba(line, 0, 1, 1, 1, 0);
	cairo_pattern_add_color_stop_rgba(line, 0.5, 1, 1, 1, 0.7);
	cairo_pattern_add_color_stop_rgba(line, 1, 1, 1, 1, 0);
	cairo_rectangle(cr, W / 2, mid_y + 12, 1, mid_h - 24);
	cairo_set_source(cr, line);
	cairo_fill(cr);
	cairo_pattern_destroy(line);

	// left of the line, what to do
	double lw = W / 2 - 34;
	text_on_wallpaper(cr, "Franklin Gothic Medium, Noto Sans Italic, Sans Italic 30", "tileWin", 0,
		H * 0.36, lw, 60, RIGHT);
	const char *help = g.stage == STAGE_DONE ? "Welcome" : "To begin, click your user name";
	text_on_wallpaper(cr, "Tahoma, Noto Sans, Sans 13", help, 0, H * 0.36 + 62, lw, 28, RIGHT);

	// right of it, a tile for each user: the list has its middle at the middle of the
	// screen, as on XP; when it is taller than the room between the bands, it starts
	// under the upper band and goes as far down as needed for the chosen one to show
	double x = W / 2 + 28, w = W / 2 - 60 < 380 ? W / 2 - 60 : 380;
	int total = g.user_count > 0 ? g.user_count + 1 : 1;
	double gap = 8, top = mid_y + 34, bottom = H - band - 4, list_h = -gap, sel_end = 0;
	for (int i = 0; i < total; i++) {
		list_h += xp_tile_h(i) + gap;
		if (i == g.user_sel) {
			sel_end = list_h;
		}
	}
	double y = H / 2 - list_h / 2;
	int first = 0;
	if (y < top) {
		y = top;
		for (double skipped = 0; first < total - 1 && y + sel_end - skipped > bottom; first++) {
			skipped += xp_tile_h(first) + gap;
		}
	}
	for (int i = first; i < total && y + xp_tile_h(i) <= bottom; i++) {
		bool sel = i == g.user_sel || g.user_count == 0;
		double th = xp_tile_h(i);
		if (sel) {
			cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + w, 0);
			cairo_pattern_add_color_stop_rgba(p, 0, 0, 0.14, 0.5, 0.85);
			cairo_pattern_add_color_stop_rgba(p, 1, 0, 0.14, 0.5, 0);
			box_path(cr, x, y, w, th, 6);
			cairo_set_source(cr, p);
			cairo_fill(cr);
			cairo_pattern_destroy(p);
		}
		const struct user *u = i < g.user_count ? &g.users[i] : NULL;
		double pic = 48, px = x + 10, py = y + 11;
		cairo_rectangle(cr, px - 2, py - 2, pic + 4, pic + 4);
		if (sel) {
			cairo_set_source_rgb(cr, 1, 0.8, 0.3);
		} else {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
		}
		cairo_fill(cr);
		square_avatar(cr, u, px, py, pic);
		text_on_wallpaper(cr, "Tahoma, Noto Sans, Sans Bold 12", g.user_count ? user_shown(i) : "Log in",
			px + pic + 12, py, w - pic - 30, sel ? 24 : pic, LEFT);
		add_hit(o, x, y, w, th, AREA_USER, i);
		if (sel && g.stage != STAGE_DONE) {
			double fx = px + pic + 12, fy = py + 26, fw = w - pic - 76, fh = 24;
			if (typing_name()) {
				name_field(o, cr, fx, fy, fw, fh, 2);
				fy += fh + 6;
			}
			password_field(o, cr, fx, fy, fw, fh, 2, false);
			// the green arrow
			box_path(cr, fx + fw + 8, fy, 26, fh, 3);
			cairo_set_source_rgb(cr, 0.24, 0.62, 0.22);
			cairo_fill(cr);
			arrow(cr, fx + fw + 21, fy + fh / 2, 15, 0xffffffff);
			add_hit(o, fx + fw + 8, fy, 26, fh, AREA_LOGIN, 0);
			fy += fh + 4;
			if (*status_line()) {
				text_on_wallpaper(cr, g.font, status_line(), fx, fy, w - pic - 30, 20, LEFT);
			}
		}
		y += th + gap;
	}

	// below, the way out on the left and what else there is on the right
	double by = H - band + 18, bx = 30;
	static const char *const words[] = { "Hibernate", "Sleep", "Restart", "Turn off computer" };
	for (int i = POWER_POWEROFF; i >= POWER_HIBERNATE; i--) {
		if (!power_on(i)) {
			continue;
		}
		box_path(cr, bx, by, 28, 28, 4);
		if (i == POWER_POWEROFF) {
			cairo_set_source_rgb(cr, 0.84, 0.28, 0.16);
		} else {
			cairo_set_source_rgb(cr, 0.2, 0.46, 0.82);
		}
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		power_glyph(cr, i, bx + 14, by + 14, 18, 0xffffffff);
		PangoLayout *layout = make_layout(cr, "Tahoma, Noto Sans, Sans 10", words[i]);
		int tw, th;
		pango_layout_get_pixel_size(layout, &tw, &th);
		g_object_unref(layout);
		text_on_wallpaper(cr, "Tahoma, Noto Sans, Sans 10", words[i], bx + 34, by, tw + 4, 28, LEFT);
		add_hit(o, bx, by, 34 + tw + 4, 28, AREA_POWER, i);
		bx += 34 + tw + 18;
	}
	double rx = W - 340, rw = 310;
	const char *greeting = dm_config_str(g.config, "greeting");
	double ry = by - 8;
	if (*greeting) {
		text_on_wallpaper(cr, "Tahoma, Noto Sans, Sans 9", greeting, rx, ry, rw, 20, RIGHT);
		ry += 22;
	}
	if (sessions_shown()) {
		session_row(o, cr, rx, ry, rw, 24, 0xffffffff);
		ry += 26;
	}
	if (has_layouts()) {
		layout_row(o, cr, rx, ry, rw, 20, 0xffffffff);
	}
}

/* ---------- Windows 95, 2000, and the older ones ---------- */

/* A button as they were: raised, with a black frame around the default one. */
static void old_button(struct output *o, cairo_t *cr, double x, double y, double w, double h,
		const char *label, bool is_default, enum area area, int index) {
	if (g.look == LOOK_OLD) {
		box_path(cr, x, y, w, h, 3);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, is_default ? 2 : 1);
		cairo_stroke(cr);
		text_in(cr, g.font, label, x, y, w, h, 0x000000ff, MIDDLE);
	} else {
		if (is_default) {
			cairo_rectangle(cr, x - 1, y - 1, w + 2, h + 2);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_fill(cr);
		}
		cairo_rectangle(cr, x, y, w, h);
		color(cr, g.card_bg);
		cairo_fill(cr);
		bevel(cr, x, y, w, h, false);
		text_in(cr, g.font, label, x, y, w, h, g.fg, MIDDLE);
	}
	add_hit(o, x, y, w, h, area, index);
}

/* The field of the old looks: sunken, or a black line on the very first ones. */
static void old_field_frame(cairo_t *cr, double x, double y, double w, double h) {
	if (g.look == LOOK_OLD) {
		cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		bevel(cr, x, y, w, h, true);
	}
}

/* The keys on the dialog of Windows 95. */
static void key_icon(cairo_t *cr, double x, double y, double s) {
	cairo_set_source_rgb(cr, 0.86, 0.76, 0.2);
	cairo_set_line_width(cr, s * 0.11);
	cairo_new_path(cr);
	cairo_arc(cr, x + s * 0.3, y + s * 0.3, s * 0.2, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, x + s * 0.44, y + s * 0.44);
	cairo_line_to(cr, x + s * 0.92, y + s * 0.92);
	cairo_move_to(cr, x + s * 0.7, y + s * 0.7);
	cairo_line_to(cr, x + s * 0.58, y + s * 0.82);
	cairo_move_to(cr, x + s * 0.84, y + s * 0.84);
	cairo_line_to(cr, x + s * 0.72, y + s * 0.96);
	cairo_stroke(cr);
}

static void draw_dialog(struct output *o, cairo_t *cr, double W, double H) {
	bool oldest = g.look == LOOK_OLD;
	bool banner = g.theme && g.theme->name && strcmp(g.theme->name, "win2000") == 0;
	double title_h = oldest ? 22 : 20, banner_h = banner ? 62 : 0;
	double w = 430;
	double label_w = 88, icon_w = banner || oldest ? 0 : 58;
	bool sessions = sessions_shown(), layouts = has_layouts();
	const char *greeting = dm_config_str(g.config, "greeting");
	double rows = 2 + (sessions ? 1 : 0) + (layouts ? 1 : 0);
	double h = title_h + banner_h + 14 + (*greeting ? 22 : 0) + rows * 30 + 26 + 34 + 14;
	double x = column_x(W, w), y = (H - h) / 2;

	// the frame
	if (oldest) {
		cairo_rectangle(cr, x + 4, y + 4, w, h);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.9);
		cairo_fill(cr);
		cairo_rectangle(cr, x, y, w, h);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill(cr);
		cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		cairo_rectangle(cr, x, y, w, h);
		color(cr, g.card_bg);
		cairo_fill(cr);
		bevel(cr, x, y, w, h, false);
		bevel(cr, x + 1, y + 1, w - 2, h - 2, false);
	}

	// the title bar
	double tx = x + (oldest ? 0.5 : 3), ty = y + (oldest ? 0.5 : 3), tw = w - (oldest ? 1 : 6);
	cairo_save(cr);
	cairo_rectangle(cr, tx, ty, tw, title_h);
	cairo_clip(cr);
	color(cr, g.title_bg);
	cairo_paint(cr);
	if (g.gradient) {
		cairo_pattern_t *p = cairo_pattern_create_linear(tx, 0, tx + tw, 0);
		char *end;
		for (const char *s = g.gradient; *s;) {
			double at = strtod(s, &end);
			if (end == s || *end != ':') {
				break;
			}
			uint32_t c = 0x000000ff;
			char word[16];
			size_t n = strcspn(end + 1, " ");
			snprintf(word, sizeof(word), "%.*s", (int)n, end + 1);
			tw_parse_color(word, &c);
			cairo_pattern_add_color_stop_rgb(p, at, (c >> 24 & 0xff) / 255.0,
				(c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0);
			s = end + 1 + n;
			s += strspn(s, " ");
		}
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
	}
	cairo_restore(cr);
	char title[256];
	card_title(title, sizeof(title));
	text_in(cr, g.bold, title, tx + 6, ty, tw - 12, title_h, g.title_fg, oldest ? MIDDLE : LEFT);

	double top = ty + title_h;
	if (banner) {
		// the band of Windows 2000, with the name on it
		cairo_pattern_t *p = cairo_pattern_create_linear(tx, 0, tx + tw, 0);
		cairo_pattern_add_color_stop_rgb(p, 0, 0.05, 0.13, 0.42);
		cairo_pattern_add_color_stop_rgb(p, 1, 0.42, 0.56, 0.82);
		cairo_rectangle(cr, tx, top, tw, banner_h);
		cairo_set_source(cr, p);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		text_in(cr, "Georgia, Noto Serif, Serif Bold 20", "tileWin", tx + 16, top, tw - 30, banner_h,
			0xffffffff, LEFT);
		top += banner_h;
	}
	uint32_t ink = oldest ? 0x000000ff : g.fg;
	double cy = top + 14, left = x + 14;
	if (icon_w > 0) {
		key_icon(cr, left, cy + 2, 40);
		left += icon_w;
	}
	double field_x = left + label_w, field_w = x + w - 14 - field_x;
	if (*greeting) {
		text_in(cr, g.font, greeting, left, cy - 2, x + w - 14 - left, 20, ink, LEFT);
		cy += 22;
	}

	// User name: typed, or the one on the list, and a click for the next
	text_in(cr, g.font, "User name:", left, cy, label_w, 24, ink, LEFT);
	if (typing_name()) {
		name_field(o, cr, field_x, cy, field_w, 24, 0);
	} else {
		cairo_rectangle(cr, field_x, cy, field_w, 24);
		color(cr, g.field_bg);
		cairo_fill(cr);
		text_in(cr, g.font, g.users[g.user_sel].name, field_x + 6, cy, field_w - 30, 24, g.field_fg, LEFT);
		text_in(cr, g.font, "\xe2\x96\xbe", field_x + field_w - 22, cy, 18, 24, g.field_fg, MIDDLE);
		add_hit(o, field_x, cy, field_w, 24, AREA_USER, (g.user_sel + 1) % (g.user_count + 1));
	}
	old_field_frame(cr, field_x, cy, field_w, 24);
	cy += 30;
	text_in(cr, g.font, g.stage == STAGE_PROMPT ? g.prompt : "Password:", left, cy, label_w, 24, ink, LEFT);
	password_field(o, cr, field_x, cy, field_w, 24, 0, false);
	old_field_frame(cr, field_x, cy, field_w, 24);
	cy += 30;
	if (sessions) {
		text_in(cr, g.font, "Log on to:", left, cy, label_w, 24, ink, LEFT);
		session_row(o, cr, field_x, cy, field_w, 24, ink);
		cy += 30;
	}
	if (layouts) {
		layout_row(o, cr, field_x, cy, field_w, 24, ink);
		cy += 30;
	}
	text_in(cr, g.font, status_line(), left, cy, x + w - 14 - left, 22,
		g.message_error ? 0xc00000ff : ink, LEFT);
	cy += 26;

	// the buttons, on the right as they were
	double bw = 78, bh = 26, bx = x + w - 14;
	static const char *const words[] = { "Hibernate", "Sleep", "Restart", "Shut Down" };
	for (int i = POWER_POWEROFF; i >= POWER_REBOOT; i--) {
		if (power_on(i)) {
			bx -= bw;
			old_button(o, cr, bx, cy, bw, bh, words[i], false, AREA_POWER, i);
			bx -= 6;
		}
	}
	bx -= bw;
	old_button(o, cr, bx, cy, bw, bh, "Cancel", false, AREA_CANCEL, 0);
	bx -= 6 + bw;
	old_button(o, cr, bx, cy, bw, bh, g.stage == STAGE_PROMPT ? "Go on" : "OK", true, AREA_LOGIN, 0);
}

/* ---------- sway ---------- */

/* The ring of swaylock: it lights up a little further with each key. */
static void draw_ring(struct output *o, cairo_t *cr, double W, double H) {
	double r = 92, thick = 13, cx = column_x(W, 2 * r) + r, cy = H / 2 - 40;
	bool wrong = g.message_error && g.message[0];
	bool busy = g.stage == STAGE_CHECKING || g.stage == STAGE_DONE;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 0.12, 0.12, 0.18, 0.75);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	color(cr, wrong ? 0xf38ba8ff : busy ? 0x89b4faff : (g.accent & 0xffffff00) | 0x70);
	cairo_set_line_width(cr, thick);
	cairo_stroke(cr);
	size_t chars = 0;
	for (size_t i = 0; i < g.password_len; i++) {
		chars += ((unsigned char)g.password[i] & 0xc0) != 0x80;
	}
	if (chars > 0 && !busy) {
		double at = -M_PI / 2 + chars * 0.55;
		cairo_new_path(cr);
		cairo_arc(cr, cx, cy, r, at, at + 0.9);
		color(cr, g.accent);
		cairo_stroke(cr);
	}
	add_hit(o, cx - r, cy - r, 2 * r, 2 * r, AREA_PASSWORD, 0);
	const char *inside = g.stage == STAGE_PROMPT ? g.prompt : busy ? "..." : shown_name();
	text_in(cr, g.bold, inside, cx - r + 18, cy - 16, 2 * r - 36, 32, 0xcdd6f4ff, MIDDLE);
	double y = cy + r + 22, w = 300, x = cx - w / 2;
	if (typing_name() && g.stage == STAGE_IDLE) {
		name_field(o, cr, x, y, w, 30, 6);
		y += 38;
	} else if (g.user_count > 0) {
		text_in(cr, g.font, "Up and Down for another user", x, y, w, 22, 0xa6adc8ff, MIDDLE);
		add_hit(o, x, y, w, 22, AREA_USER, (g.user_sel + 1) % (g.user_count + 1));
		y += 26;
	}
	if (*status_line()) {
		text_in(cr, g.font, status_line(), x - 60, y, w + 120, 24, wrong ? 0xf38ba8ff : 0xcdd6f4ff, MIDDLE);
		y += 26;
	}
	if (sessions_shown()) {
		session_row(o, cr, x, y, w, 24, 0xcdd6f4ff);
		y += 28;
	}
	if (has_layouts()) {
		layout_row(o, cr, x, y, w, 22, 0xcdd6f4ff);
	}
	power_corner(o, cr, W, H, 34);
}

static void draw_login(struct output *o, cairo_t *cr, double W, double H) {
	switch (g.look) {
	case LOOK_WIN10:
	case LOOK_WIN11:
		draw_modern(o, cr, W, H);
		break;
	case LOOK_WIN8:
		draw_win8(o, cr, W, H);
		break;
	case LOOK_AERO:
		draw_aero(o, cr, W, H);
		break;
	case LOOK_XP:
		draw_xp(o, cr, W, H);
		break;
	case LOOK_DIALOG:
	case LOOK_OLD:
		draw_dialog(o, cr, W, H);
		break;
	case LOOK_SWAY:
		draw_ring(o, cr, W, H);
		break;
	case LOOK_CARD:
		draw_card(o, cr, W, H);
		draw_power(o, cr, W, H);
		break;
	}
}
