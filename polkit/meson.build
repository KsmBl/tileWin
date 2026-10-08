polkit_agent = dependency('polkit-agent-1', required: false)

if polkit_agent.found()
	executable(
		'tilewin-polkit', [
			'main.c',
			wl_protos_src,
		],
		include_directories: [sway_inc],
		dependencies: [cairo, gdk_pixbuf, jsonc, math, pango, pangocairo, polkit_agent, rsvg,
			wayland_client, xkbcommon],
		link_with: [lib_sway_common, lib_sway_client],
		install: true,
	)
else
	warning('polkit-agent-1 not found: tilewin-polkit (the password prompt of polkit) is not built')
endif
