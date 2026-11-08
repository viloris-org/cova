#!/usr/bin/env bash
# Cova — build every package family this host can produce.
#
# The host decides what is possible:
#   Linux    -> .tar.gz always; .deb with dpkg-deb; .rpm with rpmbuild;
#               pacman package with bsdtar+zstd
#   macOS    -> universal .tar.gz, .pkg and .dmg
#
# A missing tool is reported and skipped rather than failed, so the same script
# runs unchanged on a contributor's machine and in CI.
#
# Usage:
#   scripts/package/build-all.sh            # build everything available
#   COVA_BINARY=... COVA_VERSION=... scripts/package/build-all.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

resolve_defaults

os_name="$(uname -s)"
skipped=()

have() { command -v "$1" >/dev/null 2>&1; }

run_step() {
  local name="$1" script="$2"
  note "building $name"
  if bash "$PKG_SCRIPTS_DIR/$script"; then
    :
  else
    die "$name failed"
  fi
}

if [[ "$os_name" == "Darwin" ]]; then
  run_step "tarball" "build-tarball.sh"
  if have pkgbuild && have hdiutil; then
    run_step "macOS .pkg and .dmg" "build-macos.sh"
  else
    skipped+=("macOS installers (pkgbuild/hdiutil missing)")
  fi
else
  run_step "tarball" "build-tarball.sh"
  if have dpkg-deb; then
    run_step ".deb" "build-deb.sh"
  else
    skipped+=(".deb (dpkg-deb missing)")
  fi
  if have rpmbuild || [[ -n "${COVA_RPM_ROOT:-}" ]]; then
    run_step ".rpm" "build-rpm.sh"
  else
    skipped+=(".rpm (rpmbuild missing)")
  fi
  if have bsdtar && have zstd; then
    run_step "pacman package" "build-arch.sh"
  else
    skipped+=("pacman package (bsdtar/zstd missing)")
  fi
fi

# SHA256SUMS is what install.sh verifies downloads against. `find -printf` is
# GNU-only, so the file list is produced with a portable shell glob.
(
  cd "$OUT_DIR"
  # bash 3.2 (the macOS default) rejects `"${arr[@]}"` on an empty array under
  # `set -u`, so the file list is newline-delimited and read back line by line.
  list="$(mktemp)"
  trap 'rm -f "$list"' EXIT
  for pattern in '*.deb' '*.rpm' '*.pkg.tar.zst' '*.tar.gz' '*.dmg'; do
    for f in $pattern; do
      if [ -e "$f" ]; then basename "$f" >> "$list"; fi
    done
  done
  : > SHA256SUMS
  while IFS= read -r file; do
    if command -v sha256sum >/dev/null 2>&1; then
      sha256sum "$file" >> SHA256SUMS
    else
      shasum -a 256 "$file" >> SHA256SUMS
    fi
  done < "$list"
)
note "wrote $OUT_DIR/SHA256SUMS"

echo
echo "artifacts in $OUT_DIR:"
ls -1 "$OUT_DIR"
if (( ${#skipped[@]} > 0 )); then
  echo
  echo "skipped:"
  printf '  - %s\n' "${skipped[@]}"
fi
