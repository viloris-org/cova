#!/usr/bin/env bash
# Cova — Windows MSI installer.
#
# Produces dist/cova-<version>-windows-<arch>.msi containing cova.exe plus the
# licence files. The package is authored in windows/cova.wxs and built with
# WiX (v5 or newer), which the release job installs as a .NET global tool:
#
#   dotnet tool install --global wix --version 5.0.2
#
# The installer is per-machine and unsigned. Cova has no code-signing
# certificate, so SmartScreen will warn about an unknown publisher; that is
# stated in README.md rather than hidden.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require wix
resolve_defaults

# On Windows Arm64, MSYS2's uname still reports x86_64 because the MSYS2
# runtime is x64-emulated, so release builds set COVA_ARCH explicitly.
case "$TARGET_ARCH" in
  x86_64|amd64|x64) msi_arch=x64; asset_arch=x86_64 ;;
  aarch64|arm64)    msi_arch=arm64; asset_arch=arm64 ;;
  *) die "unsupported architecture for a Windows MSI: $TARGET_ARCH" ;;
esac

stem="cova-${COVA_VERSION}-windows-${asset_arch}"
output="$OUT_DIR/${stem}.msi"
product_version="$(msi_version "$COVA_VERSION")"

# WiX is a native Windows executable. When this script runs in MSYS2 or Cygwin,
# hand it Windows-shaped paths; on a Unix host the paths stay as they are (and
# WiX is not available, so `require wix` above already failed).
native_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -m "$1"
  else
    printf '%s' "$1"
  fi
}

source_file="$(native_path "$PKG_SCRIPTS_DIR/windows/cova.wxs")"
binary_file="$(native_path "$BINARY")"
license_file="$(native_path "$ROOT/LICENSE")"
notice_file="$(native_path "$ROOT/NOTICE")"
output_file="$(native_path "$output")"

note "building $output"
# Do not leave a .wixpdb in dist/: it is not an installable release asset and
# SHA256SUMS only covers the installers.
wix build \
  -arch "$msi_arch" \
  -pdbtype none \
  -d "MsiVersion=$product_version" \
  -d "FullVersion=$COVA_VERSION" \
  -d "BinarySource=$binary_file" \
  -d "LicenseSource=$license_file" \
  -d "NoticeSource=$notice_file" \
  -o "$output_file" \
  "$source_file"

[[ -f "$output" ]] || die "WiX did not produce $output"
note "built $output"
