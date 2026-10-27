devices_conf = configuration_data()
devices_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))

tilewin_devices_root = configure_file(
	input: 'tilewin-devices-root.in',
	output: 'tilewin-devices-root',
	configuration: devices_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

configure_file(
	input: 'org.tilewin.devices.policy.in',
	output: 'org.tilewin.devices.policy',
	configuration: devices_conf,
	install_dir: '/usr/share/polkit-1/actions',
)

# its own entry in the start menu and the launcher, as devmgmt.msc is
install_data(
	'tilewin-device-manager.desktop',
	install_dir: join_paths(datadir, 'applications'),
)
