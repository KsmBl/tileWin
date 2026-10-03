#ifndef _TILEWIN_PANEL_KEYBOARDS_H
#define _TILEWIN_PANEL_KEYBOARDS_H
#include <json.h>

/*
 * Of the inputs tileWin lists (get_inputs), the keyboard whose layouts the
 * taskbar shows: not a virtual one (tileWin's own tools and others type
 * through those, always in English), the one with the most layouts, a real
 * keyboard before the power button and the hotkeys that count as keyboards
 * too. Borrowed from inputs; NULL when there is no keyboard.
 */
json_object *keyboards_main(json_object *inputs);

#endif
