#!/usr/bin/env bash
# Cova — macOS installer (.pkg and .dmg).
#
# Must run on macOS: it uses pkgbuild/hdiutil, which have no equivalent
# elsewhere and no cross-compilation story. The binary is expected to be a
# universal (x86_64 + arm64) build so one download covers Intel and Apple
# Silicon.
#
# The packages are unsigned. Gatekeeper therefore warns on first open; the
# README inside the image explains the one-time override. Signing needs an
# Apple Developer ID and happens outside this script.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require pkgbuild hdiutil
[[ "$(uname -s)" == "Darwin" ]] || die "the macOS installers can only be built on macOS"

resolve_defaults
macos_arch="${COVA_PACKAGE_ARCH:-universal}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- .pkg -------------------------------------------------------------------
stage_tree "$work/root"
pkg_id="org.viloris.cova"

pkgbuild --quiet \
  --root "$work/root" \
  --identifier "$pkg_id" \
  --version "$COVA_VERSION" \
  --install-location "/" \
  "$OUT_DIR/cova-${COVA_VERSION}-macos-${macos_arch}.pkg"

note "built $OUT_DIR/cova-${COVA_VERSION}-macos-${macos_arch}.pkg"

# --- .dmg -------------------------------------------------------------------
# One image wrapping the installer, which is the layout macOS users expect for
# a command-line tool: open the image, run the package, done.
image_root="$work/image"
mkdir -p "$image_root"
cp "$OUT_DIR/cova-${COVA_VERSION}-macos-${macos_arch}.pkg" "$image_root/"

cat > "$image_root/README.txt" <<'TXT'
Cova — installation
===================

1. Double-click the .pkg in this image and follow the installer.
   It places `cova` in /usr/local/bin.

2. This package is not signed with an Apple Developer ID, so macOS may refuse
   to open it. If so, right-click (or Control-click) the package and choose
   "Open" once, or clear the quarantine flag first:

     xattr -d com.apple.quarantine cova-*.pkg

3. Verify the installation:

     cova --version
     cova doctor

Cova needs a C++26-capable compiler (Clang >= 17 or GCC >= 14) and Ninja to
build projects. `cova doctor` reports what it finds.
TXT

hdiutil create -quiet -volname "Cova $COVA_VERSION" \
  -srcfolder "$image_root" \
  -fs HFS+ -format UDZO -ov \
  "$OUT_DIR/cova-${COVA_VERSION}-macos-${macos_arch}.dmg"

note "built $OUT_DIR/cova-${COVA_VERSION}-macos-${macos_arch}.dmg"
