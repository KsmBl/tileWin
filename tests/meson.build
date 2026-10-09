glib = dependency('glib-2.0')

test(
	'twconf',
	executable(
		'test-twconf',
		'test_twconf.c',
		include_directories: sway_inc,
		link_with: lib_sway_common,
		dependencies: [cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	suite: 'unit',
)

test(
	'accent',
	executable(
		'test-accent',
		'test_accent.c',
		include_directories: sway_inc,
		link_with: lib_sway_common,
		dependencies: [cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	env: { 'TILEWIN_DATADIR': meson.project_source_root() },
	suite: 'unit',
)

test(
	'grid',
	executable(
		'test-grid',
		['test_grid.c', '../common/tw_grid.c'],
		include_directories: sway_inc,
		dependencies: [math],
	),
	suite: 'unit',
)

test(
	'chart-scale',
	executable(
		'test-chart-scale',
		['test_chart_scale.c', '../panel/draw.c'],
		include_directories: [sway_inc, include_directories('../panel')],
		link_with: lib_sway_common,
		dependencies: [cairo, pango, pangocairo, math],
	),
	suite: 'unit',
)

test(
	'keyboards',
	executable(
		'test-keyboards',
		['test_keyboards.c', '../panel/keyboards.c'],
		include_directories: include_directories('../panel'),
		dependencies: [jsonc],
	),
	suite: 'unit',
)

test(
	'themes',
	executable(
		'test-themes',
		'test_themes.c',
		include_directories: sway_inc,
		link_with: lib_sway_common,
		dependencies: [cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	# the themes of the source tree, and no user themes beside them
	env: {
		'TILEWIN_DATADIR': meson.project_source_root(),
		'XDG_CONFIG_HOME': meson.current_build_dir() / 'no-config',
	},
	suite: 'unit',
)

test(
	'confdoc',
	executable(
		'test-confdoc',
		['test_confdoc.c', '../settings/confdoc.c'],
		include_directories: [sway_inc, include_directories('../settings')],
		link_with: lib_sway_common,
		dependencies: [glib, cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	suite: 'unit',
)

test(
	'disks',
	executable(
		'test-disks',
		'test_disks.c',
		include_directories: sway_inc,
		link_with: lib_sway_common,
		dependencies: [cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	suite: 'unit',
)

test(
	'widgets',
	executable(
		'test-widgets',
		'test_widgets.c',
		include_directories: sway_inc,
		link_with: lib_sway_common,
		dependencies: [cairo, gdk_pixbuf, pango, pangocairo, rsvg, math],
	),
	suite: 'unit',
)

# the savers as the programs find them: the directories built here
saver_gui_env = ['TILEWIN_SAVERS=' + meson.project_build_root() / 'screensaver' / 'savers']
saver_test_env = ['HOME=/nonexistent', 'XDG_CONFIG_HOME=/nonexistent',
	'XDG_DATA_HOME=/nonexistent', 'TILEWIN_DATADIR=/nonexistent',
	'TILEWIN_SAVERS=' + meson.project_build_root() / 'screensaver' / 'savers']
test(
	'savers',
	executable(
		'test-savers',
		'test_savers.c',
		include_directories: [sway_inc, saver_inc],
		link_whole: [lib_saver_kit],
		link_with: [lib_sway_common],
		dependencies: saver_deps,
		export_dynamic: true,
	),
	args: [files('../config/taskbar.conf')],
	env: saver_test_env,
	depends: saver_modules,
	suite: 'unit',
)

# a directory of a saver of one's own, dropped next to them, is found, listed and runs
test(
	'saver-directories',
	executable(
		'test-saver-directories',
		'test_saver_directories.c',
		include_directories: [sway_inc, saver_inc],
		link_whole: [lib_saver_kit],
		link_with: [lib_sway_common],
		dependencies: saver_deps,
		export_dynamic: true,
	),
	args: [meson.project_build_root() / 'screensaver' / 'savers',
		meson.project_build_root() / 'screensaver' / 'example', screensaver_exe],
	env: ['HOME=/nonexistent', 'XDG_CONFIG_HOME=/nonexistent', 'TILEWIN_DATADIR=/nonexistent'],
	depends: saver_modules + [example_saver],
	suite: 'unit',
)

test(
	'blizzard',
	executable(
		'test-blizzard',
		['test_blizzard.c', doomsday_sources],
		include_directories: [sway_inc, saver_inc, include_directories('../screensaver/savers/doomsday')],
		link_with: [lib_saver_kit, lib_sway_common],
		dependencies: saver_deps,
	),
	env: saver_test_env,
	suite: 'unit',
	timeout: 120,
)

test(
	'doomsday-decay-jungle',
	executable(
		'test-doomsday',
		['test_doomsday.c', doomsday_sources],
		include_directories: [sway_inc, saver_inc, include_directories('../screensaver/savers/doomsday')],
		link_with: [lib_saver_kit, lib_sway_common],
		dependencies: saver_deps,
	),
	env: saver_test_env,
	suite: 'unit',
	timeout: 240,
)

test(
	'focus-priority',
	executable(
		'test-priority',
		['test_priority.c', '../sway/wm/priority.c'],
		include_directories: sway_inc,
	),
	suite: 'unit',
)

test(
	'reorder',
	executable(
		'test-reorder',
		['test_reorder.c', '../settings/reorder.c'],
		include_directories: include_directories('../settings'),
		dependencies: [glib],
	),
	suite: 'unit',
)

virtual_keyboard_xml = '../protocols/virtual-keyboard-unstable-v1.xml'
keyboard_tool = executable(
	'keyboard-tool',
	[
		'keyboard_tool.c',
		custom_target(
			'virtual_keyboard_client_h',
			input: virtual_keyboard_xml,
			output: '@BASENAME@-client-protocol.h',
			command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
		),
		custom_target(
			'virtual_keyboard_c',
			input: virtual_keyboard_xml,
			output: '@BASENAME@-protocol.c',
			command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
		),
	],
	dependencies: [wayland_client, xkbcommon],
)

virtual_pointer_xml = '../protocols/wlr-virtual-pointer-unstable-v1.xml'
pointer_tool = executable(
	'pointer-tool',
	[
		'pointer_tool.c',
		custom_target(
			'wlr_virtual_pointer_client_h',
			input: virtual_pointer_xml,
			output: '@BASENAME@-client-protocol.h',
			command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
		),
		custom_target(
			'wlr_virtual_pointer_c',
			input: virtual_pointer_xml,
			output: '@BASENAME@-protocol.c',
			command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
		),
	],
	dependencies: [wayland_client],
)

test(
	'gravity-mode',
	find_program('gravity_mode.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'named-desktops',
	find_program('named_desktops.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'multi-screen',
	find_program('multi_screen.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'color-scheme',
	find_program('color_scheme.sh'),
	args: [meson.project_build_root()],
	suite: 'unit',
)

test(
	'qt-platform-theme',
	find_program('qt_platform_theme.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 60,
)

test(
	'restart-helpers',
	find_program('restart_helpers.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'screen-choice',
	find_program('screen_choice.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'git-widget',
	find_program('git_widget.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'focus-priority-session',
	find_program('focus_priority.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'restart-keeps-windows',
	find_program('restart_keeps_windows.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'compositor-options',
	find_program('compositor_options.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'clipboard-split',
	find_program('clipboard_split.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

# works the settings app, so it needs a compositor to drive:
#   meson test -C build --suite gui
test(
	'settings-custom-app',
	find_program('settings_custom_app.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool, keyboard_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-switches-save',
	find_program('settings_switches.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	# each of these starts a compositor of its own and finds it by the socket
	# it adds, so two of them at once would find each other's
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-presets-save',
	find_program('settings_presets.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-numbers-save',
	find_program('settings_numbers.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-menu-themes',
	find_program('settings_menu_themes.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-snap-layouts',
	find_program('settings_snap_layouts.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-drag-widget',
	find_program('settings_drag_widget.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

if gdk_pixbuf.found()
	count_content = executable(
		'count-content',
		'count_content.c',
		dependencies: [gdk_pixbuf],
	)

	# needs a compositor and grim, so it is not part of the default run:
	#   meson test -C build --suite gui
	test(
		'settings-pages-draw',
		find_program('render_pages.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 900,
	)

	test(
		'desktop-widgets',
		find_program('desktop_widgets.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [pointer_tool, count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 300,
	)

	test(
		'screensaver',
		find_program('screensaver.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [pointer_tool, count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 120,
	)

	test(
		'doomsday',
		find_program('doomsday.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [pointer_tool, count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 240,
	)

	test(
		'desktop-widget-styles',
		find_program('desktop_widget_styles.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 300,
	)
endif

test(
	'settings-network',
	find_program('settings_network.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'panel-idle',
	find_program('panel_idle.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'settings-icon',
	find_program('settings_icon.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'network-chart',
	find_program('network_chart.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 240,
)

test(
	'desktop-alt-f4',
	find_program('desktop_alt_f4.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool, keyboard_tool],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 150,
)

test(
	'settings-backup',
	find_program('settings_backup.sh'),
	args: [meson.project_build_root()],
	suite: 'unit',
)

python = find_program('python3', required: false)
if python.found()
	# fake DNS servers, block lists and NetworkManager files of its own
	test(
		'dns-apply',
		find_program('dns_apply.sh'),
		args: [tilewin_dns_apply],
		suite: 'unit',
	)

	test(
		'dns-service',
		python,
		args: [files('dns_service.py'), tilewin_dnsd],
		suite: 'unit',
		timeout: 300,
	)

	test(
		'settings-coverage',
		python,
		args: [files('settings_coverage.py'), meson.project_source_root()],
		suite: 'unit',
	)
endif

test(
	'app-windows',
	find_program('app_windows.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool, keyboard_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 180,
)

test(
	'x11-titlebar',
	find_program('x11_titlebar.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 60,
)

test(
	'taskbar-badges',
	find_program('taskbar_badges.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'taskbar-sound',
	find_program('taskbar_sound.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'jump-list',
	find_program('jump_list.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'snap-layouts',
	find_program('snap_layouts.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'snap-animated',
	find_program('snap_animated.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'edge-buttons',
	find_program('edge_buttons.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'snip-text',
	find_program('snip_text.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'win3-extras',
	find_program('win3_extras.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'taskview-desktops',
	find_program('taskview_desktops.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool, keyboard_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'xp-start-button',
	find_program('xp_start_button.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'sticky-notes',
	find_program('sticky_notes.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool, keyboard_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'workspaces-widget',
	find_program('workspaces_widget.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'panel-config-appears',
	find_program('panel_config_appears.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'startmenu-styles',
	find_program('startmenu_styles.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 180,
)

test(
	'emoji-picker',
	find_program('emoji_picker.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool, keyboard_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'thumbnail-colors',
	find_program('thumbnail_colors.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'media-buttons',
	find_program('media_buttons.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

test(
	'battery-saver',
	find_program('battery_saver.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'snap-resize',
	find_program('snap_resize.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 90,
)

test(
	'new-window-size',
	find_program('new_window_size.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	suite: 'gui',
	is_parallel: false,
	timeout: 60,
)

test(
	'multi-screen-panel',
	find_program('multi_screen_panel.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	env: saver_gui_env,
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 120,
)

if gdk_pixbuf.found()
	test(
		'pointer-trail',
		find_program('pointer_trail.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		env: saver_gui_env,
		depends: [pointer_tool, count_content],
		suite: 'gui',
		is_parallel: false,
		timeout: 60,
	)

	test(
		'polkit-themes',
		find_program('polkit_themes.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [keyboard_tool, count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 300,
	)

	test(
		'polkit-agent',
		find_program('polkit_agent.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [keyboard_tool, count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 120,
	)

	test(
		'notification-area',
		find_program('notification_area.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [count_content],
		env: saver_gui_env,
		suite: 'gui',
		is_parallel: false,
		timeout: 60,
	)
endif
