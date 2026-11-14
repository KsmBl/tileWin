#ifndef _TILEWIN_PANEL_TEXTFIELD_H
#define _TILEWIN_PANEL_TEXTFIELD_H
#include "tw_textedit.h"

struct panel;

/* Sets the selection colors of the panel theme (text.selection_bg/fg). */
void text_style_colors(struct panel *panel, struct text_style *style);

#endif
