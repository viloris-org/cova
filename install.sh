#!/usr/bin/env sh
# Cova — one-line installer.
#
#   curl -fsSL https://raw.githubusercontent.com/viloris-org/cova/main/install.sh | sh
#
# Installs the latest Cova build from GitHub Releases. Every published release
# is a beta, so this script only ever installs a beta: there is no stable
# channel yet, and pretending otherwise would imply a compatibility promise
# Cova has not made (PRD §26.2).
#
# Options (environment variables):
#   COVA_VERSION   install a specific version, e.g. 0.1.0-beta-1
#   COVA_PREFIX    installation prefix (default: /usr/local, or ~/.local when
#                  it is not writable)
#   COVA_BIN_DIR   override the directory the binary is placed in
#
# The download is verified against the release's SHA256SUMS before anything is
# installed.
set -eu

REPO="viloris-org/cova"
VERSION="${COVA_VERSION:-}"

say() { printf '%s\n' "$*" >&2; }
die() { say "error: $*"; exit 1; }

need() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

detect_os() {
  case "$(uname -s)" in
    Linux) echo linux ;;
    Darwin) echo macos ;;
    MINGW*|MSYS*|CYGWIN*)
      die "Windows releases are .zip archives; download one from https://github.com/$REPO/releases and extract it"
      ;;
    *) die "unsupported operating system: $(uname -s)" ;;
  esac
}

detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo x86_64 ;;
    aarch64|arm64) echo aarch64 ;;
    *) die "unsupported architecture: $(uname -m)" ;;
  esac
}

# Resolves the newest published beta when COVA_VERSION is unset. `latest` in the
# GitHub API ignores pre-releases, so the release list is filtered instead.
latest_version() {
  if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
    gh release list --repo "$REPO" --limit 20 --json tagName,isPrerelease \
      --jq '[.[] | select(.isPrerelease)][0].tagName' 2>/dev/null | sed 's/^v//'
    return
  fi
  need curl
  curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=20" |
    tr ',' '\n' |
    sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' |
    head -1
}

main() {
  os="$(detect_os)"
  arch="$(detect_arch)"

  if [ -z "$VERSION" ]; then
    VERSION="$(latest_version)"
    [ -n "$VERSION" ] || die "could not determine the latest Cova version; set COVA_VERSION"
  fi
  case "$VERSION" in
    *-beta*|*beta*) ;;
    *) say "note: $VERSION is not a beta; installing it anyway" ;;
  esac

  # macOS ships one universal binary; Linux is per-architecture.
  if [ "$os" = macos ]; then
    asset="cova-${VERSION}-macos-universal.tar.gz"
  else
    asset="cova-${VERSION}-linux-${arch}.tar.gz"
  fi

  base_url="https://github.com/$REPO/releases/download/v${VERSION}"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  say "==> downloading Cova $VERSION ($os/$arch)"
  need curl
  curl -fsSL "$base_url/$asset" -o "$tmp/$asset" || die "cannot download $asset"
  curl -fsSL "$base_url/SHA256SUMS" -o "$tmp/SHA256SUMS" || die "cannot download SHA256SUMS"

  say "==> verifying checksum"
  expected="$(grep " $asset\$" "$tmp/SHA256SUMS" | cut -d' ' -f1)"
  [ -n "$expected" ] || die "no checksum for $asset in SHA256SUMS"
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$tmp/$asset" | cut -d' ' -f1)"
  else
    actual="$(shasum -a 256 "$tmp/$asset" | cut -d' ' -f1)"
  fi
  [ "$expected" = "$actual" ] || die "checksum mismatch for $asset"

  say "==> extracting"
  tar -xzf "$tmp/$asset" -C "$tmp"
  bin="$(find "$tmp" -type f -name cova | head -1)"
  [ -n "$bin" ] || die "the archive does not contain a cova binary"

  # Prefer a system-wide prefix when it is writable, otherwise fall back to a
  # per-user directory instead of demanding sudo.
  if [ -n "${COVA_BIN_DIR:-}" ]; then
    dest="$COVA_BIN_DIR"
  else
    prefix="${COVA_PREFIX:-/usr/local}"
    dest="$prefix/bin"
    if [ ! -w "$dest" ] && [ ! -w "$(dirname "$dest")" ]; then
      dest="$HOME/.local/bin"
      say "note: $prefix/bin is not writable; installing to $dest"
    fi
  fi
  mkdir -p "$dest"
  install -m 0755 "$bin" "$dest/cova" 2>/dev/null || {
    cp "$bin" "$dest/cova" && chmod 0755 "$dest/cova"
  }

  say ""
  say "installed $dest/cova"
  "$dest/cova" --version
  case ":$PATH:" in
    *":$dest:"*) ;;
    *) say ""; say "add $dest to PATH:"; say "  export PATH=\"$dest:\$PATH\"" ;;
  esac
}

main "$@"
