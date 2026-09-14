panel_tray_files = have_tray ? files(
	'tray/host.c',
	'tray/item.c',
	'tray/watcher.c',
) : []

panel_deps = [
	cairo,
	dependency('gio-2.0'),
	gdk_pixbuf,
	jsonc,
	math,
	pango,
	pangocairo,
	rsvg,
	rt,
	wayland_client,
	wayland_cursor,
	xkbcommon,
]
if have_tray
	panel_deps += sdbus
endif

executable(
	'tilewin-panel', [
		'apps.c',
		'bar.c',
		'config.c',
		'desktop.c',
		'draw.c',
		'flyouts.c',
		'ipc.c',
		'launcher.c',
		'main.c',
		'popup.c',
		'proc.c',
		'startmenu.c',
		'surface.c',
		'tooltip.c',
		'trayicons.c',
		'wayland.c',
		'widgets/basic.c',
		'widgets/custom.c',
		'widgets/system.c',
		'widgets/taskbar.c',
		'widgets/tray.c',
		panel_tray_files,
		wl_protos_src,
	],
	include_directories: [sway_inc, include_directories('.')],
	dependencies: panel_deps,
	link_with: [lib_sway_common, lib_sway_client],
	install: true,
)
