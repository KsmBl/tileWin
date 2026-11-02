#!/bin/sh
# Makes the files of a GitHub release: a ready-made archive for this system's
# processor, which "tilewin-update release" installs when its libraries are
# there, and SHA256SUMS. Run in the repository, after the version is set.
#
# usage: tools/make-release-archive.sh <output dir>
set -eu

out=$(mkdir -p "$1" && cd "$1" && pwd)
cd "$(dirname "$(readlink -f "$0")")/.."
version=$(sed -n "s/^[[:space:]]*version: '\([^']*\)',.*/\1/p" meson.build | head -n 1)
name=tilewin-$version-$(uname -m)-linux.tar.gz
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/tilewin/data"
./install.sh --no-deps --no-user-config --prefix /usr/local --destdir "$work/tilewin/stage" >/dev/null
cp data/tilewin.desktop data/*.pam "$work/tilewin/data/"
cp scripts/tilewin-system-setup "$work/tilewin/"
tar -C "$work" -czf "$out/$name" tilewin
(cd "$out" && sha256sum "$name" > SHA256SUMS)
echo "$out/$name"
