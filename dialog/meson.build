# tilewin-dialog, the message box in the look of the theme, and the GTK 3 module
# that shows GTK's message dialogs as it
executable(
	'tilewin-dialog', [
		'main.c',
		# the look of the theme, as the settings have it, and the theme in use
		'../settings/themecss.c',
		'../settings/ipc.c',
	],
	include_directories: [sway_inc, include_directories('../settings')],
	dependencies: [gtk4, dependency('gtk4-wayland'), jsonc, math, pango],
	link_with: [lib_sway_common],
	install: true,
)

gtk3 = dependency('gtk+-3.0', required: false)
if gtk3.found()
	tilewin_gtk3_dialogs = shared_module(
		'tilewin-gtk3-dialogs',
		'gtk3module.c',
		dependencies: [gtk3, dependency('gmodule-2.0')],
		install: true,
		install_dir: join_paths(prefix, get_option('libdir'), 'tileWin'),
	)
else
	warning('gtk+-3.0 not found: GTK 3 apps keep their own message dialogs')
endif
