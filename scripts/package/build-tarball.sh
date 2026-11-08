#!/usr/bin/env bash
# Cova — portable tarball for any Unix-like host.
#
# Produces dist/cova-<version>-<os>-<arch>.tar.gz with bin/cova plus the license
# files, for systems without a native package manager.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require bsdtar
resolve_defaults

os_name="${COVA_PACKAGE_OS:-$(uname -s | tr '[:upper:]' '[:lower:]')}"
# macOS publishes one universal binary, so the archive is not per-architecture.
tar_arch="${COVA_PACKAGE_ARCH:-$TARGET_ARCH}"
stem="cova-${COVA_VERSION}-${os_name}-${tar_arch}"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$stem/bin" "$stage/$stem/share/doc/cova"
install -m 0755 "$BINARY" "$stage/$stem/bin/cova"
install -m 0644 "$ROOT/LICENSE" "$stage/$stem/share/doc/cova/LICENSE"
install -m 0644 "$ROOT/NOTICE" "$stage/$stem/share/doc/cova/NOTICE"
cat > "$stage/$stem/INSTALL" <<'TXT'
Cova quick install
==================

  sudo install -m 0755 bin/cova /usr/local/bin/cova

Verify:

  cova --version

Cova also needs a C++26-capable compiler (Clang >= 17 or GCC >= 14) and Ninja
to build projects. Run `cova doctor` to check the environment.
TXT

output="$OUT_DIR/${stem}.tar.gz"
(cd "$stage" && bsdtar --uid 0 --gid 0 -czf "$output" "$stem")
note "built $output"
