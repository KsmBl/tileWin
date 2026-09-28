# What runs the screen savers (the kit): finding them, running them, and the
# helpers they share. It goes into every program that shows savers, which
# lend its functions to the savers they load, and into the savers that come
# with tileWin, so these need nothing of the program.
saver_inc = include_directories('include')
saver_deps = [cairo, gdk_pixbuf, jsonc, math, pango, pangocairo]
lib_saver_kit = static_library(
	'tilewin-saver-kit', [
		'kit/savers.c',
		'kit/saver_tilewin.c',
	],
	include_directories: [sway_inc, saver_inc],
	dependencies: saver_deps,
	link_with: [lib_sway_common],
	pic: true,
)

screensaver_exe = executable(
	'tilewin-screensaver', [
		'main.c',
		wl_protos_src,
	],
	include_directories: [sway_inc, saver_inc],
	dependencies: saver_deps + [wayland_client],
	link_whole: [lib_saver_kit],
	link_with: [lib_sway_common, lib_sway_client],
	export_dynamic: true,
	install: true,
)

# the screen savers: a directory each, with its module and its screenshot
tilewin_savers_dir = join_paths(tilewin_datadir, 'screensavers')
saver_shot_env = ['HOME=/nonexistent', 'XDG_CONFIG_HOME=/nonexistent', 'TILEWIN_SAVERS=']
saver_modules = []
# a file as a C string, for savers that show a text of the source tree
embed_exe = executable('tilewin-embed', '../tools/embed.c', native: true)

subdir('savers')
subdir('example')

# for screen savers of your own (see README.md here)
install_headers('include/tilewin-saver.h', 'include/savers.h', 'include/saver_util.h',
	'include/saver_noise.h', subdir: 'tileWin')
