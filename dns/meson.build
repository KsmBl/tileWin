dns_libexecdir = join_paths(prefix, get_option('libexecdir'))

lib_dns = static_library(
	'tilewin-dns',
	['wire.c', 'domainset.c'],
)

tilewin_dnsd = executable(
	'tilewin-dnsd',
	'main.c',
	link_with: [lib_dns],
	dependencies: [dependency('threads')],
	install: true,
	install_dir: get_option('libexecdir'),
)

dns_conf = configuration_data()
dns_conf.set('libexecdir', dns_libexecdir)

configure_file(
	input: 'tilewin-dnsd.service.in',
	output: 'tilewin-dnsd.service',
	configuration: dns_conf,
	install_dir: join_paths(prefix, 'lib', 'systemd', 'system'),
)

configure_file(
	input: 'tilewin-dns-apply.in',
	output: 'tilewin-dns-apply',
	configuration: dns_conf,
	install_dir: get_option('libexecdir'),
	install_mode: 'rwxr-xr-x',
)

# polkit reads its actions and rules only from these places
configure_file(
	input: 'org.tilewin.dns.policy.in',
	output: 'org.tilewin.dns.policy',
	configuration: dns_conf,
	install_dir: '/usr/share/polkit-1/actions',
)

install_data(
	'org.tilewin.dns.rules',
	install_dir: '/usr/share/polkit-1/rules.d',
)
