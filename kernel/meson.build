kernel_libexecdir = join_paths(prefix, get_option('libexecdir'))

kernel_conf = configuration_data()
kernel_conf.set('datadir', tilewin_datadir)
kernel_conf.set('libexecdir', kernel_libexecdir)
kernel_conf.set('bindir', join_paths(prefix, get_option('bindir')))

tilewin_kernel = configure_file(
	input: 'tilewin-kernel.in',
	output: 'tilewin-kernel',
	configuration: kernel_conf,
	install_dir: get_option('bindir'),
	install_mode: 'rwxr-xr-x',
)

tilewin_kernel_root = configure_file(
	input: 'tilewin-kernel-root.in',
	output: 'tilewin-kernel-root',
	configuration: kernel_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

install_data(
	'boot.list',
	'portable.list',
	'full-off.config',
	install_dir: join_paths(tilewin_datadir, 'kernel'),
)

# polkit reads its actions only from here; no rule lets it pass without a
# password: putting a kernel on the computer always asks
configure_file(
	input: 'org.tilewin.kernel.policy.in',
	output: 'org.tilewin.kernel.policy',
	configuration: kernel_conf,
	install_dir: '/usr/share/polkit-1/actions',
)

# the timer that notes the modules in use (the Kernel page switches it on)
configure_file(
	input: 'tilewin-kernel-modules.service.in',
	output: 'tilewin-kernel-modules.service',
	configuration: kernel_conf,
	install_dir: join_paths(prefix, 'lib', 'systemd', 'user'),
)
install_data(
	'tilewin-kernel-modules.timer',
	install_dir: join_paths(prefix, 'lib', 'systemd', 'user'),
)

# the livepatches set for every start of a kernel (the root helper enables it)
configure_file(
	input: 'tilewin-kernel-livepatch.service.in',
	output: 'tilewin-kernel-livepatch.service',
	configuration: kernel_conf,
	install_dir: join_paths(prefix, 'lib', 'systemd', 'system'),
)

# switching the scheduler for a game without asking for the password
install_data(
	'org.tilewin.scx.rules',
	install_dir: '/usr/share/polkit-1/rules.d',
)
