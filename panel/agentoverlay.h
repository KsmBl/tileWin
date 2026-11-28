#ifndef TW_AGENTOVERLAY_H
#define TW_AGENTOVERLAY_H

#include <glib.h>
#include "claudecharts.h"

/* Snapshots owned by the account widget for the duration of a draw. */
struct agent_overlay_series {
	char name[192];
	char legend[96];
	uint32_t color;
	const struct tw_claude_usage *usage;
	const struct tw_claude_window *weekly_gate;
	const struct tw_claude_point *points;
	int count;
};

void agent_overlay_draw(struct gadget_ctx *ctx, const struct gadget_palette *pal,
	const GArray *series);

#endif
