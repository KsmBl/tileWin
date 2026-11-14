#include "panel.h"
#include "textfield.h"

void text_style_colors(struct panel *panel, struct text_style *style) {
	uint32_t bg;
	switch (panel_style(panel)) {
	case PS_CLASSIC:
		bg = 0x000080ff;
		break;
	case PS_LUNA:
		bg = 0x316ac5ff;
		break;
	case PS_AERO:
		bg = 0x3399ffff;
		break;
	case PS_FLAT:
	case PS_FLUENT:
	default:
		bg = 0x0078d7ff;
		break;
	}
	style->selection_bg = tw_theme_color(panel->theme, "text.selection_bg", bg);
	style->selection_fg = tw_theme_color(panel->theme, "text.selection_fg", 0xffffffff);
}
