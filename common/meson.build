lib_sway_common = static_library(
	'sway-common',
	files(
		'cairo.c',
		'gesture.c',
		'ipc-client.c',
		'log.c',
		'loop.c',
		'list.c',
		'pango.c',
		'stringop.c',
		'util.c',
		'tw_paths.c',
		'twconf.c',
		'tw_theme.c',
		'tw_desktop.c',
		'tw_icon_loader.c',
		'icon_theme.c',
	),
	dependencies: [
		cairo,
		gdk_pixbuf,
		rsvg,
		pango,
		pangocairo,
		wayland_client.partial_dependency(compile_args: true)
	],
	include_directories: sway_inc
)
