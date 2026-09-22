executable(
	'tilewin-clipboard', [
		'main.c',
		wl_protos_src,
	],
	include_directories: [sway_inc],
	dependencies: [cairo, jsonc, math, wayland_client, xkbcommon],
	link_with: [lib_sway_common],
	install: true,
)
