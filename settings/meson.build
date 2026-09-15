executable(
	'tilewin-settings', [
		'confdoc.c',
		'ipc.c',
		'main.c',
		'page_keyboard.c',
		'page_launcher.c',
		'page_menus.c',
		'page_mouse.c',
		'page_screen.c',
		'page_taskbar.c',
		'page_theme.c',
		'page_wallpaper.c',
		'ui.c',
	],
	include_directories: [sway_inc],
	dependencies: [gtk4, jsonc, cairo, gdk_pixbuf, math, rsvg, pango],
	link_with: [lib_sway_common],
	install: true,
)

install_data(
	'../data/tilewin-settings.desktop',
	install_dir: join_paths(datadir, 'applications'),
)
