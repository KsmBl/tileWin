executable(
	'tilewin-nightlight', [
		'main.c',
		wl_protos_src,
	],
	include_directories: [sway_inc],
	dependencies: [math, wayland_client],
	link_with: [lib_sway_common],
	install: true,
)
