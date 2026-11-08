#!/usr/bin/env bash
# Cova — Arch Linux package (pacman .pkg.tar.zst).
#
# Arch does not ship rpmbuild/dpkg-deb, so this builds the pacman package
# directly instead of going through makepkg (which insists on a build() step
# and a writable PKGBUILD directory).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require bsdtar zstd
resolve_defaults

case "$TARGET_ARCH" in
  x86_64|amd64) ARCH_ARCH=x86_64 ;;
  aarch64|arm64) ARCH_ARCH=aarch64 ;;
  *) die "unsupported architecture for a pacman package: $TARGET_ARCH" ;;
esac

pkgroot="$(mktemp -d)"
trap 'rm -rf "$pkgroot"' EXIT
stage_tree "$pkgroot/pkg"

installed_size="$(du -sb "$pkgroot/pkg" | cut -f1)"

# pacman forbids `-` inside pkgver (it is the pkgver/pkgrel separator), so a
# semver pre-release tag is joined as Arch spells it: 0.1.0-beta-1 -> 0.1.0beta1.
arch_version="${COVA_VERSION//-/}"
builddate="$(date +%s)"

# Pacman's own metadata format. Every scalar field is an extra newline after
# the value, which is why each here-doc block ends with a blank line.
cat > "$pkgroot/pkg/.PKGINFO" <<EOF
pkgname = cova
pkgbase = cova
pkgver = ${arch_version}-1
pkgdesc = Cross-compilation-first build system for C and C++
url = https://github.com/viloris-org/cova
builddate = $builddate
packager = The Cova Authors <rownix101@gmail.com>
size = $installed_size
arch = $ARCH_ARCH
license = Apache-2.0

EOF

# .MTREE records the payload for pacman's database; it must not include itself.
(cd "$pkgroot/pkg" && bsdtar -czf .MTREE --format=mtree \
  --options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
  .PKGINFO usr)

output="$OUT_DIR/cova-${arch_version}-1-${ARCH_ARCH}.pkg.tar.zst"
(cd "$pkgroot/pkg" && bsdtar -cf - .PKGINFO .MTREE usr | zstd -q -19 -T0 -o "$output" -f)
note "built $output"
