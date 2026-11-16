ports_conf = configuration_data()
ports_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))

tilewin_ports_guard = configure_file(
	input: 'tilewin-ports-guard.in',
	output: 'tilewin-ports-guard',
	configuration: ports_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

# polkit reads its actions and rules only from these places
configure_file(
	input: 'org.tilewin.ports.policy.in',
	output: 'org.tilewin.ports.policy',
	configuration: ports_conf,
	install_dir: '/usr/share/polkit-1/actions',
)

install_data(
	'org.tilewin.ports.rules',
	install_dir: '/usr/share/polkit-1/rules.d',
)
