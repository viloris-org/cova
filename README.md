# Cova

**One project. One workflow. Multiple platforms.**

Cova is a cross-compilation-first build system, package manager and toolchain
manager for C/C++, written in C++26. It gives C/C++ projects the kind of
workflow that Rust developers get from Cargo — without pretending C/C++ has the
same ABI and linking model.

```bash
cova new hello
cd hello

cova build
cova run
cova test
cova fmt
cova clean
```

Local path dependencies are resolved, built and linked, including when they live
outside the workspace:

```bash
cova add math --path ../math
cova build
```

Remote dependency management is **not implemented yet**: `cova add fmt` without a
`--path`, and builds or tests that need a registry or git dependency, fail
explicitly instead of pretending to fetch them. (A regular build skips remote
development dependencies, because it does not need them.)

Cross compilation is a first-class command, not a configuration project:

```bash
cova build --target aarch64-unknown-linux-musl
```

Windows GNU targets use the same model. With a MinGW-w64 cross toolchain
installed, either spelling works:

```bash
cova build --target x86_64-pc-windows-gnu
cova build --target x86_64-w64-mingw32   # MinGW's own triple
```

Cova discovers prefixed drivers such as `x86_64-w64-mingw32-g++`, links a PE
executable and verifies its architecture after linking.

## Status

This repository implements **Milestone 0 (architecture foundation)**,
**Milestone 1 (build MVP)** and the **Milestone 2 cross-compilation foundation**
of the product requirements. See [PRD.md](PRD.md) for the full product
definition and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the engineering
contract, [docs/MILESTONES.md](docs/MILESTONES.md) for what is delivered and how
it is verified, [docs/TOOLCHAIN-RESOLUTION.md](docs/TOOLCHAIN-RESOLUTION.md) for
the resolution chain and [docs/REGISTRY.md](docs/REGISTRY.md) for the trusted
local component registry.

What works today:

| Area | State |
|---|---|
| Project creation and init (`new`, `init`) | Works, including layout detection and non-destructive init |
| Manifest (`cova.toml`) | Full parse/validate/serialise with round-trip |
| Target model (triples, ABI, constraints) | Structured, normalising, compiler-independent |
| Toolchain discovery and probing | Real discovery + real compile/link probes, including prefixed cross drivers |
| Toolchain resolution | System-first → managed fallback, system and managed sysroots, structured diagnostics |
| Build graph, planner, scheduler | Independent IR; compiler providers emit commands |
| Incremental builds | Depfile-driven header tracking, content-hash cache keys |
| `build`, `run`, `test`, `check`, `clean`, `fmt` | Works end to end |
| Artifact verification | ELF, PE/COFF, Mach-O and WebAssembly architecture checks after a build |
| `doctor`, `target list`, `toolchain list/explain`, `sysroot list/verify` | Works |
| Sysroot installation | Local archive or trusted local registry index; SHA-256 verified |
| Managed toolchain installation | Trusted local registry index; SHA-256 verified, atomic commit |
| Dependency manager (`add`, `fetch`, `update`, `vendor`) | Local path dependencies, inside or outside a workspace, are resolved transitively, built for the requested target and linked; `cova.lock` records the resolution. Remote dependency management (registry and git sources) is not implemented yet and fails honestly |
| Dependency lockfile | `cova.lock` is written on resolution; `--locked` refuses a missing, stale or would-change lockfile and `--frozen` implies `--locked --offline` |
| Compilation database | Every `build` and `check` emits `compile_commands.json` with the exact compile arguments; a workspace gets one file covering its members |
| Workspace | Discovery, ordering, shared settings and multi-package builds |
| Self-hosting | A root `cova.toml` lets a built Cova build, test and run itself (Stage 1) |
| Host platforms | Linux and macOS x86_64/aarch64, and Windows x86_64 with MinGW-w64 GCC (MSVC is not supported yet) |

What deliberately does **not** work yet, by design:

- **No network package registry or source fetcher.** Remote `cova add`, `cova
  fetch` and `cova update` refuse rather than simulate a download. PRD §18.1
  forbids trusting content we cannot verify.
- **No network component download.** `cova toolchain install` and `cova sysroot
  install` read a trusted local registry (`~/.cova/registry/index.toml` or
  `--registry <dir>`); an `http(s)://` archive URL is reported as unsupported.
- **No C++ Modules.** The capability probe reports modules unsupported and the
  planner does not emit scans rather than pretending.
- **No remote execution.** `--runner remote` is rejected explicitly.

## Installing a binary

Every published build is a beta. One line, Linux or macOS:

```bash
curl -fsSL https://raw.githubusercontent.com/viloris-org/cova/main/install.sh | sh
```

The script resolves the latest beta from GitHub Releases, verifies the download
against the release's `SHA256SUMS`, and installs to `/usr/local/bin` (or
`~/.local/bin` when that is not writable). `COVA_VERSION` pins a specific
version; `COVA_BIN_DIR` overrides the destination.

Native packages are attached to each release:

| Platform | Assets |
|---|---|
| Linux x86_64 / aarch64 | `.deb`, `.rpm`, pacman `.pkg.tar.zst`, `.tar.gz` |
| macOS Intel + Apple Silicon | universal `.pkg`, `.dmg`, `.tar.gz` |
| Windows x86_64 | `.zip` with `cova.exe` |

The Linux binaries are statically linked, so one package works across
distributions. The Windows archive is built with MSYS2's UCRT64 GCC and links
the MinGW runtime statically: unzip it and add the directory to `PATH`. It uses
the system UCRT (`ucrtbase.dll`), which ships with Windows 10 and later. Cova
builds Windows projects with a MinGW-w64 GCC; MSVC as a host toolchain is not
supported yet (PRD §20.2).

### Releasing

Pushing a `v*` tag runs [.github/workflows/release.yml](.github/workflows/release.yml),
which builds each target on a native runner, packages it, and publishes a
pre-release with the checksums. See [docs/RELEASING.md](docs/RELEASING.md).

## Building from source

Cova has no third-party dependencies. Building from source requires a
C++26-capable compiler (Clang ≥ 17 or GCC ≥ 14), Ninja and Python 3.

```bash
./bootstrap/build.sh              # build ./build/bootstrap/bin/cova
./bootstrap/build.sh --test       # build and run the full test suite
COVA_CXX=clang++ ./bootstrap/build.sh
```

The bootstrap script probes the compiler for the C++26 baseline features Cova
actually uses (`<expected>`, `<print>`, concepts, ranges, designated
initializers) — it does not infer support from a version number (PRD §16.4).

On Windows the bootstrap shell script needs MSYS2. A native build works without
it as long as a MinGW-w64 GCC, Ninja and Python 3 are on `PATH`:

```powershell
python scripts/devbuild.py build
python scripts/devbuild.py test
```

`scripts/devbuild.py` handles Windows command lines and `.exe`/`.a` naming for
MinGW; MSVC (`cl.exe`) is not a supported host toolchain yet.

Cova can also build itself once a Stage 0 binary exists (PRD §16.2):

```bash
./bootstrap/build.sh
export PATH="$PWD/build/bootstrap/bin:$PATH"
cova build          # Stage 1: the repository root is a Cova package
cova test
```

Day-to-day development uses the same driver directly:

```bash
python3 scripts/devbuild.py list                 # modules, sources, tests
python3 scripts/devbuild.py module platform      # build one module
python3 scripts/devbuild.py test-module platform # build + run its tests
python3 scripts/devbuild.py build                # link bin/cova
python3 scripts/devbuild.py test                 # build + run everything
python3 scripts/devbuild.py clean
```

Set `COVA_BUILD_DIR=build/<name>` to keep several build trees side by side.

## Using Cova

```bash
# Create and run a project
cova new hello && cd hello
cova build
cova run -- --name world

# Libraries and workspaces
cova new math --lib
cova new engine --workspace

# Cross compilation
cova target list
cova build --target aarch64-unknown-linux-musl

# Inspect what Cova decided and why
cova toolchain list
cova toolchain explain --target aarch64-unknown-linux-musl
cova graph
cova explain
cova doctor
```

Builds are never silent: resolution stages and per-action progress are printed
to stderr, and a terminal gets an animated spinner plus a progress bar
(`━━━━━━────`) carrying the task count, a status label such as
`compiling`/`linking`, elapsed time and the current target. A long compile
therefore reads as "still working", not "hung". `--quiet` silences the status
output and `--json` keeps stdout machine-readable.

### Managed components from a trusted local registry

Cova installs only what it can verify. A registry is a directory containing an
`index.toml` and the archives it names; every archive must carry a SHA-256 in the
index. See [docs/REGISTRY.md](docs/REGISTRY.md) for the schema.

```bash
cova toolchain install aarch64-linux-musl-gcc --registry /srv/cova-registry
cova sysroot install --target aarch64-unknown-linux-musl --registry /srv/cova-registry

# The same restrictions apply to component installation:
cova toolchain install aarch64-linux-musl-gcc --offline   # refuses
cova sysroot install --target aarch64-unknown-linux-musl --no-install  # refuses
```

Archives can be installed directly when you already have one:

```bash
cova sysroot install --target aarch64-unknown-linux-musl --from ./sysroot.tar.gz
```

After a build, Cova inspects each primary binary and fails if its architecture
does not match `--target` (ELF, PE/COFF, Mach-O and WebAssembly are recognised).

### Global options

Every command honours the same set (PRD §4.2):

| Option | Meaning |
|---|---|
| `--target <triple>` | Build for a target platform |
| `--profile <name>` | `dev` (default) or `release` |
| `--compiler <family[@ver]>` | Compiler family with optional version requirement |
| `--toolchain <id>` | Pin a specific toolchain identity |
| `--offline` | Forbid network access |
| `--locked` | Fail instead of rewriting `cova.lock` or `cova-toolchain.lock` |
| `--frozen` | `--offline --locked` |
| `--no-install` | Never install new components |
| `-j, --jobs <n>` | Parallel jobs |
| `-v` / `-q` | Verbose / quiet |
| `--color <auto\|always\|never>` | Colour policy |
| `--json` | Machine-readable output where supported |

## Migrating an existing project

Cova does not import CMake, Meson, Make or Bazel files, and this milestone does
not add a compatibility layer. `cova init` detects those files, reports them and
leaves them untouched, so an existing project can be migrated incrementally
while the old build stays runnable.

The simplest way is to hand your agent the migration guide. This works in
Claude Code, Codex, or any agent that can read a URL and run shell commands:

```text
Migrate this project to Cova. Refer to https://raw.githubusercontent.com/viloris-org/cova/refs/heads/main/docs/MIGRATING.md for migration to Cova.
```

The guide maps the common constructs of CMake, Meson, Make and Bazel to
`cova.toml`, states what cannot be expressed yet, and gives a verification
checklist. It is also available as [docs/MIGRATING.md](docs/MIGRATING.md).

## Architecture at a glance

```
core       Result/Error, diagnostics, fs, process, SHA-256, SemVer, TOML, JSON, config
platform   Target triples, ABI signatures, constraint evaluation, binary inspection
toolchain  Discovery, capability probes, system-first resolver, managed installer
sysroot    Sysroot manifests, verification, safe extraction, trusted registry staging
manifest   cova.toml model, validation, serialisation
workspace  Member discovery, ordering, shared settings
build      Build Graph IR, compiler providers, planner, scheduler, executor, cache
formatter  clang-format resolution and file selection
package    Lockfiles (dependency resolution is Milestone 3)
cli        Argument parsing, command orchestration, user interaction
```

Dependencies point downward only. The build graph never hard-codes a compiler
command line — compiler providers do that. The CLI never contains build logic.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Design rules

1. **Cross-compilation first.** Target modelling is not an add-on.
2. **System first, managed fallback.** Use a compatible system toolchain; only
   install what is genuinely missing.
3. **Non-invasive.** Cova never modifies system toolchains, SDKs or `PATH`, and
   never needs root.
4. **Transparency.** Every automatic choice and download is explainable.
5. **Honesty.** A capability that is not implemented reports precisely that. No
   command claims success it did not verify.
6. **Correctness over cache hits.** Wrong cache reuse is a correctness bug, so
   keys cover full inputs, flags, toolchain, sysroot, target and ABI.

## Documentation

- [PRD.md](PRD.md) — product requirements
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module boundaries, conventions, testing rules
- [docs/MILESTONES.md](docs/MILESTONES.md) — what is delivered per milestone and how it is verified
- [docs/TOOLCHAIN-RESOLUTION.md](docs/TOOLCHAIN-RESOLUTION.md) — the resolution chain in detail
- [docs/REGISTRY.md](docs/REGISTRY.md) — trusted local component registry schema and install rules
- [docs/RELEASING.md](docs/RELEASING.md) — cutting a release, package formats and signing
- [docs/MIGRATING.md](docs/MIGRATING.md) — moving an existing CMake, Meson, Make or Bazel project to Cova

## License

Licensed under the [Apache License, Version 2.0](LICENSE). Attribution notices
are in [NOTICE](NOTICE).

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in this project shall be licensed as above, without any additional
terms or conditions (Apache-2.0 §5).
