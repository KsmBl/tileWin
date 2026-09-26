# the screen savers, also drawn by the preview of the settings app
lib_savers = static_library(
	'tilewin-savers', [
		'savers.c',
		'saver_tilewin.c',
		'saver_ad.c',
		'saver_classic.c',
		'saver_diggers.c',
		'saver_hellfire.c',
		'saver_matrix.c',
		'saver_maze.c',
		'saver_new.c',
		'saver_pipes.c',
		'saver_win7.c',
	],
	include_directories: [sway_inc],
	dependencies: [cairo, gdk_pixbuf, jsonc, math, pango, pangocairo],
	link_with: [lib_sway_common],
)

executable(
	'tilewin-screensaver', [
		'main.c',
		wl_protos_src,
	],
	include_directories: [sway_inc],
	dependencies: [cairo, gdk_pixbuf, math, pango, pangocairo, wayland_client],
	link_with: [lib_savers, lib_sway_common, lib_sway_client],
	install: true,
)
