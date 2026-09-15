pam = dependency('pam', required: false)
if not pam.found()
	pam = cc.find_library('pam', required: false)
endif

if pam.found()
	executable(
		'tilewin-lock', [
			'main.c',
			wl_protos_src,
		],
		include_directories: [sway_inc],
		dependencies: [cairo, gdk_pixbuf, pam, pango, pangocairo, rsvg, wayland_client, xkbcommon],
		link_with: [lib_sway_common, lib_sway_client],
		install: true,
	)
else
	warning('PAM not found: tilewin-lock (the lock screen) is not built')
endif
