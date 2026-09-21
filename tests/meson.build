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

# works the settings app, so it needs a compositor to drive:
#   meson test -C build --suite gui
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
