# tilewin-portal: the login pages of public Wi-Fi, accepted by themselves
portal_curl = dependency('libcurl', version: '>=7.85')

lib_portal = static_library(
	'tilewin-portal-pages',
	['html.c', 'accept.c'],
	dependencies: [portal_curl],
)

portal_accept_js = custom_target(
	'portal-accept-js',
	input: 'accept.js',
	output: 'accept_js.h',
	command: [embed_exe, '@INPUT@', '@OUTPUT@', 'portal_accept_js'],
)

executable(
	'tilewin-portal',
	['main.c', 'chromium.c', portal_accept_js],
	include_directories: [sway_inc],
	dependencies: [portal_curl, jsonc, wayland_client, sdbus],
	link_with: [lib_portal, lib_sway_common],
	install: true,
)
