project(
	'tilewin',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=1.3',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
		'wrap_mode=nodownload',
	],
)

add_project_arguments(
	[
		'-DWLR_USE_UNSTABLE',
		'-D_POSIX_C_SOURCE=200809L',

		'-Wno-unused-parameter',
		'-Wno-unused-result',
		'-Wno-missing-braces',
		'-Wno-format-zero-length',
		'-Wundef',
		'-Wvla',
	],
	language: 'c',
)

cc = meson.get_compiler('c')

is_freebsd = host_machine.system().startswith('freebsd')
datadir = get_option('datadir')
sysconfdir = get_option('sysconfdir')
prefix = get_option('prefix')

if is_freebsd
	add_project_arguments('-D_C11_SOURCE', language: 'c')
endif

# Execute the wlroots subproject, if any
wlroots_version = ['>=0.20.0', '<0.21.0']
subproject(
	'wlroots',
	default_options: ['examples=false'],
	required: false,
	version: wlroots_version,
)
wlroots = dependency('wlroots-0.20', version: wlroots_version, fallback: 'wlroots')
wlroots_features = {
	'xwayland': false,
	'libinput_backend': false,
	'session': false,
}
foreach name, _ : wlroots_features
	var_name = 'have_' + name.underscorify()
	have = wlroots.get_variable(pkgconfig: var_name, internal: var_name) == 'true'
	wlroots_features += { name: have }
endforeach

null_dep = dependency('', required: false)

jsonc = dependency('json-c', version: '>=0.13')
pcre2 = dependency('libpcre2-8')
wayland_server = dependency('wayland-server', version: '>=1.21.0')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_protos = dependency('wayland-protocols', version: '>=1.41', default_options: ['tests=false'])
xkbcommon = dependency('xkbcommon', version: '>=1.5.0')
cairo = dependency('cairo')
pango = dependency('pango')
pangocairo = dependency('pangocairo')
gdk_pixbuf = dependency('gdk-pixbuf-2.0', required: get_option('gdk-pixbuf'))
rsvg = dependency('librsvg-2.0')
pixman = dependency('pixman-1')
libevdev = dependency('libevdev')
libinput = wlroots_features['libinput_backend'] ? dependency('libinput', version: '>=1.26.0') : null_dep
xcb = wlroots_features['xwayland'] ? dependency('xcb') : null_dep
drm = dependency('libdrm')
libudev = wlroots_features['libinput_backend'] ? dependency('libudev') : null_dep
math = cc.find_library('m')
rt = cc.find_library('rt')
xcb_icccm = wlroots_features['xwayland'] ? dependency('xcb-icccm') : null_dep
threads = dependency('threads') # for pthread_setschedparam and pthread_atfork

if get_option('sd-bus-provider') == 'auto'
	if not get_option('tray').disabled()
		assert(get_option('auto_features').auto(), 'sd-bus-provider must not be set to auto since auto_features != auto')
	endif
	sdbus = dependency(['libsystemd', 'libelogind'],
		required: false,
		version: '>=239',
	)
	if not sdbus.found()
		sdbus = dependency('basu', required: false)
	endif
else
	sdbus = dependency(get_option('sd-bus-provider'), required: get_option('tray'))
endif

tray_deps_found = sdbus.found()
if get_option('tray').enabled() and not tray_deps_found
	error('Building with -Dtray=enabled, but sd-bus has not been not found')
endif
have_tray = (not get_option('tray').disabled()) and tray_deps_found

conf_data = configuration_data()

conf_data.set10('HAVE_GDK_PIXBUF', gdk_pixbuf.found())
conf_data.set10('HAVE_LIBSYSTEMD', sdbus.found() and sdbus.name() == 'libsystemd')
conf_data.set10('HAVE_LIBELOGIND', sdbus.found() and sdbus.name() == 'libelogind')
conf_data.set10('HAVE_BASU', sdbus.found() and sdbus.name() == 'basu')
conf_data.set10('HAVE_TRAY', have_tray)
foreach sym : ['LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM', 'LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY', 'LIBINPUT_SWITCH_KEYPAD_SLIDE']
	conf_data.set10('HAVE_' + sym, cc.has_header_symbol('libinput.h', sym, dependencies: libinput))
endforeach

scdoc = dependency('scdoc', version: '>=1.9.2', native: true, required: get_option('man-pages'))
if scdoc.found()
	scdoc_prog = find_program(scdoc.get_variable(pkgconfig: 'scdoc'), native: true)
	mandir = get_option('mandir')
	man_files = [
		'sway/sway.1.scd',
		'sway/sway.5.scd',
		'sway/sway-bar.5.scd',
		'sway/sway-input.5.scd',
		'sway/sway-ipc.7.scd',
		'sway/sway-output.5.scd',
		'swaybar/swaybar-protocol.7.scd',
		'swaymsg/swaymsg.1.scd',
	]

	if get_option('swaynag')
		man_files += [
			'swaynag/swaynag.1.scd',
			'swaynag/swaynag.5.scd',
		]
	endif

	foreach filename : man_files
		topic = filename.split('.')[-3].split('/')[-1]
		section = filename.split('.')[-2]
		output = '@0@.@1@'.format(topic, section)

		custom_target(
			output,
			input: filename,
			output: output,
			command: scdoc_prog,
			install: true,
			feed: true,
			capture: true,
			install_dir: '@0@/man@1@'.format(mandir, section)
		)
	endforeach
endif

add_project_arguments('-DSYSCONFDIR="/@0@"'.format(join_paths(prefix, sysconfdir)), language : 'c')
tilewin_datadir = join_paths(prefix, datadir, 'tileWin')
add_project_arguments('-DTILEWIN_DATADIR="@0@"'.format(tilewin_datadir), language : 'c')

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, '--git-dir=.git', 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, '--git-dir=.git', 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"@0@-@1@ (" __DATE__ ", branch \'@2@\')"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif
add_project_arguments('-DSWAY_VERSION=@0@'.format(version), language: 'c')

fs = import('fs')

# Strip relative path prefixes from the code if possible, otherwise hide them.
relative_dir = fs.relative_to(meson.current_source_dir(), meson.global_build_root()) + '/'
if cc.has_argument('-fmacro-prefix-map=/prefix/to/hide=')
	add_project_arguments(
		'-fmacro-prefix-map=@0@='.format(relative_dir),
		language: 'c',
	)
else
	add_project_arguments(
		'-DSWAY_REL_SRC_DIR="@0@"'.format(relative_dir),
		language: 'c',
	)
endif


sway_inc = include_directories('include')

subdir('include')
subdir('protocols')
subdir('common')
subdir('sway')
subdir('swaymsg')

# the taskbar and the lock screen use the client helpers too
subdir('client')
if get_option('swaybar')
	subdir('swaybar')
endif
if get_option('swaynag')
	subdir('swaynag')
endif
subdir('panel')
subdir('lock')
subdir('nightlight')

gtk4 = dependency('gtk4', version: '>=4.12', required: get_option('settings'))
if gtk4.found()
	gtk4 = gtk4.as_system()
	subdir('settings')
endif

# tileWin data files
install_subdir('themes', install_dir: tilewin_datadir)
install_subdir('config', install_dir: tilewin_datadir)

scripts_conf = configuration_data()
scripts_conf.set('datadir', tilewin_datadir)
scripts_conf.set('bindir', join_paths(prefix, get_option('bindir')))
foreach script : ['tilewin-session', 'tilewin-theme', 'tilewin-color-scheme', 'tilewin-app-icons']
	configure_file(
		input: 'scripts' / script + '.in',
		output: script,
		configuration: scripts_conf,
		install_dir: get_option('bindir'),
		install_mode: 'rwxr-xr-x',
	)
endforeach

# not a configure_file: it uses wpctl/pactl names like @DEFAULT_SINK@
install_data(
	'scripts/tilewin-media.in',
	rename: 'tilewin-media',
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)

install_data(
	'data/tilewin.desktop',
	install_dir: join_paths(datadir, 'wayland-sessions'),
)

# xdg-desktop-portal looks for <lowercase desktop name>-portals.conf
install_data(
	'data/tilewin-portals.conf',
	install_dir: join_paths(datadir, 'xdg-desktop-portal'),
)

if get_option('fish-completions')
	fish = dependency('fish', required: false)
	if fish.found()
		fish_completions_dir = fish.get_variable(pkgconfig: 'completionsdir',
			pkgconfig_define: ['datadir', join_paths(prefix, datadir)])
	else
		fish_completions_dir = join_paths(datadir, 'fish', 'vendor_completions.d')
	endif
	install_data(
		'completions/fish/tilewin.fish',
		'completions/fish/tilewinmsg.fish',
		'completions/fish/tilewin-panel.fish',
		'completions/fish/tilewin-theme.fish',
		'completions/fish/tilewin-settings.fish',
		'completions/fish/tilewin-media.fish',
		'completions/fish/tilewin-app-icons.fish',
		install_dir: fish_completions_dir,
	)
endif

summary({
	'fish-completions': get_option('fish-completions'),
	'settings': gtk4.found(),
	'gdk-pixbuf': gdk_pixbuf.found(),
	'tray': have_tray,
	'man-pages': scdoc.found(),
}, bool_yn: true)
