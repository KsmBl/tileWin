# tilewin-cpu-root, what the Processor page of the settings runs with pkexec to
# limit the clock of the cores, and the service that sets the limits at every start
cpu_conf = configuration_data()
cpu_conf.set('libexecdir', join_paths(prefix, get_option('libexecdir')))

tilewin_cpu_root = configure_file(
	input: 'tilewin-cpu-root.in',
	output: 'tilewin-cpu-root',
	configuration: cpu_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

configure_file(
	input: 'org.tilewin.cpu.policy.in',
	output: 'org.tilewin.cpu.policy',
	configuration: cpu_conf,
	install_dir: '/usr/share/polkit-1/actions',
)

configure_file(
	input: 'tilewin-cpu-limits.service.in',
	output: 'tilewin-cpu-limits.service',
	configuration: cpu_conf,
	install_dir: join_paths(prefix, 'lib', 'systemd', 'system'),
)
