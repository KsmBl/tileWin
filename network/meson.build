network_conf = configuration_data()
network_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))

tilewin_network_root = configure_file(
	input: 'tilewin-network-root.in',
	output: 'tilewin-network-root',
	configuration: network_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

# NetworkManager's dispatcher looks here as well as in /etc
configure_file(
	input: '90-tilewin-network.in',
	output: '90-tilewin-network',
	configuration: network_conf,
	install_dir: '/usr/lib/NetworkManager/dispatcher.d',
	install_mode: 'rwxr-xr-x',
)

configure_file(
	input: 'org.tilewin.network.policy.in',
	output: 'org.tilewin.network.policy',
	configuration: network_conf,
	install_dir: '/usr/share/polkit-1/actions',
)
