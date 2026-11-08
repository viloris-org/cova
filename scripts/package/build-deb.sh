#!/usr/bin/env bash
# Cova — Debian/Ubuntu package (.deb).
#
# Produces dist/cova_<version>_<arch>.deb. The architecture is the Debian name
# (amd64, arm64), which differs from the kernel machine name.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require dpkg-deb
resolve_defaults

case "$TARGET_ARCH" in
  x86_64|amd64) DEB_ARCH=amd64 ;;
  aarch64|arm64) DEB_ARCH=arm64 ;;
  armv7l|armhf) DEB_ARCH=armhf ;;
  *) die "unsupported architecture for a .deb: $TARGET_ARCH" ;;
esac

pkgroot="$(mktemp -d)"
trap 'rm -rf "$pkgroot"' EXIT
stage_tree "$pkgroot/root"

installed_size="$(du -sk "$pkgroot/root" | cut -f1)"

mkdir -p "$pkgroot/root/DEBIAN"
cat > "$pkgroot/root/DEBIAN/control" <<EOF
Package: cova
Version: $COVA_VERSION
Section: devel
Priority: optional
Architecture: $DEB_ARCH
Installed-Size: $installed_size
Maintainer: The Cova Authors <rownix101@gmail.com>
Homepage: https://github.com/viloris-org/cova
Description: Cross-compilation-first build system for C and C++
 Cova is a build system, package manager and toolchain manager for C and C++,
 inspired by Cargo. It treats cross compilation as a first-class command
 rather than a configuration project.
EOF

if ! is_static_elf; then
  # Only ask for a runtime when the binary actually links one. A static build
  # has no shared-library dependency and must not pull in libc6.
  echo "Depends: libc6 (>= 2.34)" >> "$pkgroot/root/DEBIAN/control"
fi

install -d -m 0755 "$pkgroot/root/DEBIAN"
# Keep the staged metadata out of the payload.
chmod 0644 "$pkgroot/root/DEBIAN/control"

output="$OUT_DIR/cova_${COVA_VERSION}_${DEB_ARCH}.deb"
dpkg-deb --root-owner-group --build "$pkgroot/root" "$output" >/dev/null
note "built $output"
