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
| Compilation database | `compile_commands_json` in `src/build/planner` | `e2e_cli_test` |

## Milestone 2 — Cross-compilation foundation (delivered)

| Requirement | Implementation | Verification |
|---|---|---|
| Structured targets, ABI signatures and constraints | `src/platform/{triple,abi,constraints}` | `platform_triple_test` |
| Cross compiler discovery | Prefixed/versioned drivers (`aarch64-linux-musl-g++`, `g++-14`) in `src/toolchain/discovery` | `toolchain_resolver_test` |
| System sysroot discovery | `/opt/<triple>/sysroot`, `/usr/local/<triple>/sysroot`, `$COVA_SYSROOT` in `src/toolchain/resolver` | `cross_clang_uses_a_system_sysroot` |
| System-first resolver and mixed sources | `src/toolchain/resolver`; system compiler + system/managed sysroot | `toolchain_resolver_test`, `cova explain` |
| Self-contained cross GCC | Prefixed GCC is not forced through a redundant Cova sysroot | `self_contained_cross_gcc_needs_no_sysroot` |
| Trusted local component registry | `src/toolchain/installer`, `SysrootManager::install`, `docs/REGISTRY.md` | `toolchain_installer_test`, `sysroot_manager_test` |
| Managed sysroot installation | Archive **or** trusted registry index; SHA-256/size verification; safe extraction; atomic commit | `registry_installs_verified_archive` |
| Managed toolchain installation | Archive verified against the index, compiler banner re-checked, atomic commit | `installs_a_verified_registry_archive` |
| `--offline` / `--no-install` | Refused before an archive is read; distinct diagnostics | `offline_cross_without_sysroot_names_the_restriction`, installer tests |
| Artifact architecture verification | ELF, PE/COFF, Mach-O and WebAssembly inspection in `src/platform/binary` | `platform_binary_test`, `cross_build_rejects_a_wrong_architecture_artifact`; every successful build |
| Target-specific output | `target/<triple>/<profile>` from the planner | `e2e_cli_test`, `build_planner_test` |

No network transport is implemented. A registry archive referenced by an
`http(s)://` URL is rejected; operators mirror archives into a local registry
and record their SHA-256. A host without a compatible system cross compiler,
system sysroot or installed managed component still fails with the
missing-sysroot/target diagnostic rather than producing the wrong binary.

## Milestone 3 — Package management (local path dependencies delivered)

`cova add` / `remove` edit path and version declarations. Local path dependencies
are resolved transitively by `build`, `check`, `run`, `test` and `fetch`, inside
or outside a workspace: public includes and definitions propagate transitively,
private settings stay private, dependency cycles are rejected with the offending
chain, and every package is rebuilt for the requested `--target`. Their artifacts
are linked into the dependent.

Dependency resolution writes `cova.lock`. `--locked` fails when the lockfile is
missing, stale or would change, and never rewrites it; `--frozen` implies
`--offline --locked` (PRD §15.2). Unit tests: `package_dependency_test`; end-to-end
coverage: `path_dependency_outside_a_workspace_builds_and_links`,
`lockfile_is_written_and_enforced`.

Remote dependency management (registry and git sources), `cova update` and
vendoring of remote dependencies remain unimplemented and fail explicitly because
there is no trusted registry or verified source fetcher (PRD §18.1, OD-003). A
regular build skips remote development dependencies, which it does not need.

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
