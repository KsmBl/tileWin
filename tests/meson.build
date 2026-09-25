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
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'named-desktops',
	find_program('named_desktops.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'multi-screen',
	find_program('multi_screen.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	depends: [pointer_tool],
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'git-widget',
	find_program('git_widget.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'restart-keeps-windows',
	find_program('restart_keeps_windows.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'compositor-options',
	find_program('compositor_options.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'clipboard-split',
	find_program('clipboard_split.sh'),
	args: [meson.project_build_root(), meson.project_source_root()],
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
	suite: 'gui',
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-switches-save',
	find_program('settings_switches.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
	suite: 'gui',
	# each of these starts a compositor of its own and finds it by the socket
	# it adds, so two of them at once would find each other's
	is_parallel: false,
	timeout: 300,
)

test(
	'settings-drag-widget',
	find_program('settings_drag_widget.sh'),
	args: [meson.project_build_root()],
	depends: [pointer_tool],
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
		suite: 'gui',
		is_parallel: false,
		timeout: 900,
	)

	test(
		'desktop-widgets',
		find_program('desktop_widgets.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [pointer_tool, count_content],
		suite: 'gui',
		is_parallel: false,
		timeout: 300,
	)

	test(
		'desktop-widget-styles',
		find_program('desktop_widget_styles.sh'),
		args: [meson.project_build_root(), meson.project_source_root()],
		depends: [count_content],
		suite: 'gui',
		is_parallel: false,
		timeout: 300,
	)
endif

python = find_program('python3', required: false)
if python.found()
	test(
		'settings-coverage',
		python,
		args: [files('settings_coverage.py'), meson.project_source_root()],
		suite: 'unit',
	)
endif
