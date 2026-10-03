#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include "keyboards.h"

static const char *str(json_object *obj, const char *key) {
	json_object *value;
	return obj && json_object_object_get_ex(obj, key, &value) ?
		json_object_get_string(value) : NULL;
}

static bool has(const char *text, const char *part) {
	if (!text) {
		return false;
	}
	size_t n = strlen(part);
	for (const char *p = text; *p; p++) {
		if (strncasecmp(p, part, n) == 0) {
			return true;
		}
	}
	return false;
}

static int score(json_object *input) {
	const char *type = str(input, "type");
	json_object *names;
	if (!type || strcmp(type, "keyboard") != 0 ||
			!json_object_object_get_ex(input, "xkb_layout_names", &names) ||
			!json_object_is_type(names, json_type_array)) {
		return -1;
	}
	const char *id = str(input, "identifier"), *name = str(input, "name");
	int s = (int)json_object_array_length(names) * 10;
	if (!has(id, "virtual") && !has(name, "virtual")) {
		s += 1000;
	}
	if ((has(id, "keyboard") || has(id, "kbd") || has(name, "keyboard")) &&
			!has(id, "virtual")) {
		s += 5; // a keyboard to type on, not a button that sends keys
	}
	return s;
}

json_object *keyboards_main(json_object *inputs) {
	if (!inputs || !json_object_is_type(inputs, json_type_array)) {
		return NULL;
	}
	json_object *best = NULL;
	int best_score = -1;
	for (size_t i = 0; i < json_object_array_length(inputs); i++) {
		json_object *input = json_object_array_get_idx(inputs, i);
		int s = score(input);
		if (s > best_score) {
			best_score = s;
			best = input;
		}
	}
	return best;
}
