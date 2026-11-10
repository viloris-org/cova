# Cova — Migrating an Existing Project

> **Applies to Cova 0.1.0-beta-2.** The manifest schema and the main CLI are
> beta; this document describes the keys the binary in this tree accepts today.
> Check `cova <command> --help` and the manifest parser before relying on a key
> this guide does not mention.

Cova has **no CMake, Meson, Make or Bazel compatibility layer**, and this
milestone does not add one: PRD §21 lists "CMake project adaptation" as future
work and OD-004 leaves the mapping design open. Migration therefore means
rewriting the build description as one or more `cova.toml` packages.

That is deliberate. A half-working importer that silently drops a define, an
include path or a link dependency produces wrong binaries; a migration guide
plus a verifiable workflow does not. Cova never modifies or deletes existing
build configuration files. `cova init` detects `CMakeLists.txt`, `meson.build`,
`Makefile` and `BUILD.bazel`, reports them, and leaves them untouched, so you
can migrate incrementally and keep the old build as the reference until parity
is proven.

- [1. The mental model](#1-the-mental-model)
- [2. What Cova can express](#2-what-cova-can-express)
- [3. What cannot be expressed](#3-what-cannot-be-expressed-and-what-to-do-instead)
- [4. Inventory the old build first](#4-inventory-the-old-build-first)
- [5. The migration workflow](#5-the-migration-workflow)
- [6. CMake mapping reference](#6-cmake-mapping-reference)
- [7. Verification checklist](#7-verification-checklist)
- [8. Meson, Make/Autotools and Bazel](#8-meson-makeautotools-and-bazel)
- [9. Agent-assisted migration](#9-agent-assisted-migration)
- [10. Troubleshooting](#10-troubleshooting)
- [11. Quick manifest reference](#11-quick-manifest-reference)

---

## 1. The mental model

Four rules explain most of the mapping:

1. **One package is one build target.** A package is a directory with a
   `cova.toml`, one `[build] type` (`executable`, `static-library` or
   `shared-library`) and one artifact. Two CMake targets need two packages.
2. **A workspace is a set of packages.** A root `cova.toml` with
   `[workspace] members = [...]` builds them together, with one `target/`
   directory at the workspace root.
3. **Dependencies are local packages.** `[dependencies]` entries with `path`
   are resolved transitively, built for the requested target, and linked. Their
   `public-includes` and `public-definitions` propagate. Nothing else does.
4. **The manifest is declarative, not a script.** There are no custom commands,
   generators, configure steps or install rules. If a step needs to run code
   before compilation, Cova cannot express it yet.

Outputs live under:

```
target/<short-target-triple>/<profile>/bin|lib|obj
```

For example `target/x86_64-linux-gnu/dev/bin/app` or
`target/aarch64-linux-musl/release/lib/libengine.a`.

---

## 2. What Cova can express

| Build concern | Declaration |
|---|---|
| Target kind | `[build] type = "executable"` / `"static-library"` / `"shared-library"` |
| Sources | `[build] sources = ["src/**/*.cpp"]`, `source-dirs` (fallback when no sources match), `exclude` |
| C++ standard | `[package] edition = "c++20"` (C sources always get `-std=c17`) |
| Include directories | `public-includes` (also visible to dependents), `private-includes` |
| Preprocessor definitions | `public-definitions` (also visible to dependents), `private-definitions` |
| Compiler flags | `[build] compile-options = ["-Wall", "-Werror"]` |
| Linker flags | `[build] link-options = ["-Wl,--gc-sections"]` |
| System libraries | `[build] link-libraries = ["z", "/opt/lib/libfoo.a"]` |
| Build profiles | `[profile.dev]`, `[profile.release]`, custom `[profile.<name>]`; `--profile <name>` |
| Per-target overrides | `[targets.<triple>]` with `definitions`, `compile-options`, `link-options` |
| Local dependencies | `[dependencies] name = { path = "../path" }` |
| Workspace | `[workspace] members = ["libs/*", "apps/*"]` |
| Tests | `[test] sources = ["tests/**/*.cpp"]`; one binary per source |
| Formatting | `[format]`; `cova fmt [--check] [--diff]` |
| Toolchain selection | `[toolchain] policy`, `compiler`, `version`, `auto-install`, `preferred` |
| Default target | `[build] target = "aarch64-unknown-linux-musl"` |
| Test runner | CLI `--runner native|qemu|wine`; default in `~/.cova/config.toml` (`runtime.default_runner`) |

`link-libraries` entries become linker input as follows:

- a plain name such as `"z"` becomes `-lz`;
- a path containing `/` or `\` is passed as-is;
- an entry starting with `-` is passed verbatim.

Use `link-options` for anything that is more than one token (`-framework
Cocoa`, `-Wl,-rpath,...`, `-L<dir>`), because each array entry is appended as a
single argument.

---

## 3. What cannot be expressed (and what to do instead)

Read this table before committing to a migration. If a project depends on the
left column, the migration is incomplete unless the workaround is acceptable.

| Old build feature | Cova status | Workaround |
|---|---|---|
| Custom commands / generators (`add_custom_command`, `genrule`, `custom_target`) | Not supported; `build-script = true` is rejected explicitly | Generate outside Cova and commit the output, or keep the generator as a separate manual step |
| `configure_file`, Autotools `./configure` | Not supported | Commit the configured output, or run the configuration step before `cova build` |
| Multiple targets in one build file | One target per package | Split into workspace members or path-dependency packages |
| Per-file or per-language flags (`set_source_files_properties`, `$<COMPILE_LANGUAGE:CXX>`) | Not supported | Apply flags to the whole package, or move the files into another package |
| Platform-specific source sets | `[targets.<triple>]` has no `sources` | Guard with the preprocessor, or put platform code in a package enabled by a target-conditioned dependency |
| `install()`, `export()`, CPack, `meson install`, `make install` | `cova install/package/publish` are not implemented | Consume artifacts from `target/<triple>/<profile>/` |
| Remote/git/registry dependencies (`FetchContent`, `ExternalProject`, vcpkg, Conan) | Only local `path` dependencies are resolved | Vendor the sources and declare a path dependency, or use a system-installed library |
| CMake package config, `pkg-config` integration | Not implemented | Run `pkg-config` manually and transcribe `-I`, `-D`, `-l` and `-L` |
| Precompiled headers | Not supported | Remove the PCH or keep it out of the Cova build |
| C++ Modules (`.ixx`, `.cppm`) | Not supported | Do not migrate the module build yet |
| MSVC (`cl.exe`) | Not supported | Use a MinGW-w64 GCC on Windows |
| Resource compiler (`.rc`, `windres`) | Not supported | Compile the resource outside Cova and pass the object through `link-options` (not tracked as an input) |
| `WINDOWS_EXPORT_ALL_SYMBOLS`, `.def` files, version scripts | No automatic export handling | Pass the `.def`/script through `link-options` (for MinGW, `-Wl,--export-all-symbols`), or annotate exports in the source |
| Assembly sources (`.s`, `.S`, `.asm`) | Not compiled; only known C/C++ extensions are collected | Pre-assemble and pass the object through `link-options` (not tracked as an input), or wrap the code |
| Unity builds | Not needed | Compile normally, or generate one unity `.cpp` and exclude the originals |
| Compiler launcher (`ccache`, `sccache`) | No launcher setting | Not available in this milestone |
| Per-manifest test runner (`[test] runner`, `runner-command`) | Parsed but not applied | Use `cova test --runner qemu`, or set `runtime.default_runner` in `~/.cova/config.toml` |
| Per-test arguments, environment, working directory or fixtures (CTest properties) | Not supported; `cova test --` applies the same arguments to every test binary | Put defaults in the test binary, or use a wrapper |
| Per-target kind and dependency selection (`[targets.<triple>] type/dependencies`) | Parsed but not applied; only `definitions`, `compile-options` and `link-options` are used | Use separate packages, or target-conditioned entries in `[dependencies]` |
| CMake `try_compile` / feature probes | No configure phase | Decide from known targets and put definitions in the manifest |

Four more consequences of the current design:

- **`edition` selects the dialect spelling.** `edition = "c++20"` compiles with
  `-std=c++20`, not `gnu++20`. CMake defaults `CXX_EXTENSIONS` to ON, so use
  `edition = "gnu++20"` when the old build relied on GNU extensions.
- **Shared-library packages need `-fPIC` in `compile-options`.** The planner
  passes `-fPIC` to the shared-library *link* command, not to the compiles; add
  `compile-options = ["-fPIC"]` to every `shared-library` package in this
  milestone.
- **Link libraries do not propagate.** If package `lib` needs `-lz`, a
  dependent executable must declare `"z"` too. Only public include directories
  and public definitions propagate.
- **C is always `-std=c17`.** A mixed C/C++ package gets `-std=c17` for `.c`
  files and `-std=<edition>` for C++. There is no per-language option list, so
  a `-std=gnu11` added to `compile-options` would also hit the C++ files.

---

## 4. Inventory the old build first

The old build is the source of truth. Capture it before editing anything.

**CMake**

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose        # link lines and custom commands
cat build/compile_commands.json      # exact compile flags per TU
```

**Meson**

```bash
meson setup builddir
ninja -C builddir -v
```

**Make / Autotools**

```bash
make -n                              # dry run
make V=1                            # real commands
```

**Bazel**

```bash
bazel aquery 'mnemonic("CppCompile", //...)'   # compile actions
bazel aquery 'mnemonic("CppLink", //...)'      # link actions
```

Write down, per target:

- kind (executable, static, shared, interface/header-only, object library);
- source files, including generated and platform-specific files;
- include directories, split into PUBLIC / PRIVATE / SYSTEM;
- preprocessor definitions, split into PUBLIC / PRIVATE;
- compile options, especially language-specific and per-file options;
- link libraries, link options, rpath and SONAME settings;
- build types and their flags;
- test sources, test registration and test data;
- generated files, custom commands and configure steps;
- install/export/package rules;
- cross-compilation settings from the toolchain file.

This inventory is also the checklist for "what did the migration lose?".

---

## 5. The migration workflow

### Step 1 — work on a branch

```bash
git switch -c cova-migration
```

Keep the old build runnable. Do not delete `CMakeLists.txt`, `meson.build`, the
`Makefile` or `BUILD.bazel` in this change.

### Step 2 — initialise one package directory

For a single-target project, run this at the repository root:

```bash
cd /path/to/project
cova init
```

`cova init` writes `cova.toml`, and `.gitignore`/`.clang-format` when absent. It
does not overwrite sources and it reports any foreign build files it found. If
the project already has a `cova.toml`, `--force` is required to replace it.

For a multi-target project, decide the package layout first (Step 3) and run
`cova init` inside each package directory, or write the manifests by hand.

### Step 3 — decide the package layout

If the old project has one target, there is nothing to split: keep the
`cova.toml` that `cova init` wrote at the repository root and adjust `[build]`
to match the compile database.

For multiple targets, use one package per target. CMake projects that already
use `add_subdirectory` often map cleanly:

```
repo/
  cova.toml                 # [workspace]
  libs/engine-core/
    cova.toml               # static-library
    include/
    src/
  apps/engine-cli/
    cova.toml               # executable
    src/
  tests/
```

Two constraints shape this:

- A package root is the directory containing its `cova.toml`; source globs are
  relative to it and may not contain `..`.
- Two packages cannot share the same root, so two CMake targets in one directory
  need separate directories.

If you cannot move sources, you can point a package at a source tree elsewhere
with a non-glob directory entry such as `sources = ["../../src"]` (it walks the
directory). That escapes the package root and is not recommended; prefer
reorganising or moving shared code into a path dependency.

### Step 4 — write each manifest from the compile database

Start with the target's own compile command. For each `arguments` entry in
`compile_commands.json`:

- `-std=` / `CXX_STANDARD` → `[package] edition`;
- `-I<dir>` → `public-includes` or `private-includes`, depending on whether
  consumers need it;
- `-D<name>=<value>` → `public-definitions` or `private-definitions`;
- warning and codegen flags → `compile-options`;
- `-O*`, `-g`, `-DNDEBUG`, `-flto`, sanitizer flags → the matching
  `[profile.<name>]` (see Step 7);
- target/sysroot flags → let Cova's toolchain resolver supply them;
- `-L`, `-Wl,...`, `-framework` and other multi-token flags → `link-options`;
- `-l<name>` → `link-libraries`.

The `cova.toml` files are relative to each package root. Flags that are
relative paths (for example `-Wl,--version-script=exports.map`) are resolved
from the package root because all actions run with that working directory.

### Step 5 — dependencies

For each dependency:

- **Local target or vendored source** → give it a package and use
  `{ path = "../name" }`.
- **Header-only dependency** → a `static-library` package with no matching
  sources and the headers in `public-includes`. Cova notes that there is
  nothing to build, and dependents still get the include path.
- **System library** → `link-libraries`, plus `private-includes` or
  `link-options = ["-L<dir>"]` when it is not in the default search path.
- **Remote dependency** → vendor it. There is no fetcher in this milestone.

### Step 6 — tests

Declare tests with `[test] sources`. If the `[test]` table exists but omits
`sources`, it defaults to `tests/**/*.cpp`; if the table is absent, Cova does
not discover tests on its own. Cova compiles each matched test source into its
own executable and runs it; a test passes when the executable exits 0. Test
binaries link the package's objects (except the package's `main` object) and its
dependency artifacts, so they can call your implementation directly.

This means:

- every test source needs its own `main` — either written by you, or supplied by
  a framework such as GoogleTest's `gtest_main` or Catch2's `Catch2Main`;
- test file stems must be unique, because the binary is named after the stem;
- there is no test discovery or per-test registration in the manifest. The
  binary decides what to run.

GoogleTest recipe (system install):

```toml
[build]
link-libraries = ["gtest", "gtest_main"]

[test]
sources = ["tests/**/*.cpp"]
```

Every test source then contains only `TEST(...)` cases. If GoogleTest is
installed outside the default prefix, add its include directory to
`private-includes` and its library directory to `link-options`.

Catch2 v3 usually wants its main library plus its include and library
directories:

```toml
[build]
private-includes = ["/opt/catch2/include"]
link-libraries = ["Catch2Main", "Catch2"]
link-options = ["-L/opt/catch2/lib"]
```

For header-only Catch2 v2 there is no shared main object: each test binary is
built from one test source, so define the main in that source:

```cpp
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
```

Cova's own in-tree framework under `src/testing` is part of the Cova source
tree, not an installed SDK. Do not add it as a dependency of a migrated project.

Pass framework arguments through with `--`:

```bash
cova test -- --gtest_filter=Engine.*
cova test --filter "[render]"        # bare positional argument to the test binary
```

The `--filter` value is passed as a single positional argument, which suits
Catch2 test specs but not GoogleTest's `--gtest_filter=...`; use the `--`
passthrough for GoogleTest.

### Step 7 — profiles and build types

Cova always has a `dev` profile (no `--profile` needed) and a `release` profile
(`--release`). Both are defined in the manifest and can be replaced:

```toml
[profile.dev]
optimization = 0
debug = true
assertions = true

[profile.release]
optimization = 3
debug = false
lto = true
assertions = false
```

Common CMake build types:

| CMake build type | Cova invocation | Profile contents |
|---|---|---|
| `Debug` | `cova build` | `[profile.dev]`, the default |
| `Release` | `cova build --release` | `[profile.release]` |
| `RelWithDebInfo` | `cova build --profile relwithdebinfo` | `optimization = 2`, `debug = true`, `assertions = false`, `lto = false` |
| `MinSizeRel` | `cova build --profile minsize` | `optimization = 2`, `debug = false`, `assertions = false`, `extra-cxxflags = ["-Os"]` |

Example:

```toml
[profile.relwithdebinfo]
optimization = 2
debug = true
assertions = false
lto = false

[profile.minsize]
optimization = 2
debug = false
assertions = false
extra-cxxflags = ["-Os"]
```

`assertions = false` adds `-DNDEBUG`. `lto = true` adds `-flto` to both compile
and link. `sanitize = "address"` (or `"undefined"`, `"thread"`) adds the
sanitizer to both. Profile flags are compiler-family independent; put raw
compiler-specific switches in `extra-cxxflags` / `extra-ldflags`.

Note that Cova's `release` profile enables LTO by default, while CMake's
`Release` does not. Set `lto = false` in `[profile.release]` if you need the old
behaviour.

### Step 8 — cross-compilation

A CMake toolchain file mostly maps to CLI flags and target overrides:

```bash
cova target list
cova toolchain explain --target aarch64-unknown-linux-musl
cova build --target aarch64-unknown-linux-musl
```

```toml
[build]
target = "aarch64-unknown-linux-musl"   # default when --target is absent

[targets.aarch64-unknown-linux-musl]
compile-options = ["-DARCH_ARM=1"]
link-options = ["-Wl,--gc-sections"]
```

Target override keys are canonicalised, so `[targets.x86_64-w64-mingw32]` and
`[targets.x86_64-pc-windows-gnu]` select the same target. Cova resolves the
compiler and sysroot itself (system first, managed fallback); see
[TOOLCHAIN-RESOLUTION.md](TOOLCHAIN-RESOLUTION.md).

Platform-specific source sets cannot be expressed directly. Put the platform
code in a package and enable it with a target-conditioned dependency:

```toml
[dependencies]
win-backend = { path = "platform/win", target = ["windows"] }
posix-backend = { path = "platform/posix", target = ["linux", "macos"] }
```

`target` filters accept a canonical triple, an architecture (`x86_64`), or an
OS (`windows`, `linux`, `macos`).

For tests under emulation, pass the runner on the command line (the
`[test] runner` manifest key is accepted but not applied in this milestone):

```bash
cova test --runner qemu            # or --runner wine
```

The default can also be set in `~/.cova/config.toml`:

```toml
[runtime]
default_runner = "qemu"
```

### Step 9 — verify, then switch CI

See §7. Keep both builds in CI until the Cova build passes the same test suite
and produces comparable binaries. Only then remove the old build files, and do
that by hand.

---

## 6. CMake mapping reference

| CMake | Cova |
|---|---|
| `add_executable(app ...)` | Package with `[build] type = "executable"` |
| `add_library(x STATIC ...)` | `type = "static-library"` |
| `add_library(x SHARED ...)` | `type = "shared-library"` plus `compile-options = ["-fPIC"]` (see §3) |
| `add_library(x INTERFACE ...)` | `static-library` package with headers in `public-includes` and no sources |
| `add_library(x OBJECT ...)` | `static-library` package (archive instead of raw objects) |
| `add_library(x)` with `BUILD_SHARED_LIBS` | One kind per package; choose static or shared, and create a second package for the other kind |
| `target_sources` / source list | `[build] sources` globs; use `exclude` for files that must not compile |
| `target_include_directories(... PUBLIC ...)` | `public-includes` |
| `target_include_directories(... PRIVATE ...)` | `private-includes` |
| `target_include_directories(... SYSTEM ...)` | `compile-options = ["-isystem", "<dir>"]` |
| `target_compile_definitions(... PUBLIC ...)` | `public-definitions` |
| `target_compile_definitions(... PRIVATE ...)` | `private-definitions` |
| `target_compile_options(... PRIVATE ...)` | `compile-options` |
| `target_link_libraries(... PRIVATE m)` | `link-libraries = ["m"]` on the package that links |
| `target_link_libraries(... PUBLIC m)` | Same on the final executable; system libs do not propagate |
| `target_link_options` | `link-options` |
| `set(CMAKE_CXX_STANDARD 20)` | `[package] edition = "c++20"` |
| `set(CMAKE_C_STANDARD ...)` | Not selectable; C is compiled as `-std=c17` |
| `set(CMAKE_CXX_FLAGS_DEBUG ...)` | `[profile.dev] extra-cxxflags` (or `compile-options` if unconditional) |
| `CMakePresets.json` | Map each preset to a profile plus `--target`/`--profile`; there is no preset file |
| `set(CMAKE_BUILD_TYPE ...)` | `--profile` / `--release` |
| `add_subdirectory(sub)` | Workspace member or path dependency |
| `find_package(Foo)` | `link-libraries` + include/defines transcribed by hand |
| `pkg_check_modules(...)` | Run `pkg-config` manually; transcribe flags |
| `FetchContent` / `ExternalProject_Add` | Vendor and use `{ path = ... }` |
| `configure_file` | Generate before the build and commit the result |
| `add_custom_command` / `add_custom_target` | Not supported |
| `install(...)` / `export(...)` | Not supported |
| `enable_testing()` / `add_test(...)` | `[test] sources` |
| `add_test(NAME foo COMMAND foo)` | One test source per binary; binary name is the source stem |
| `set_target_properties(... OUTPUT_NAME ...)` | `[package] name` (it names the artifact) |
| `POSITION_INDEPENDENT_CODE` | `compile-options = ["-fPIC"]` |
| `target_precompile_headers` | Not supported |
| Generator expressions | Evaluate manually; split targets per configuration |
| `if(WIN32)` source/flag selection | `[targets.<triple>]` plus target-conditioned dependencies |
| `CMAKE_SYSROOT` / toolchain file | `--target` plus Cova's toolchain resolver |

### Before / after example

Before, a two-target CMake project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(engine CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(engine_core STATIC src/engine.cpp src/render.cpp)
target_include_directories(engine_core PUBLIC include PRIVATE src)
target_compile_definitions(engine_core PUBLIC ENGINE_VERSION=2 PRIVATE ENGINE_INTERNAL=1)
target_link_libraries(engine_core PUBLIC Threads::Threads PRIVATE z)

add_executable(engine_cli app/main.cpp)
target_link_libraries(engine_cli PRIVATE engine_core)
```

After, a workspace:

`cova.toml` at the repository root:

```toml
[workspace]
members = ["libs/engine-core", "apps/engine-cli"]
```

`libs/engine-core/cova.toml`:

```toml
[package]
name = "engine_core"
version = "0.1.0"
edition = "c++20"

[build]
type = "static-library"
sources = ["src/**/*.cpp"]
public-includes = ["include"]
private-includes = ["src"]
public-definitions = ["ENGINE_VERSION=2"]
private-definitions = ["ENGINE_INTERNAL=1"]
```

`apps/engine-cli/cova.toml`:

```toml
[package]
name = "engine_cli"
version = "0.1.0"
edition = "c++20"

[build]
type = "executable"
sources = ["src/**/*.cpp"]
link-libraries = ["z"]          # PRIVATE z from engine_core: repeat it here

[dependencies]
engine_core = { path = "../../libs/engine-core" }
```

Notes:

- `Threads::Threads` is only partly automatic: Cova adds `-pthread` to
  executable and shared-library *artifact* links, but not to compiles or test
  links. Add `compile-options = ["-pthread"]` where the source depends on the
  compile-side macro, and `link-options = ["-pthread"]` when a test needs it.
- `z` was `PRIVATE` to `engine_core`; because link libraries do not propagate,
  the executable declares it.
- `engine_core` becomes `libengine_core.a`; the executable links the archive by
  path, so no `-L`/`-l` is needed for the dependency itself.
- The sources moved into per-package directories. Keeping the old layout is
  possible only with the non-glob `sources = ["../.."]` escape described above,
  or by making the package root the directory that already holds the sources.

---

## 7. Verification checklist

Run these from the package or workspace root:

```bash
cova metadata --json              # resolved target, profile, sources, toolchain
cova build --dry-run              # plan without executing
cova check                        # compile every TU, no link
cova build                        # full build
cova run                          # primary executable, if any
cova test                         # every declared test binary
cova fmt --check                  # formatting gate
cova doctor                       # toolchain, sysroot, formatter, paths
```

Then compare against the old build:

1. **Compile flags.** Cova writes `compile_commands.json` at the package or
   workspace root. Diff the `-I`, `-D`, `-std=`, `-O`, `-g`, warning and
   sanitizer flags against CMake's database. Every difference should be
   intentional.
2. **Link inputs.** Cova does not print the link command, so compare behaviour:
   `ldd`/`readelf -d` on Linux, `otool -L` on macOS, `objdump -p` on Windows,
   plus the project's own runtime tests.
3. **Outputs.** Check `target/<triple>/<profile>/bin|lib` for the expected
   artifact names and suffixes.
4. **Tests.** The same tests must pass, including tests that exercise the
   dependency graph rather than individual functions.
5. **Cross builds.** Build every target you ship, and inspect the artifact
   architecture (Cova verifies it after linking).
6. **Clean build.** Delete `target/` and build again; incremental correctness
   must not hide a missing declaration.

CI migration checklist:

- replace configure/build/test steps with `cova build`, `cova test`;
- keep `cova fmt --check` as a gate if the project already used clang-format;
- commit `cova.lock`; `cova build` writes it and `--locked` refuses a stale or
  missing file. `--locked` also requires `cova-toolchain.lock`, so both files
  must be present;
- `cova-toolchain.lock` records the **system** toolchain identity. Commit it and
  use `--locked` only when CI has the same compiler/sysroot identity; on
  heterogeneous CI, run without `--locked` and let Cova resolve per machine;
- a regular build skips `dev-dependencies`, so test-only path packages do not
  slow production builds.

---

## 8. Meson, Make/Autotools and Bazel

### Meson

| Meson | Cova |
|---|---|
| `project('x', 'cpp', default_options: ['cpp_std=c++20'])` | `[package] edition = "c++20"` |
| `executable()` / `library()` / `shared_library()` | `[build] type` |
| `sources` | `[build] sources` |
| `include_directories` | `public-includes` / `private-includes` |
| `c_args` / `cpp_args` | `compile-options` (no per-language split) |
| `link_args` | `link-options` |
| `link_with` | Path dependency |
| `dependencies: dependency('zlib')` | `link-libraries = ["z"]` |
| `declare_dependency` | A package with `public-includes` / `public-definitions` |
| `subproject()` | Path dependency |
| `buildtype: debug/release/debugoptimized/minsize` | Profiles |
| `cross_file` | `--target` + `[targets.<triple>]` + `[toolchain]` |
| `custom_target()` / generators | Not supported |
| `test()` | `[test]` sources |
| `install_subdir()` / `install_headers()` | Not supported |
| `pkgconfig` module | Transcribe flags by hand |

### Make / Autotools

- `./configure` and generated `config.h` have no equivalent. Run the old
  configure step, commit the generated file (or generate it outside the Cova
  build), and point `private-includes` at it.
- Compiler variables (`CC`, `CXX`, `CFLAGS`, `LDFLAGS`, `LIBS`) map to
  `[toolchain]`, `compile-options`, `link-options` and `link-libraries`.
  `pkg-config --cflags/--libs` output must be transcribed; the manifest has no
  shell expansion.
- `VPATH` builds become source globs, subject to the no-`..` glob rule.
- `make install`, `make uninstall`, `make dist` have no equivalent.
- `make check` maps to `cova test`.
- `make -j` maps to `cova build -j`.

### Bazel

- One Cova package per `cc_binary`, `cc_library` or `cc_test` target.
- `srcs`, `hdrs`, `copts`, `linkopts`, `defines`, `local_defines`,
  `includes` map to the corresponding `[build]` keys.
- `deps` on another package in the repository → path dependency.
- `select()` → `[targets.<triple>]` overrides or target-conditioned
  dependencies; there is no general conditional expression.
- `genrule` / `cc_proto_library` → not supported; generate before the build.
- `data` / runfiles → not supported.
- `cc_test` → `[test] sources`; the test binary is the source stem.
- Bazel's sandbox is stricter than Cova's package root; check for accidental
  reads outside the package.

---

## 9. Agent-assisted migration

The mapping is mechanical enough that an agent can do the inventory and the
first pass, provided it is constrained: an agent that invents a TOML key or
"simulates" a custom command produces a build that looks migrated and is not.

### Hand the agent the guide

This document is the specification. The whole brief can be one line:

```text
Migrate this project to Cova. Refer to https://raw.githubusercontent.com/viloris-org/cova/refs/heads/main/docs/MIGRATING.md for migration to Cova.
```

Use the raw URL rather than a repository page so the agent reads the current
text. If you want the boundaries stated explicitly, add:

```text
- Work on a branch and leave the old build files untouched.
- Use only manifest keys documented in docs/MIGRATING.md; do not invent keys.
- Stop and report any feature the guide says cannot be expressed. Do not
  simulate it with a custom command, a checked-in binary or a stub.
- Run the verification checklist and compare the new compile_commands.json with
  the old one.
- Report the mapping per target, every deviation, and anything that still needs
  the old build.
```

Give the agent a clean branch, permission to run the old build and `cova`, and
the old build files plus `compile_commands.json`. The build commands are how it
verifies itself; the guide's §4 explains how to produce the inventory.

### Definition of done

- `cova check`, `cova build` and `cova test` pass from a clean `target/`.
- The old build still passes on the same branch, so the comparison is honest.
- `compile_commands.json` differences are enumerated and intentional.
- The report names every unsupported feature; none is silently dropped.
- The diff contains manifests, workspace scaffolding and at most test entry
  points — no bulk source rewrites.

### Human review

Review the manifest diff, not just the passing build. A missing
`private-includes`, a dropped `-D`, or a dependency's system library left
unlinked can still compile and produce a subtly different binary. The
before/after flag comparison is the cheapest place to catch it.

---

## 10. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `no source files matched the manifest` | Wrong glob, wrong package root, or only unsupported extensions | Check `cova metadata --json`; globs are relative to the package root; `.s`/`.S`/`.asm` and unknown extensions are skipped |
| `undefined reference` when linking an executable | A dependency's system library was `PRIVATE` in the old build; link libraries do not propagate | Add the library to the executable's `link-libraries` |
| Duplicate `main` in tests | The package's entry point is not named `main.cpp`, so its object is linked into tests | Set `[build] entry-point = "path/to/entry.cpp"` |
| A test binary is missing or overwritten | Two test sources share a stem | Rename one source; binaries are named after the stem |
| `-framework Cocoa` becomes `-lCocoa` | Multi-token flags in `link-libraries` | Put them in `link-options` |
| Headers from a dependency are not found | Only `public-includes` propagate; private include dirs do not | Move the directory to `public-includes` or add it to the consumer |
| Link fails with `recompile with -fPIC` | Shared-library packages do not add `-fPIC` to compiles in this milestone | Add `compile-options = ["-fPIC"]` to the `shared-library` package |
| `cova run` works in the build tree but not after copying | A shared-library dependency was linked by absolute path | Prefer static libraries, or set SONAME/rpath with `link-options` |
| `--locked` fails on CI | `cova-toolchain.lock` pins a different system compiler/sysroot | Use the same toolchain identity, or run without `--locked` |
| C++ Modules fail to build | Module support is not implemented | Do not migrate the module build yet |
| GNU C extensions are rejected | C sources are forced to `-std=c17` | Use `-std=gnu17` in `compile-options` for C-only packages, or adjust the code |
| `cova test` says `no tests to run` | The `[test]` section is absent; Cova does not discover tests implicitly | Add `[test] sources = ["tests/**/*.cpp"]` |
| `cova fmt` skips tests | Default formatter scope is the build source set and `[format] include` replaces it when set | Set `[format] include` to the full list, including `tests/**/*.cpp` |
| `compile_commands.json` was replaced | Cova writes it at the project root | Export CMake's database into `build/`, not the root |
| A target override does not apply | The key is not a canonical triple | Use the canonical spelling from `cova target list`; the parser canonicalises known aliases |
| A Windows project only builds with MSVC | MSVC is not a supported Cova host toolchain | Port to MinGW-w64 GCC and rebuild dependencies for that ABI, or keep the MSVC build |

---

## 11. Quick manifest reference

A single executable package:

```toml
[package]
name = "app"                    # also names the artifact
version = "0.1.0"
edition = "c++20"               # C++ standard; C is always -std=c17

[build]
type = "executable"             # executable | static-library | shared-library
                                # shared-library also needs compile-options = ["-fPIC"]
sources = ["src/**/*.cpp"]      # globs relative to this package
exclude = ["src/win32/**"]      # globs removed from the source set
public-includes = ["include"]   # also exposed to dependents
private-includes = ["src"]
public-definitions = ["APP_PUBLIC=1"]
private-definitions = ["APP_INTERNAL=1"]
compile-options = ["-Wall", "-Wextra", "-Wpedantic"]
link-options = ["-Wl,--gc-sections"]
link-libraries = ["z"]          # -lz; paths and leading-'-' entries pass through
entry-point = "src/main.cpp"    # optional; excludes it from test links
target = "x86_64-unknown-linux-gnu"  # optional default target

[profile.dev]
optimization = 0
debug = true
assertions = true
extra-cxxflags = []
extra-ldflags = []

[profile.release]
optimization = 3
debug = false
lto = true
assertions = false

[toolchain]
policy = "system-first"        # system-first | managed-first | system-only | managed-only
compiler = ""                   # "", "gcc", "clang"
version = ""                    # e.g. ">=14"
auto-install = false
preferred = ""                  # a pinned toolchain id

[format]
tool = "clang-format"
style = "file"
include = []                    # when non-empty, replaces the default file set
exclude = []
fallback-style = "LLVM"

[test]
sources = ["tests/**/*.cpp"]
exclude = []
# `runner` and `runner-command` are parsed but not applied in this milestone;
# use `cova test --runner <native|qemu|wine>` or ~/.cova/config.toml.

[dependencies]
math = { path = "../math" }
win = { path = "platform/win", target = ["windows"] }

[dev-dependencies]
fixtures = { path = "../fixtures" }

[targets.aarch64-unknown-linux-musl]
definitions = ["ARCH_ARM=1"]
compile-options = ["-march=armv8-a"]
link-options = ["-static"]
```

A workspace root needs only:

```toml
[workspace]
members = ["libs/*", "apps/*"]
exclude = ["libs/experimental"]

[workspace.toolchain]
policy = "system-first"
auto-install = false
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for module boundaries and
[MILESTONES.md](MILESTONES.md) for the capabilities delivered by this tree.
