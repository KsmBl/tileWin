updates_conf = configuration_data()
updates_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))

tilewin_updates = configure_file(
	input: 'tilewin-updates.in',
	output: 'tilewin-updates',
	configuration: updates_conf,
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)

tilewin_updates_root = configure_file(
	input: 'tilewin-updates-root.in',
	output: 'tilewin-updates-root',
	configuration: updates_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

# polkit reads its actions only from here; installing always asks for the
# password (kept for a few minutes, so that Install and Clean up ask once)
configure_file(
	input: 'org.tilewin.updates.policy.in',
	output: 'org.tilewin.updates.policy',
	configuration: updates_conf,
	install_dir: '/usr/share/polkit-1/actions',
)
