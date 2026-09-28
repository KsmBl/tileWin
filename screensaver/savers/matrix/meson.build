# matrix: its module, and a picture of it for the settings
# its own code, which it can rain down instead of the film's characters
matrix_source = custom_target(
	'matrix-source',
	input: 'matrix.c',
	output: 'matrix_source.h',
	command: [embed_exe, '@INPUT@', '@OUTPUT@', 'matrix_source'],
)
saver_module = shared_module(
	'saver', files('matrix.c'), matrix_source,
	name_prefix: '',
	include_directories: [sway_inc, saver_inc],
	dependencies: saver_deps,
	link_with: [lib_saver_kit, lib_sway_common],
	install: true,
	install_dir: join_paths(tilewin_savers_dir, 'matrix'),
)
saver_modules += saver_module
custom_target(
	'matrix-screenshot',
	output: 'screenshot.png',
	command: [screensaver_exe, '--screenshot', meson.current_build_dir(), '@OUTPUT@'],
	env: saver_shot_env,
	depends: saver_module,
	build_by_default: true,
	install: true,
	install_dir: join_paths(tilewin_savers_dir, 'matrix'),
)
