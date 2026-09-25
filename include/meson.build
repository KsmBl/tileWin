configure_file(output: 'config.h',  configuration: conf_data)

# The version header, refreshed at every build and only rewritten when the
# commit changed, so only what includes it is compiled again.
version_template = configure_file(
	input: 'tilewin-version.h.in',
	output: 'tilewin-version.h.in',
	configuration: { 'VERSION': meson.project_version(), 'VCS_TAG': '@VCS_TAG@' },
)
tilewin_version_h = vcs_tag(
	input: version_template,
	output: 'tilewin-version.h',
	command: git.found() ? [
		'sh', '-c', 'c=$(git -C "$1" rev-parse --short HEAD 2>/dev/null) && ' +
			'b=$(git -C "$1" rev-parse --abbrev-ref HEAD 2>/dev/null) && printf -- "-%s on %s" "$c" "$b"',
		'sh', meson.project_source_root(),
	] : ['true'],
	fallback: '',
)
