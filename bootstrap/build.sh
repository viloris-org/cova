#!/usr/bin/env bash
# Cova — Stage 0 bootstrap (PRD §16.2, §16.3).
#
# Builds Cova from a clean checkout using only a system C++ compiler and Ninja.
# This is deliberately the least clever build in the project: no Cova, no
# registry, no network, no third-party libraries.
#
# Usage:
#   ./bootstrap/build.sh                 # build into ./build/bootstrap
#   ./bootstrap/build.sh --test          # build, then run the test suite
#   ./bootstrap/build.sh --clean         # remove the bootstrap tree
#   COVA_CXX=clang++ ./bootstrap/build.sh
#
# Exit codes: 0 success, 1 build/test failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX="${COVA_CXX:-}"
JOBS="${COVA_JOBS:-$(nproc 2>/dev/null || echo 4)}"
PYTHON="${COVA_PYTHON:-}"
BUILD_DIR="${COVA_BUILD_DIR:-$ROOT/build/bootstrap}"
RUN_TESTS=0
CLEAN=0

usage() {
  sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
}

for arg in "$@"; do
  case "$arg" in
    --test) RUN_TESTS=1 ;;
    --clean) CLEAN=1 ;;
    -j*) JOBS="${arg#-j}" ;;
    --jobs=*) JOBS="${arg#--jobs=}" ;;
    -h|--help) usage ;;
    *) echo "error: unknown argument '$arg'" >&2; exit 2 ;;
  esac
done

if [[ "$CLEAN" == "1" ]]; then
  rm -rf "$BUILD_DIR"
  echo "removed $BUILD_DIR"
  exit 0
fi

# --- Prerequisites ----------------------------------------------------------
if ! command -v ninja >/dev/null 2>&1; then
  echo "error: ninja is required for the bootstrap build" >&2
  echo "  Debian/Ubuntu: sudo apt install ninja-build" >&2
  echo "  Fedora:        sudo dnf install ninja-build" >&2
  echo "  macOS:         brew install ninja" >&2
  exit 2
fi

# MSYS2 calls it `python`, most Unix hosts call it `python3`.
if [[ -z "$PYTHON" ]]; then
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
      PYTHON="$candidate"
      break
    fi
  done
fi
if [[ -z "$PYTHON" ]] || ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "error: Python 3 is required for the bootstrap build" >&2
  exit 2
fi

# Pick a compiler: honour COVA_CXX, else prefer clang++, then g++.
if [[ -z "$CXX" ]]; then
  for candidate in clang++ g++ c++; do
    if command -v "$candidate" >/dev/null 2>&1; then
      CXX="$candidate"
      break
    fi
  done
fi
if [[ -z "$CXX" ]] || ! command -v "$CXX" >/dev/null 2>&1; then
  echo "error: no C++ compiler found (tried clang++, g++, c++)" >&2
  exit 2
fi

# Verify the compiler actually accepts the C++ standard Cova targets. PRD §16.4
# requires this to be a probe, not an assumption from a version number.
echo "==> compiler: $CXX ($("$CXX" --version 2>/dev/null | head -1))"
probe_dir="$(mktemp -d)"
trap 'rm -rf "$probe_dir"' EXIT
cat > "$probe_dir/probe.cpp" <<'PROBE'
#include <cstdio>
#include <expected>
#include <format>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

template <class T>
concept Sized = requires(T t) { { t.size() } -> std::convertible_to<std::size_t>; };

struct Point { int x; int y; };

int main() {
  std::vector<int> values{1, 2, 3};
  static_assert(Sized<decltype(values)>);
  auto doubled = values | std::views::transform([](int v) { return v * 2; });
  Point p{.x = 1, .y = 2};
  std::expected<int, int> good = p.x + p.y;
  if (!good) {
    return 1;
  }
  auto text = std::format("{}", *good);
  std::print("{}", text);
  std::puts("");
  return 0;
}
PROBE

for std_flag in -std=c++26 -std=c++2c -std=c++23; do
  if "$CXX" "$std_flag" -o "$probe_dir/probe" "$probe_dir/probe.cpp" >/dev/null 2>&1; then
    if "$probe_dir/probe" >/dev/null 2>&1; then
      echo "    baseline: $std_flag accepted and the probe runs"
      COVA_STD="$std_flag"
      break
    fi
  fi
done

if [[ -z "${COVA_STD:-}" ]]; then
  echo "error: $CXX cannot compile the Cova baseline" >&2
  echo "  Cova needs std::expected, std::print, concepts, ranges and designated" >&2
  echo "  initializers. Use Clang >= 17 or GCC >= 14." >&2
  exit 2
fi

echo "==> building into $BUILD_DIR"
echo "    jobs: $JOBS"
echo

# --- Build ------------------------------------------------------------------
COVA_BUILD_DIR="$BUILD_DIR" "$PYTHON" "$ROOT/scripts/devbuild.py" build -j "$JOBS"

echo
echo "==> built: $BUILD_DIR/bin/cova"
"$BUILD_DIR/bin/cova" --version

# --- Self-check -------------------------------------------------------------
echo
echo "==> smoke test: cova new + cova build in a scratch directory"
scratch="$(mktemp -d)"
smoke_home="$(mktemp -d)"
set +e
(
  cd "$scratch"
  export COVA_HOME="$smoke_home"
  "$BUILD_DIR/bin/cova" new smoke >/dev/null 2>&1 &&
    cd smoke &&
    "$BUILD_DIR/bin/cova" build >/dev/null 2>&1 &&
    "$BUILD_DIR/bin/cova" run >/dev/null 2>&1
)
smoke=$?
set -e
rm -rf "$scratch" "$smoke_home"

if [[ "$smoke" -ne 0 ]]; then
  echo "warning: the bootstrap smoke test failed (exit $smoke)" >&2
  echo "  the binary exists; check \`cova doctor\` for the cause" >&2
else
  echo "    ok: cova new -> cova build -> cova run"
fi

# --- Tests ------------------------------------------------------------------
if [[ "$RUN_TESTS" == "1" ]]; then
  echo
  echo "==> running the test suite"
  COVA_BUILD_DIR="$BUILD_DIR" "$PYTHON" "$ROOT/scripts/devbuild.py" test -j "$JOBS"
fi

echo
echo "done."
echo "  binary: $BUILD_DIR/bin/cova"
echo "  next:   export PATH=\"$BUILD_DIR/bin:\$PATH\""
