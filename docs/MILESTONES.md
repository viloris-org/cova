# Cova — Milestones and Verification

This document records what the tree currently delivers, how it is verified, and
what is deliberately not implemented yet. It is the companion to
[PRD.md](../PRD.md) and [ARCHITECTURE.md](ARCHITECTURE.md).

## Milestone 0 — Architecture foundation (delivered)

| Requirement | Implementation | Verification |
|---|---|---|
| Target triples and constraints | `src/platform/triple`, `src/platform/abi`, `src/platform/constraints` | `platform_triple_test` |
| Toolchain descriptor | `src/toolchain/descriptor.*` | `toolchain_resolver_test` |
| System toolchain discovery | `src/toolchain/discovery` | `toolchain_resolver_test`, `cova toolchain list` |
| Capability probing | `src/toolchain/probe` (real compile, archive and link probes) | `cova doctor`, resolver probes |
| Structured diagnostics | `src/core/diagnostics` + resolver failure builder | `cova build --target aarch64-unknown-linux-musl` on a host without a sysroot |

The resolver is usable without a project build (`cova explain`, `cova doctor`).

## Milestone 1 — Build MVP (delivered)

| Requirement | Implementation | Verification |
|---|---|---|
| `cova new`, `cova init` | `src/cli/cmd_new.cpp` | `e2e_cli_test` |
| Manifest parse/serialise | `src/manifest` | `manifest_parse_test`, `e2e_cli_test` |
| Build graph, planner, scheduler, executor | `src/build/**` | `build_planner_test`, `e2e_cli_test` |
| Incremental builds | depfile header tracking + content-hash state | `incremental_build_rebuilds_nothing_when_unchanged` |
| Artifact cache | `src/build/cache` | `build_planner_test`, `cova cache gc` |
| `cova build`, `run`, `test`, `check`, `clean` | `src/cli/cmd_build.cpp` | `e2e_cli_test` |
| `cova fmt` | `src/formatter` | `e2e_cli_test`, `formatter_test` |
| Diagnostics in the requested stream | `src/cli/common.cpp` | `e2e_cli_test` |

## Milestone 2 — Cross-compilation (interfaces + honest partial path)

- Targets, ABI signatures, constraints and mixed-source descriptors exist and
  are tested.
- The resolver is system-first and refuses incompatible candidates with the
  PRD §7.7 diagnostic shape.
- A sysroot can be installed from a **local archive** (or a local registry
  cache directory) with traversal-checked extraction and structural
  verification. There is no public sysroot registry in this build.
- Managed toolchain download and managed sysroot download are **not**
  implemented. Commands report that rather than simulating a download.
- Cross builds therefore succeed only when a compatible system sysroot is
  already available; otherwise resolution fails with the missing-sysroot
  diagnostic.

## Milestone 3 — Package management (command surface only)

`cova add` / `remove` edit path and version declarations. `tree` and `vendor`
are local-only. `fetch`, `update`, remote `add` and `vendor` of remote
dependencies fail explicitly because there is no trusted registry or verified
source fetcher (PRD §18.1, OD-003). Workspace path dependencies are consumed by
the multi-package planner (include interface + archive linking); standalone path
dependencies outside a workspace are declared but not yet consumed.

## Milestone 4 — Workspace and self-hosting (workspace delivered)

Workspace discovery, member ordering and multi-package planning work
(`workspace_test`, `cova workspace`). The `bootstrap/` script builds Cova with
only a C++ compiler, Ninja and Python (Stage 0). A `cova.toml` at the repository
root allows the built binary to build the project itself (Stage 1); the bootstrap
script remains the authoritative clean-room path.

## Milestone 5 — Ecosystem expansion (not started)

Windows/MSVC, WASI, Android NDK, CMake adaptation, remote execution/ caching,
C++ modules and registry publishing are not implemented. Their PRD sections are
planning material, not current promises.

## Verification commands

```bash
./bootstrap/build.sh --test          # clean-room build + full suite
./bootstrap/build.sh                 # build ./build/bootstrap/bin/cova
python3 scripts/devbuild.py test     # incremental developer build + tests
python3 scripts/devbuild.py test-module build
```

The test suite is dependency-free and lives under `tests/unit` and
`tests/integration`. Network-dependent registries are never contacted.
