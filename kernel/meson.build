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

# the automatic updates: the run as root and its timer, the check at every
# start, the builds as the system user tilewin-kernel, and Check now without a
# password (tilewin-kernel-root auto-enable switches them on)
foreach unit : ['tilewin-kernel-auto.service', 'tilewin-kernel-auto-now.service',
		'tilewin-kernel-auto-boot.service',
		'tilewin-kernel-auto-build@.service']
	configure_file(
		input: unit + '.in',
		output: unit,
		configuration: kernel_conf,
		install_dir: join_paths(prefix, 'lib', 'systemd', 'system'),
	)
endforeach
install_data(
	'tilewin-kernel-auto.timer',
	install_dir: join_paths(prefix, 'lib', 'systemd', 'system'),
)
install_data(
	'tilewin-kernel.sysusers',
	rename: 'tilewin-kernel.conf',
	install_dir: join_paths(prefix, 'lib', 'sysusers.d'),
)
install_data(
	'org.tilewin.kernel-auto.rules',
	install_dir: '/usr/share/polkit-1/rules.d',
)

# the modules of the running kernel kept through an update of the
# distribution's kernel, from two hooks of pacman
tilewin_keep_modules = configure_file(
	input: 'tilewin-keep-modules.in',
	output: 'tilewin-keep-modules',
	configuration: kernel_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)
if find_program('pacman', required: false).found()
	foreach hook : ['pre', 'post']
		configure_file(
			input: 'tilewin-keep-modules-' + hook + '.hook.in',
			output: '10-tilewin-keep-modules-' + hook + '.hook',
			configuration: kernel_conf,
			install_dir: '/usr/share/libalpm/hooks',
		)
	endforeach
endif
