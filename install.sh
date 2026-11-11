#!/usr/bin/env bash
# tileWin installer: installs build dependencies, builds a release build and
# installs the compositor, taskbar, themes, default configs and the session
# entry for display managers.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

PREFIX=/usr/local
INSTALL_DEPS=1
UNINSTALL=0
BUILDTYPE=release
DESTDIR=""
USER_CONFIG=1
BUILD_DIR="${TILEWIN_BUILD_DIR:-build-release}"
MESON="${MESON:-meson}"

usage() {
	cat <<EOF
Usage: ./install.sh [options]

  --prefix <dir>      Installation prefix (default: /usr/local)
  --no-deps           Do not install build dependencies
  --debug             Build with debug info instead of an optimized release
  --destdir <dir>     Stage the installation into <dir> (for packaging/testing)
  --no-user-config    Do not create ~/.config/tileWin
  --uninstall         Remove an installation made with this script
  -h, --help          Show this help

To update later:  tilewin-update
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--prefix) PREFIX="$2"; shift ;;
	--prefix=*) PREFIX="${1#*=}" ;;
	--no-deps) INSTALL_DEPS=0 ;;
	--debug) BUILDTYPE=debugoptimized ;;
	--destdir) DESTDIR="$2"; shift ;;
	--destdir=*) DESTDIR="${1#*=}" ;;
	--no-user-config) USER_CONFIG=0 ;;
	--uninstall) UNINSTALL=1 ;;
	-h|--help) usage; exit 0 ;;
	*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

msg() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
	die "run ./install.sh without sudo; it asks for your password when it needs it"
fi

SUDO=""
needs_root() {
	local dir="$1"
	while [ ! -e "$dir" ]; do dir=$(dirname "$dir"); done
	[ ! -w "$dir" ]
}
if [ -z "$DESTDIR" ] && needs_root "$PREFIX" && [ "$(id -u)" -ne 0 ]; then
	SUDO=${TILEWIN_SUDO:-sudo}
	command -v "$SUDO" >/dev/null 2>&1 || die "writing to $PREFIX needs root and $SUDO is not installed"
fi

SESSION_DIR=/usr/share/wayland-sessions

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" -eq 1 ]; then
	log="$BUILD_DIR/meson-logs/install-log.txt"
	# installed by tilewin-update the build is gone, and the list stays with the files
	[ -f "$log" ] || log="$DESTDIR$PREFIX/share/tileWin/install-log.txt"
	[ -f "$log" ] || die "no install log found ($log); cannot uninstall"
	if [ -z "$DESTDIR" ] && command -v tilewin-app-icons >/dev/null 2>&1; then
		msg "Giving apps your own icon theme back"
		tilewin-app-icons restore
	fi
	# with the DNS service gone, DNS has to go back to NetworkManager first
	dns_apply="$PREFIX/libexec/tilewin-dns-apply"
	if [ -z "$DESTDIR" ] && [ -x "$dns_apply" ] &&
			systemctl is-enabled --quiet tilewin-dnsd.service 2>/dev/null; then
		msg "Handing DNS back to NetworkManager"
		$SUDO "$dns_apply" disable || warn "could not stop the DNS service"
	fi
	msg "Removing installed files"
	grep -v '^#' "$log" | while read -r file; do
		[ -n "$file" ] && [ -e "$DESTDIR$file" ] && $SUDO rm -f "$DESTDIR$file" && echo "  removed $file"
	done
	$SUDO rm -rf "$DESTDIR$PREFIX/share/tileWin"
	if [ -z "$DESTDIR" ] && [ -f "$SESSION_DIR/tilewin.desktop" ] && [ "$PREFIX" != /usr ]; then
		$SUDO rm -f "$SESSION_DIR/tilewin.desktop"
	fi
	msg "Done. Your configuration in ~/.config/tileWin was kept."
	exit 0
fi

# ---------------------------------------------------------------- dependencies
# A screen with a backlight (a laptop) is dimmed with brightnessctl.
wants_brightnessctl() {
	[ -n "$(ls -A /sys/class/backlight 2>/dev/null)" ] && ! command -v brightnessctl >/dev/null 2>&1
}

# The taskbar's volume, the sound flyout and the mixer talk to a sound server
# that speaks PulseAudio (pactl): PipeWire with its PulseAudio part, or
# PulseAudio itself. Installed only when neither is there, so nothing is
# swapped under a setup that works.
wants_sound_server() {
	! command -v pipewire-pulse >/dev/null 2>&1 && ! command -v pulseaudio >/dev/null 2>&1
}

# Package lists of the extras this computer needs: brightness, sound server.
extras() { # brightness-package sound-packages...
	local out=()
	wants_brightnessctl && out+=("$1")
	shift
	wants_sound_server && out+=("$@")
	echo "${out[*]}"
}

# A sound server installed just now runs from the next login on; it is
# started for this one too.
start_sound_server() {
	command -v systemctl >/dev/null 2>&1 || return 0
	if command -v pipewire-pulse >/dev/null 2>&1; then
		systemctl --user enable --now pipewire.socket pipewire-pulse.socket wireplumber.service \
			>/dev/null 2>&1 || warn "could not start PipeWire; it starts with your next login"
	fi
}

install_deps() {
	local sound_missing=0
	wants_sound_server && sound_missing=1
	if command -v pacman >/dev/null 2>&1; then
		msg "Installing dependencies with pacman"
		sudo pacman -S --needed --noconfirm base-devel meson ninja pkgconf \
			wlroots0.20 wayland wayland-protocols libxkbcommon libinput libevdev \
			pixman libdrm cairo pango gdk-pixbuf2 librsvg gtk4 json-c pcre2 \
			xcb-util-wm xorg-xwayland systemd-libs polkit curl \
			grim slurp wl-clipboard xdg-utils
		sudo pacman -S --needed --noconfirm scdoc pavucontrol xfce4-terminal thunar xdg-desktop-portal-gtk \
			tesseract tesseract-data-eng libpulse || \
			warn "optional packages could not be installed"
		local more
		more=$(extras brightnessctl pipewire pipewire-pulse wireplumber)
		if [ -n "$more" ]; then
			msg "Installing $more"
			# shellcheck disable=SC2086
			sudo pacman -S --needed --noconfirm $more || warn "could not install $more"
		fi
	elif command -v apt-get >/dev/null 2>&1; then
		msg "Installing dependencies with apt"
		sudo apt-get update
		# apt installs nothing of a list with one unknown name in it: this
		# list holds only what Debian 13 has, wlroots 0.20 comes below
		sudo apt-get install -y build-essential meson ninja-build pkg-config \
			libwayland-dev wayland-protocols libxkbcommon-dev xkb-data \
			libinput-dev libevdev-dev libudev-dev libpixman-1-dev libdrm-dev libcairo2-dev \
			libpango1.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev libgtk-4-dev libglib2.0-dev \
			libjson-c-dev libpcre2-dev libxcb1-dev libxcb-icccm4-dev xwayland libsystemd-dev \
			libpam0g-dev libpolkit-agent-1-dev polkitd pkexec libcurl4-openssl-dev curl \
			libcap2-bin python3 \
			grim slurp wl-clipboard xdg-utils pulseaudio-utils || \
			warn "some packages could not be installed"
		# Debian 13 has wlroots 0.18 only: 0.20 is built along with tileWin then
		if apt-cache show libwlroots-0.20-dev >/dev/null 2>&1; then
			sudo apt-get install -y libwlroots-0.20-dev || warn "could not install libwlroots-0.20-dev"
		else
			msg "Installing what building wlroots 0.20 takes"
			sudo apt-get install -y libseat-dev libdisplay-info-dev libliftoff-dev hwdata \
				libgbm-dev libegl-dev libgles-dev libvulkan-dev glslang-tools liblcms2-dev \
				libxcb-composite0-dev libxcb-ewmh-dev libxcb-res0-dev libxcb-render0-dev \
				libxcb-xfixes0-dev libxcb-errors-dev libffi-dev libexpat1-dev libxml2-dev bison || \
				warn "some packages wlroots needs could not be installed"
		fi
		sudo apt-get install -y scdoc pavucontrol xfce4-terminal thunar xdg-desktop-portal-gtk \
			tesseract-ocr tesseract-ocr-eng libpulse-dev || \
			warn "optional packages could not be installed"
		local more
		more=$(extras brightnessctl pipewire-pulse wireplumber)
		if [ -n "$more" ]; then
			msg "Installing $more"
			# shellcheck disable=SC2086
			sudo apt-get install -y $more || warn "could not install $more"
		fi
	elif command -v dnf >/dev/null 2>&1; then
		msg "Installing dependencies with dnf (best effort)"
		sudo dnf install -y gcc meson ninja-build pkgconf-pkg-config wlroots-devel \
			wayland-devel wayland-protocols-devel libxkbcommon-devel libinput-devel \
			libevdev-devel pixman-devel libdrm-devel cairo-devel pango-devel \
			gdk-pixbuf2-devel librsvg2-devel gtk4-devel json-c-devel pcre2-devel \
			xcb-util-wm-devel xorg-x11-server-Xwayland systemd-devel polkit-devel libcurl-devel \
			grim slurp wl-clipboard xdg-utils pulseaudio-utils || \
			warn "some packages are missing; tileWin needs wlroots 0.20"
		local more
		more=$(extras brightnessctl pipewire-pulseaudio wireplumber)
		if [ -n "$more" ]; then
			msg "Installing $more"
			# shellcheck disable=SC2086
			sudo dnf install -y $more || warn "could not install $more"
		fi
	else
		warn "unknown distribution: install the dependencies listed in README.md manually"
	fi
	if [ "$sound_missing" -eq 1 ] && ! wants_sound_server; then
		start_sound_server
	fi
}

if [ "$INSTALL_DEPS" -eq 1 ]; then
	install_deps
fi

command -v "$MESON" >/dev/null 2>&1 || die "meson not found (install it, e.g. 'sudo pacman -S meson')"
command -v ninja >/dev/null 2>&1 || die "ninja not found"

# Without wlroots 0.20 on the system (Debian 13 has 0.18), it is built from
# subprojects/ along with what it needs newer than the distribution has:
# wayland, wayland-protocols, libdrm and libxkbcommon. They are linked in
# statically and not installed, so nothing of the system is replaced.
command -v pkg-config >/dev/null 2>&1 || die "pkg-config not found"
# Once downloaded, meson.build takes wlroots from there even when the system
# has it by now, so it stays static then too.
BUNDLED_WLROOTS=0
if ! pkg-config --exists 'wlroots-0.20 >= 0.20.0' || [ -n "$(ls -d subprojects/wlroots-*/ 2>/dev/null)" ]; then
	BUNDLED_WLROOTS=1
	msg "wlroots 0.20 is built along with tileWin, from subprojects/"
	"$MESON" subprojects download wlroots wayland wayland-protocols libdrm libxkbcommon >/dev/null ||
		die "could not download wlroots and its libraries into subprojects/"
fi

# ---------------------------------------------------------------- build
# Older versions of this script let "sudo meson install" rebuild, which left
# files of root in the build directory that a normal build cannot overwrite.
if [ -d "$BUILD_DIR" ] && [ -n "$(find "$BUILD_DIR" ! -user "$(id -u)" -print -quit 2>/dev/null)" ]; then
	msg "Files in $BUILD_DIR belong to root; giving them back to $(id -un)"
	sudo chown -R "$(id -u):$(id -g)" "$BUILD_DIR"
fi

msg "Configuring ($BUILDTYPE build, prefix $PREFIX)"
setup_args=(--prefix "$PREFIX" --buildtype "$BUILDTYPE" -Dman-pages=disabled -Dwerror=false)
if [ "$BUILDTYPE" = release ]; then
	setup_args+=(-Db_lto=true)
fi
if [ "$BUNDLED_WLROOTS" -eq 1 ]; then
	# static: the libraries built here end up inside the programs. libxkbcommon
	# looks for the keyboard layouts where the system has them, not below $PREFIX
	xkb_root=$(pkg-config --variable=xkb_base xkeyboard-config 2>/dev/null || true)
	setup_args+=(--default-library=static
		"-Dlibxkbcommon:xkb-config-root=${xkb_root:-/usr/share/X11/xkb}"
		-Dlibxkbcommon:x-locale-root=/usr/share/X11/locale)
fi
# Set up again only when the options changed: meson notices changed build files
# itself, and a needless reconfigure costs time on every update.
setup_stamp="$BUILD_DIR/.install-setup-args"
if [ -f "$BUILD_DIR/build.ninja" ] && [ -f "$setup_stamp" ] &&
		[ "$(cat "$setup_stamp")" = "${setup_args[*]}" ]; then
	msg "Build directory is set up already; building only what changed"
elif [ -d "$BUILD_DIR" ]; then
	"$MESON" setup --reconfigure "$BUILD_DIR" "${setup_args[@]}" >/dev/null
	printf '%s' "${setup_args[*]}" > "$setup_stamp"
else
	"$MESON" setup "$BUILD_DIR" "${setup_args[@]}" >/dev/null
	printf '%s' "${setup_args[*]}" > "$setup_stamp"
fi

msg "Building"
"$MESON" compile -C "$BUILD_DIR"

# ---------------------------------------------------------------- install
msg "Installing"
if [ -n "$DESTDIR" ]; then
	"$MESON" install -C "$BUILD_DIR" --destdir "$DESTDIR" --no-rebuild --skip-subprojects >/dev/null
else
	# all that takes root in one go, so a password is asked for once. It also
	# needs root with a prefix of your own, for the session and the lock screen.
	ROOT=$SUDO
	if [ -z "$ROOT" ] && [ "$(id -u)" -ne 0 ]; then
		ROOT=${TILEWIN_SUDO:-sudo}
	fi
	# --no-rebuild in there: everything is built above, root must not write build files
	$ROOT sh "$PWD/scripts/tilewin-system-setup" "$PREFIX" "$PWD" "$(id -u):$(id -g)" build \
		"$(readlink -f "$BUILD_DIR")" "$(command -v "$MESON")"
fi

# ---------------------------------------------------------------- user config
if [ "$USER_CONFIG" -eq 1 ]; then
	config_home="${XDG_CONFIG_HOME:-$HOME/.config}/tileWin"
	mkdir -p "$config_home/themes"
	for file in common.conf tilemode.conf windowmode.conf taskbar.conf; do
		if [ ! -e "$config_home/$file" ]; then
			install -m644 "config/$file" "$config_home/$file"
			echo "  created $config_home/$file"
		fi
	done
	[ -s "$config_home/current-theme" ] || echo win10 > "$config_home/current-theme"
fi

cat <<EOF

tileWin is installed.

  Start it:        choose "tileWin" in your display manager,
                   or run "tilewin-session" from a text console.
  Switch theme:    tilewin-theme list / tilewin-theme set win95
  Switch mode:     Super+Shift+W  or  tilewinmsg mode toggle
  Configuration:   ~/.config/tileWin/{common,tilemode,windowmode,taskbar}.conf
  Update later:    tilewin-update (the newest release or the newest source)
EOF

# In a running session: what a restart brings in, and what only a new login does.
# The taskbar, the settings, the helpers and the config come in with
# "tilewinmsg restart", the windows staying open; the compositor itself cannot
# be swapped under open windows (a Wayland app dies with its compositor).
if [ -z "$DESTDIR" ] && command -v tilewinmsg >/dev/null 2>&1 &&
		running=$(tilewinmsg -t get_version 2>/dev/null); then
	running=$(printf '%s' "$running" | sed -n 's/.*"human_readable": *"\([^"]*\)".*/\1/p')
	installed=$("$PREFIX/bin/tilewin" --version 2>/dev/null | sed 's/^[^0-9]*//')
	if [ -n "$running" ] && [ -n "$installed" ] && [ "${running%% *}" != "${installed%% *}" ]; then
		cat <<EOF

  tileWin is running ${running%% *}; ${installed%% *} is installed now.
  "tilewinmsg restart" brings in the new taskbar, settings, helpers and
  config with your windows open. The new compositor itself (window handling,
  input, screens) starts with your next login, or right away with
  "tilewinmsg restart relaunch-apps" (opens your apps again, without what
  was unsaved in them).
EOF
	elif [ -n "$running" ]; then
		echo
		echo "  Run \"tilewinmsg restart\" to bring the update in; your windows stay open."
	fi
fi
