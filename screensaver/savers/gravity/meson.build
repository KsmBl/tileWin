# gravity: its module, and a picture of it for the settings
saver_module = shared_module(
	'saver', files('gravity.c'),
	name_prefix: '',
	include_directories: [sway_inc, saver_inc],
	dependencies: saver_deps,
	link_with: [lib_saver_kit, lib_sway_common],
	install: true,
	install_dir: join_paths(tilewin_savers_dir, 'gravity'),
)
saver_modules += saver_module
custom_target(
	'gravity-screenshot',
	output: 'screenshot.png',
	command: [screensaver_exe, '--screenshot', meson.current_build_dir(), '@OUTPUT@'],
	env: saver_shot_env,
	depends: saver_module,
	build_by_default: true,
	install: true,
	install_dir: join_paths(tilewin_savers_dir, 'gravity'),
)
