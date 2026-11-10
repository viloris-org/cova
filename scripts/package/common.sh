#!/usr/bin/env bash
# Cova — shared packaging helpers.
#
# Every package family is produced from one staging tree so the installed file
# layout is identical everywhere:
#
#   /usr/bin/cova
#   /usr/share/doc/cova/LICENSE
#   /usr/share/doc/cova/NOTICE
#
# Package metadata (name, version, description, license) comes from cova.toml,
# so a version bump in the manifest is the only place a release number lives.
set -euo pipefail

PKG_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PKG_SCRIPTS_DIR/../.." && pwd)"

COVA_VERSION="${COVA_VERSION:-}"
BINARY="${COVA_BINARY:-}"
OUT_DIR="${COVA_OUT_DIR:-$ROOT/dist}"
TARGET_ARCH="${COVA_ARCH:-$(uname -m)}"

die() { echo "error: $*" >&2; exit 1; }
note() { echo "==> $*" >&2; }

require() {
  for tool in "$@"; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
  done
}

manifest_version() {
  sed -n 's/^version[[:space:]]*=[[:space:]]*"\(.*\)"$/\1/p' "$ROOT/cova.toml" | head -1
}

# MSI ProductVersion is numeric, with up to four dot-separated fields. A semver
# pre-release suffix has no MSI representation, so only the numeric part is
# kept; MajorUpgrade in windows/cova.wxs allows same-version upgrades, which is
# what makes successive betas of the same patch version replace each other.
msi_version() {
  local full="$1" numeric
  numeric="${full%%-*}"
  numeric="${numeric%%+*}"
  local major minor patch
  IFS='.' read -r major minor patch <<< "$numeric"
  major="${major:-0}"
  minor="${minor:-0}"
  patch="${patch:-0}"
  for part in "$major" "$minor" "$patch"; do
    [[ "$part" =~ ^[0-9]+$ ]] || die "cannot map version '$full' to an MSI ProductVersion"
  done
  printf '%s.%s.%s' "$major" "$minor" "$patch"
}

# Resolve the defaults once: version from the manifest, binary from the
# bootstrap build tree unless the caller overrides either.
resolve_defaults() {
  if [[ -z "$COVA_VERSION" ]]; then
    COVA_VERSION="$(manifest_version)"
    [[ -n "$COVA_VERSION" ]] || die "cannot read version from cova.toml"
  fi
  if [[ -z "$BINARY" ]]; then
    for candidate in \
      "$ROOT/build/bootstrap/bin/cova" \
      "$ROOT/build/bootstrap/bin/cova.exe" \
      "$ROOT/build/bin/cova" \
      "$ROOT/build/bin/cova.exe"; do
      if [[ -x "$candidate" ]]; then
        BINARY="$candidate"
        break
      fi
    done
  fi
  [[ -n "$BINARY" && -x "$BINARY" ]] || die "no cova binary to package (set COVA_BINARY)"
  mkdir -p "$OUT_DIR"
  # Export the canonical variable names so per-format scripts started by
  # build-all.sh see the same version, binary, output directory and arch.
  export COVA_VERSION
  export COVA_BINARY="$BINARY"
  export COVA_OUT_DIR="$OUT_DIR"
  export COVA_ARCH="$TARGET_ARCH"
}

# Stages the canonical installed layout under $1.
stage_tree() {
  local dest="$1"
  rm -rf "$dest"
  mkdir -p "$dest/usr/bin" "$dest/usr/share/doc/cova"
  install -m 0755 "$BINARY" "$dest/usr/bin/cova"
  install -m 0644 "$ROOT/LICENSE" "$dest/usr/share/doc/cova/LICENSE"
  install -m 0644 "$ROOT/NOTICE" "$dest/usr/share/doc/cova/NOTICE"
}

# True when the binary is a fully static ELF, so a package can state it has no
# shared-library dependencies at all.
is_static_elf() {
  file -b "$BINARY" 2>/dev/null | grep -q "statically linked"
}
