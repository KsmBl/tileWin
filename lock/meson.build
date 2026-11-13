pam = dependency('pam', required: false)
if not pam.found()
	pam = cc.find_library('pam', required: false)
endif

if pam.found()
	# the fingerprint reader through fprintd, where there is sd-bus
	lock_sources = ['main.c', wl_protos_src]
	lock_deps = [cairo, gdk_pixbuf, pam, pango, pangocairo, rsvg, wayland_client, xkbcommon]
	lock_args = []
	if sdbus.found()
		lock_sources += 'fingerprint.c'
		lock_deps += sdbus
		lock_args += '-DHAVE_FINGERPRINT=1'
	endif
	executable(
		'tilewin-lock', lock_sources,
		include_directories: [sway_inc],
		c_args: lock_args,
		dependencies: lock_deps,
		link_with: [lib_sway_common, lib_sway_client],
		install: true,
	)
else
	warning('PAM not found: tilewin-lock (the lock screen) is not built')
endif
