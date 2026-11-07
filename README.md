# Cova

**One project. One workflow. Multiple platforms.**

Cova is a cross-compilation-first build system, package manager and toolchain
manager for C/C++, written in C++26. It gives C/C++ projects the kind of
workflow that Rust developers get from Cargo — without pretending C/C++ has the
same ABI and linking model.

```bash
cova new hello
cd hello

cova add fmt
cova build
cova run
cova test
cova fmt
cova clean
```

Cross compilation is a first-class command, not a configuration project:

```bash
cova build --target aarch64-unknown-linux-musl
```

## Status

This repository implements **Milestone 0 (architecture foundation)** and
**Milestone 1 (build MVP)** of the product requirements, with Milestone 2
interfaces in place. See [PRD.com](PRD.com) for the full product definition and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the engineering contract.

What works today:

| Area | State |
|---|---|
| Project creation and init (`new`, `init`) | Works, including layout detection and non-destructive init |
| Manifest (`cova.toml`) | Full parse/validate/serialise with round-trip |
| Target model (triples, ABI, constraints) | Structured, normalising, compiler-independent |
| Toolchain discovery and probing | Real discovery + real compile/link probes |
| Toolchain resolution | System-first → managed → trusted download chain, with diagnostics |
| Build graph, planner, scheduler | Independent IR; compiler providers emit commands |
| Incremental builds | Depfile-driven header tracking, content-hash cache keys |
| `build`, `run`, `test`, `check`, `clean`, `fmt` | Works end to end |
| `doctor`, `target list`, `toolchain list/explain`, `sysroot list/verify` | Works |
| Sysroot installation | Local archive only; no public registry yet (fails honestly) |
| Dependency manager (`add`, `fetch`, `update`, `vendor`) | Manifest edits only; no fetcher yet (fails honestly) |
| Workspace | Discovery, ordering and shared settings; multi-package builds wired |

What deliberately does **not** work yet, by design:

- **No package registry and no source fetcher.** `cova add`/`fetch`/`update`
  refuse rather than simulate a download. PRD §18.1 forbids trusting content we
  cannot verify.
- **No managed toolchain download.** `cova toolchain install` explains that no
  trusted registry is configured.
- **No C++ Modules.** `scan_deps_argv` returns "not supported" rather than
  pretending.
- **No remote execution.** `--runner remote` is rejected explicitly.

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

### Global options

Every command honours the same set (PRD §4.2):

| Option | Meaning |
|---|---|
| `--target <triple>` | Build for a target platform |
| `--profile <name>` | `dev` (default) or `release` |
| `--compiler <family[@ver]>` | Compiler family with optional version requirement |
| `--toolchain <id>` | Pin a specific toolchain identity |
| `--offline` | Forbid network access |
| `--locked` | Refuse to change the lockfile |
| `--frozen` | `--offline --locked` |
| `--no-install` | Never install new components |
| `-j, --jobs <n>` | Parallel jobs |
| `-v` / `-q` | Verbose / quiet |
| `--color <auto\|always\|never>` | Colour policy |
| `--json` | Machine-readable output where supported |

## Architecture at a glance

```
core       Result/Error, diagnostics, fs, process, SHA-256, SemVer, TOML, JSON, config
platform   Target triples, ABI signatures, constraint evaluation
toolchain  Discovery, capability probes, system-first resolver
sysroot    Sysroot manifests, verification, safe archive extraction
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

- [PRD.com](PRD.com) — product requirements
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module boundaries, conventions, testing rules
- [docs/MILESTONES.md](docs/MILESTONES.md) — what is delivered per milestone and how it is verified
- [docs/TOOLCHAIN-RESOLUTION.md](docs/TOOLCHAIN-RESOLUTION.md) — the resolution chain in detail

## License

Not yet chosen. The PRD does not fix a licence; pick one before publishing.
