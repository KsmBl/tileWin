# built as anyone would build it: the header and cairo, nothing of tileWin linked in
example_saver = shared_module(
	'saver', files('lavalamp.c'),
	name_prefix: '',
	include_directories: [saver_inc],
	dependencies: [cairo, math],
)
custom_target(
	'lavalamp-screenshot',
	output: 'screenshot.png',
	command: [screensaver_exe, '--screenshot', meson.current_build_dir(), '@OUTPUT@'],
	env: saver_shot_env,
	depends: example_saver,
	build_by_default: true,
)
