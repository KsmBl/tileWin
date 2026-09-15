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

After an update:  git pull && ./install.sh --no-deps && tilewinmsg restart
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
	command -v sudo >/dev/null 2>&1 || die "writing to $PREFIX needs root and sudo is not installed"
	SUDO=sudo
fi

SESSION_DIR=/usr/share/wayland-sessions

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" -eq 1 ]; then
	log="$BUILD_DIR/meson-logs/install-log.txt"
	[ -f "$log" ] || die "no install log found ($log); cannot uninstall"
	if [ -z "$DESTDIR" ] && command -v tilewin-app-icons >/dev/null 2>&1; then
		msg "Giving apps your own icon theme back"
		tilewin-app-icons restore
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
install_deps() {
	if command -v pacman >/dev/null 2>&1; then
		msg "Installing dependencies with pacman"
		sudo pacman -S --needed --noconfirm base-devel meson ninja pkgconf \
			wlroots0.20 wayland wayland-protocols libxkbcommon libinput libevdev \
			pixman libdrm cairo pango gdk-pixbuf2 librsvg gtk4 json-c pcre2 \
			xcb-util-wm xorg-xwayland systemd-libs \
			grim xdg-utils
		sudo pacman -S --needed --noconfirm scdoc pavucontrol xfce4-terminal thunar xdg-desktop-portal-gtk || \
			warn "optional packages could not be installed"
	elif command -v apt-get >/dev/null 2>&1; then
		msg "Installing dependencies with apt (best effort)"
		sudo apt-get update
		sudo apt-get install -y build-essential meson ninja-build pkg-config \
			libwlroots-0.20-dev libwayland-dev wayland-protocols libxkbcommon-dev \
			libinput-dev libevdev-dev libpixman-1-dev libdrm-dev libcairo2-dev \
			libpango1.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev libgtk-4-dev libjson-c-dev \
			libpcre2-dev libxcb-icccm4-dev xwayland libsystemd-dev grim xdg-utils || \
			warn "some packages are missing; wlroots 0.20 may need to be built manually"
	elif command -v dnf >/dev/null 2>&1; then
		msg "Installing dependencies with dnf (best effort)"
		sudo dnf install -y gcc meson ninja-build pkgconf-pkg-config wlroots-devel \
			wayland-devel wayland-protocols-devel libxkbcommon-devel libinput-devel \
			libevdev-devel pixman-devel libdrm-devel cairo-devel pango-devel \
			gdk-pixbuf2-devel librsvg2-devel gtk4-devel json-c-devel pcre2-devel \
			xcb-util-wm-devel xorg-x11-server-Xwayland systemd-devel grim xdg-utils || \
			warn "some packages are missing; tileWin needs wlroots 0.20"
	else
		warn "unknown distribution: install the dependencies listed in README.md manually"
	fi
}

if [ "$INSTALL_DEPS" -eq 1 ]; then
	install_deps
fi

command -v "$MESON" >/dev/null 2>&1 || die "meson not found (install it, e.g. 'sudo pacman -S meson')"
command -v ninja >/dev/null 2>&1 || die "ninja not found"
pkg-config --exists wlroots-0.20 || die "wlroots 0.20 development files not found"

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
if [ -d "$BUILD_DIR" ]; then
	"$MESON" setup --reconfigure "$BUILD_DIR" "${setup_args[@]}" >/dev/null
else
	"$MESON" setup "$BUILD_DIR" "${setup_args[@]}" >/dev/null
fi

msg "Building"
"$MESON" compile -C "$BUILD_DIR"

# ---------------------------------------------------------------- install
msg "Installing"
if [ -n "$DESTDIR" ]; then
	"$MESON" install -C "$BUILD_DIR" --destdir "$DESTDIR" --no-rebuild >/dev/null
else
	# --no-rebuild: everything is built above, root must not write build files
	$SUDO "$MESON" install -C "$BUILD_DIR" --no-rebuild >/dev/null
	if [ -n "$SUDO" ] && [ -e "$BUILD_DIR/meson-logs/install-log.txt" ]; then
		$SUDO chown "$(id -u):$(id -g)" "$BUILD_DIR/meson-logs/install-log.txt"
	fi
fi

# Display managers only look in /usr/share/wayland-sessions.
if [ -z "$DESTDIR" ] && [ "$PREFIX" != /usr ]; then
	msg "Registering the tileWin session in $SESSION_DIR"
	tmp=$(mktemp)
	sed "s|^Exec=.*|Exec=$PREFIX/bin/tilewin-session|" data/tilewin.desktop > "$tmp"
	sudo install -Dm644 "$tmp" "$SESSION_DIR/tilewin.desktop"
	rm -f "$tmp"
fi

# The lock screen checks passwords with its own PAM service.
if [ -z "$DESTDIR" ] && [ -d /etc/pam.d ] && [ ! -e /etc/pam.d/tilewin-lock ]; then
	msg "Installing the PAM service of the lock screen"
	sudo install -Dm644 data/tilewin-lock.pam /etc/pam.d/tilewin-lock
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
  Update later:    git pull && ./install.sh --no-deps && tilewinmsg restart
EOF
