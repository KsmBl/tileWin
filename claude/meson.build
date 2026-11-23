# tilewin-claude-usage: the usage limits of Claude Code for the Claude widget, as
# Claude Code shows them to its status line
tilewin_claude_usage = executable(
	'tilewin-claude-usage',
	'tilewin-claude-usage.c',
	include_directories: [sway_inc],
	dependencies: [jsonc, cc.find_library('util', required: false)],
	link_with: [lib_sway_common],
	install: true,
)
