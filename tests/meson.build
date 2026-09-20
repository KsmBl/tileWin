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
