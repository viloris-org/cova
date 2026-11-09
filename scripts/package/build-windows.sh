#!/usr/bin/env bash
# Cova — portable Windows archive.
#
# Produces dist/cova-<version>-windows-<arch>.zip containing cova.exe plus the
# license files. A zip rather than an installer: Cova has no code-signing
# certificate, so a plain archive the user can inspect is the honest option.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

resolve_defaults

arch="${COVA_PACKAGE_ARCH:-$TARGET_ARCH}"
stem="cova-${COVA_VERSION}-windows-${arch}"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$stem"
cp "$BINARY" "$stage/$stem/cova.exe"
chmod +x "$stage/$stem/cova.exe" 2>/dev/null || true
cp "$ROOT/LICENSE" "$stage/$stem/LICENSE"
cp "$ROOT/NOTICE" "$stage/$stem/NOTICE"
cat > "$stage/$stem/INSTALL.txt" <<'TXT'
Cova quick install
==================

  1. Extract this archive somewhere permanent, for example
     %LOCALAPPDATA%\Cova

  2. Add that directory to PATH, or run cova.exe with its full path.

  3. Verify:

       cova --version

Cova needs a C++26-capable MinGW-w64 GCC on PATH and Ninja to build projects.
Run `cova doctor` to check the environment.
TXT

output="$OUT_DIR/${stem}.zip"
if command -v zip >/dev/null 2>&1; then
  (cd "$stage" && zip -qr "$output" "$stem")
elif command -v bsdtar >/dev/null 2>&1; then
  # bsdtar picks the zip writer from the output suffix.
  (cd "$stage" && bsdtar -a -cf "$output" "$stem")
else
  die "packaging a Windows zip needs zip or bsdtar"
fi
note "built $output"
