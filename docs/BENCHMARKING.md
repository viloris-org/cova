# Benchmarking Cova

`scripts/bench.py` measures Cova against CMake + Ninja on the same sources, the
same compiler and the same flags, so the numbers compare build systems rather
than C++ code. It is dependency-free (Python 3.9+, no third-party packages) and
writes a Markdown report plus a JSON file with every raw sample.

Latest results on the development host: [cova-vs-cmake.md](bench/cova-vs-cmake.md).

## Running it

Build an optimised Cova first. A `-O0` developer build measures its own lack of
optimisation, not the build system:

```bash
COVA_BUILD_DIR=build/bench-release \
COVA_CXXFLAGS="-O2 -DNDEBUG" \
  python3 scripts/devbuild.py build

python3 scripts/bench.py                       # 40 TUs, 5 samples
python3 scripts/bench.py --tus 10,40,120 --real --cache-ab \
    --markdown docs/bench/cova-vs-cmake.md \
    --json docs/bench/cova-vs-cmake.json
```

`--quick` is a smoke run: 8 TUs, one sample, no warmup. `--keep` preserves the
scratch trees under `.cache/bench/` for inspection.

Useful options:

| Option | Effect |
|---|---|
| `--tus 10,40,120` | One table row per project size |
| `--runs`, `--warmups` | Timed samples and discarded samples per scenario |
| `--jobs` | Parallel jobs passed to both tools (default: all cores) |
| `--real` | Also build a copy of Cova's own `src/` with both tools |
| `--cache-ab` | Also measure Cova incremental builds with `--offline` |
| `--job-sweep` | Also measure cold builds at `-j1` |
| `--compiler` | C++ compiler for both tools (default: `clang++`) |
| `--cova` | Cova binary (default: `build/bench-release/bin/cova`) |
| `--tools` | `cova`, `cmake`, or both (default: both) |

The harness needs `cmake`, `ninja` and the chosen compiler on `PATH`.

## What is measured

All scenarios use one generated project: `include/common.h` pulls in the
standard library, every unit has its own header and source, and `main.cpp`
calls every unit. A source edit therefore rebuilds one file; editing
`common.h` rebuilds all of them.

| Scenario | Cova | CMake + Ninja |
|---|---|---|
| `startup` | `cova --version` | `cmake --version`, `ninja --version` |
| `cold` | `cova build`, fresh `COVA_HOME`, no `target/`, no lockfiles | `cmake -S . -B build -G Ninja` + `cmake --build build` |
| `cold-cache` | `cova build`, warm shared cache, no `target/` | — (no equivalent) |
| `noop` | `cova build` with nothing changed | `cmake --build build` |
| `noop-state` | `cova build --offline` | — (Cova-only A/B) |
| `one-file` | one source rewritten, then `cova build` | one source rewritten, then `cmake --build` |
| `plan` | `cova build --dry-run` | `ninja -n` |
| `real-cold`, `real-noop` | same, on Cova's own `src/` | same, on Cova's own `src/` |

Cold runs deliberately remove the shared cache from the picture: a fresh
`COVA_HOME` means no cached probes and no artifact cache, so a cold number is a
real cold number. Warm scenarios reuse the tree each tool left behind.

## Why `--offline` is a fair cache switch

Cova's shared artifact cache is on by default and is not something Ninja has.
`cova build --offline` disables it (`docs/CORRECTNESS.md` §6) while leaving the
local incremental record intact, which isolates the cache's cost from the
incremental machinery. That A/B is reported separately and never mixed into the
Cova-vs-CMake ratio.

## Rules for honest numbers

- **Same compiler, same flags.** Both tools receive the same `-std`, `-O`, `-g`
  and warning flags. CMake is configured with `CMAKE_BUILD_TYPE=Debug` to match
  Cova's `dev` profile (`-O0 -g`).
- **Same filesystem.** Both trees live under `.cache/bench/` on the same disk,
  so neither tool pays a different I/O price.
- **Warm filesystem.** Every sample is preceded by `os.sync()`; that flushes
  dirty pages but does not drop the page cache, which is what a developer
  experiences.
- **Fresh process per sample.** Nothing is measured in-process, so startup cost
  is included for both tools.
- **Medians, plus the range.** A desktop machine has noise. The report shows
  the median and the observed min/max; read the range before trusting a small
  difference.
- **Never compare across machines.** PRD §19.1 requires benchmarks with fixed
  hardware, OS, filesystem and project. Treat any number here as a snapshot of
  one machine.

## Interpreting the results

Four consistent patterns:

1. **Cold builds are at parity with CMake + Ninja.** Both tools run exactly one
   compiler process per translation unit. Cova used to add a preprocessing pass
   per unit to discover the header closure before compiling; it no longer does,
   because the depfile the compile writes is the closure, and a cold build has no
   cache entry to look up with a pre-computed key. See `docs/CORRECTNESS.md` §5.
   Cova still pays a full capability probe when `COVA_HOME` is fresh, which CMake
   pays for in its configure step instead.
2. **No-change builds are stat-driven.** The state file records, per input and
   output, a content hash plus the metadata sample it was computed from. A build
   with nothing to do is one `stat` per file and one closure-free plan key per
   action: no file is re-read. A `touch` that does not change bytes does not
   rebuild; a content change that keeps the size still does. The remaining cost
   at large project sizes is planning: one depfile to read and parse per compile
   action, whether or not anything changed.
3. **The shared cache serves missing work, not present work.** The incremental
   record is consulted before the cache, so an unchanged action is never
   "restored" over a file that was already correct. `--offline` therefore changes
   nothing on a no-change build - that is the intended result, not a bug in the
   switch.
4. **Cova's own advantage: the shared artifact cache.** Deleting `target/` and
   rebuilding from a warm cache restores the whole output set instead of
   recompiling, and the restore writes the depfiles back, so the restored tree is
   warm for the next build. Ninja cannot express that workload.

## Adding a scenario

Scenarios are methods on `Bench`, recorded through `self.record(name, tool,
size, fn)`, where `fn` returns a dict with at least `"s"` (seconds). Add extra
keys (like `executed` or `restored`) and the JSON output carries them. Keep a
scenario's setup inside `fn` if it is part of what a user waits for, and outside
if it is harness bookkeeping.
