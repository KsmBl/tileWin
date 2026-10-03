/*
 * Which keyboard the taskbar shows the layouts of (panel/keyboards.c), with
 * lists of inputs as tileWin gives them. A virtual keyboard (tileWin's own
 * tools type through one, and it only ever has English) used to come first
 * in the list and hide the layouts of the real one and which is in use.
 */
#include <stdio.h>
#include <string.h>
#include "keyboards.h"

static int failures;

static void check(int ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += !ok;
}

static const char *chosen(const char *json) {
	static char id[128];
	json_object *inputs = json_tokener_parse(json);
	json_object *k = keyboards_main(inputs), *v;
	snprintf(id, sizeof(id), "%s", k && json_object_object_get_ex(k, "identifier", &v) ?
		json_object_get_string(v) : "(none)");
	json_object_put(inputs);
	return id;
}

#define KB(id, name, layouts, active) \
	"{\"identifier\": \"" id "\", \"name\": \"" name "\", \"type\": \"keyboard\", " \
	"\"xkb_layout_names\": [" layouts "], \"xkb_active_layout_index\": " #active "}"

int main(void) {
	// as on a laptop: the clipboard's virtual keyboard first, then the real one,
	// then buttons and hotkeys that count as keyboards
	const char *laptop = "["
		KB("0:0:wlr_virtual_keyboard_v1", "wlr_virtual_keyboard_v1", "\"English (US)\"", 0) ","
		KB("1:1:AT_Translated_Set_2_keyboard", "AT Translated Set 2 keyboard",
			"\"English (US)\", \"German\"", 1) ","
		KB("0:3:Sleep_Button", "Sleep Button", "\"English (US)\", \"German\"", 0) ","
		"{\"identifier\": \"1267:12:Touchpad\", \"type\": \"touchpad\"}"
		"]";
	check(strcmp(chosen(laptop), "1:1:AT_Translated_Set_2_keyboard") == 0,
		"the real keyboard, not the virtual one before it, nor a button with as many layouts");

	const char *one_layout = "["
		KB("0:1:Power_Button", "Power Button", "\"English (US)\"", 0) ","
		KB("1133:49948:Logitech_USB_Keyboard", "Logitech USB Keyboard", "\"English (US)\"", 0)
		"]";
	check(strcmp(chosen(one_layout), "1133:49948:Logitech_USB_Keyboard") == 0,
		"with one layout each, a keyboard before the power button");

	const char *more = "["
		KB("1:1:AT_Translated_Set_2_keyboard", "AT Translated Set 2 keyboard", "\"English (US)\"", 0) ","
		KB("1133:1:Other_Keyboard", "Other Keyboard", "\"English (US)\", \"French\", \"German\"", 2)
		"]";
	check(strcmp(chosen(more), "1133:1:Other_Keyboard") == 0, "the one with the most layouts");

	const char *only_virtual = "["
		KB("0:0:wlr_virtual_keyboard_v1", "wlr_virtual_keyboard_v1", "\"English (US)\"", 0) "]";
	check(strcmp(chosen(only_virtual), "0:0:wlr_virtual_keyboard_v1") == 0,
		"a virtual keyboard when there is nothing else (a nested session)");

	check(strcmp(chosen("[{\"identifier\": \"x\", \"type\": \"pointer\"}]"), "(none)") == 0,
		"no keyboard at all: none");
	check(keyboards_main(NULL) == NULL, "no list: none");

	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
