kernel_libexecdir = join_paths(prefix, get_option('libexecdir'))

kernel_conf = configuration_data()
kernel_conf.set('datadir', tilewin_datadir)
kernel_conf.set('libexecdir', kernel_libexecdir)

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
