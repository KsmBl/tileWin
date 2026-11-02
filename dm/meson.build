dm_bindir = join_paths(prefix, get_option('bindir'))

dm_common = files('config.c', 'sessions.c')
dm_inc = include_directories('.')

# The daemon runs as root, so it stays small: no toolkit, no drawing.
if pam.found()
	executable(
		'tilewin-display-manager',
		['main.c', 'backend_pam.c', dm_common],
		c_args: [
			'-DTILEWIN_COMPOSITOR="@0@"'.format(join_paths(dm_bindir, 'tilewin')),
			'-DTILEWIN_GREETER="@0@"'.format(join_paths(dm_bindir, 'tilewin-dm-greeter')),
		],
		dependencies: [pam],
		install: true,
	)
	executable(
		'tilewin-dm-greeter',
		['greeter.c', dm_common, wl_protos_src],
		include_directories: [sway_inc],
		dependencies: [cairo, gdk_pixbuf, math, pango, pangocairo, rsvg, wayland_client, xkbcommon],
		link_with: [lib_sway_common, lib_sway_client],
		install: true,
	)

	dm_conf = configuration_data()
	dm_conf.set('bindir', dm_bindir)
	dm_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))
	configure_file(
		input: 'tilewin-display-manager.service.in',
		output: 'tilewin-display-manager.service',
		configuration: dm_conf,
		install_dir: '/usr/lib/systemd/system',
	)
	install_data('tilewin-dm.sysusers.conf',
		rename: 'tilewin-dm.conf',
		install_dir: '/usr/lib/sysusers.d',
	)
	configure_file(
		input: 'tilewin-dm-apply.in',
		output: 'tilewin-dm-apply',
		configuration: dm_conf,
		install_dir: get_option('libexecdir'),
		install_mode: 'rwxr-xr-x',
	)
	configure_file(
		input: 'org.tilewin.dm.policy.in',
		output: 'org.tilewin.dm.policy',
		configuration: dm_conf,
		install_dir: '/usr/share/polkit-1/actions',
	)
	install_data('display-manager.conf', install_dir: tilewin_datadir / 'config')
endif
