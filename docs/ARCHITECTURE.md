# Cova — Architecture Contract

> This document is the binding contract for everyone working in this tree.
> Read it before writing code. If a change to this contract is needed, ask the
> Lead first.

Cova is a C/C++ build system, package manager and cross-compilation toolchain
manager, written in C++26 and inspired by Cargo. The PRD is `PRD.md` at the
repo root (the original Chinese text is `PRD.com`); `§n.n` below refers to its
sections.

**Current delivery target: Milestone 0 (architecture foundation) + Milestone 1
(build MVP), with Milestone 2 interfaces in place and honest diagnostics.**

---

## 1. Non-negotiable rules

1. **Headers are the contract.** The headers listed in §4 are frozen. A module
   owner may edit *only* headers inside their own directory, and only additively
   (new declarations, never changed/removed ones) — unless the Lead approves.
2. **No third-party dependencies.** Everything Cova needs (TOML, SHA-256, JSON,
   test framework, process spawning) is implemented in-tree. `scripts/devbuild.py`
   compiles with the system C++ compiler and Ninja only.
3. **Write scope.** Each agent owns the directories in §4 and must not create or
   modify files outside them. Tests for your module go in `tests/unit/<module>_*.cpp`.
4. **C++26, but bootstrap-honest.** Use `-std=c++26` freely for language features
   (`std::expected`, concepts, `std::print`, ranges, designated initializers).
   Do not assume a feature that the bootstrap compiler does not accept — verify
   with `python3 scripts/devbuild.py module <yours>` before reporting done.
5. **Every fallible operation returns `cova::Result<T>` / `cova::Status`**
   (see `src/core/common.hpp`). No exceptions escape module boundaries; no
   `std::abort` for user errors. Exceptions are only acceptable internally to a
   TU (e.g. `std::bad_alloc` handling) and must be caught before returning.
6. **Errors are data.** Every `Error` has a stable `code` of the form
   `cova::<area>::<kind>` (e.g. `cova::toolchain::unresolved`), a one-line
   message, and context lines. Resolver-level failures must additionally be
   expressible as a `Diagnostic` per PRD §7.7.
7. **Determinism.** Anything that reaches a lockfile, a cache key or a rendered
   report must be deterministic: sort containers before serialising, never
   iterate a `std::unordered_map` for output, never embed timestamps.
8. **Security invariants are code, not comments.** No path may be deleted outside
   its allowed root (`fs::remove_tree` / `fs::safe_remove`). No archive may be
   extracted with traversal escape (`sysroot::extract_archive_safely`). No
   download is trusted without a hash check. No action may modify system
   toolchains or pull in a root requirement.

## 2. Layering

Dependencies point **downwards only**. `core` knows nothing; `cli` knows everything.

```
core  (common, diagnostics, filesystem, process, hashing, version, json, toml, config)
 ├── testing                    (in-tree test framework; depends on core)
 ├── platform                   (triple, abi, constraints)
 │    ├── toolchain             (descriptor, discovery, probe, resolver)
 │    │    └── sysroot          (manager, store, verifier)
 │    ├── manifest               (cova.toml model)
 │    │    └── workspace         (multi-package discovery)
 │    ├── build                 (graph IR, planner, scheduler, executor, cache)
 │    ├── formatter             (clang-format integration)
 │    └── package               (resolver/fetcher/lockfile — M3 scaffolding)
 └── cli                        (arg parsing, command wiring, user interaction)
```

Enforced boundaries (PRD §17.2):
* `platform` must not include anything from `cli`/`build`.
* `build` must not include `cli`, and must not spawn `clang-format` (that is
  `formatter`). `build` *does* spawn compilers — via `build::action` providers.
* `toolchain` must not include `build` (the resolver never builds anything).
* Only `cli` reads `argv`, decides exit codes, and prints user-facing text.

## 3. Module ownership

| Module | Owner | Directories | Tests |
|---|---|---|---|
| core + cli + integration | Lead | `src/core/**`, `src/cli/**`, `src/main.cpp`, `tests/integration/**` | `tests/unit/core_*.cpp`, `tests/integration/*.cpp` |
| platform | platform-dev | `src/platform/**` | `tests/unit/platform_*.cpp` |
| toolchain | toolchain-dev | `src/toolchain/**` | `tests/unit/toolchain_*.cpp` |
| sysroot | toolchain-dev | `src/sysroot/**` | `tests/unit/sysroot_*.cpp` |
| build | build-dev | `src/build/**` | `tests/unit/build_*.cpp` |
| manifest/workspace/package/formatter | manifest-dev | `src/manifest/**`, `src/workspace/**`, `src/package/**`, `src/formatter/**` | `tests/unit/manifest_*.cpp` etc. |
| testing | Lead | `src/testing/**` | `tests/unit/testing_*.cpp` |

## 4. Frozen headers

```
src/core/common.hpp                      vocabulary types, Result/Error, str helpers
src/core/diagnostics/diagnostic.hpp      Diagnostic, DiagnosticEngine
src/core/filesystem/filesystem.hpp       fs wrapper, CovaDirs, glob, safe delete
src/core/process/process.hpp             ProcessSpec/ProcessResult, run()
src/core/hashing/hash.hpp                Sha256, Fingerprint, Digest
src/core/version/version.hpp             Version, VersionRequirement
src/core/json/json.hpp                   JSON value model
src/core/toml/toml.hpp                   TOML reader/writer
src/core/config/config.hpp               user config, precedence merge
src/testing/test.hpp                     in-tree test framework
src/platform/triple/triple.hpp           Triple, Arch/Os/Env/Abi, known targets
src/platform/abi/abi.hpp                 AbiSignature, OsVersion, ABI checks
src/platform/constraints/constraints.hpp Constraint, CandidateEnvironment, evaluate()
src/toolchain/descriptor.hpp             ToolchainDescriptor, ResolvedToolchain
src/toolchain/discovery/discovery.hpp    system compiler discovery
src/toolchain/probe/probe.hpp            capability probes
src/toolchain/resolver/resolver.hpp      ResolutionRequest/Report, resolve()
src/sysroot/manager/manager.hpp          SysrootManifest, SysrootManager, safe extract
src/manifest/manifest.hpp                Manifest model, load/parse/to_toml
src/workspace/workspace.hpp              Workspace, discover_project
src/build/graph/graph.hpp                Action, ActionId, BuildGraph
src/build/action/action.hpp              CompilerProvider, Compile/Link/ArchiveRequest
src/build/planner.hpp                    plan_package/plan_workspace
src/build/scheduler/scheduler.hpp        pure Scheduler
src/build/executor/executor.hpp          Executor, BuildResult
src/build/cache/cache.hpp                BuildState, Depfile, FileHasher
src/formatter/formatter.hpp              clang-format integration
src/package/lockfile/lockfile.hpp        cova.lock, cova-toolchain.lock
src/cli/cli.hpp                          parsing, Invocation, CommandContext, Ui
```

A module owner **implements every function declared in their headers**. If a
declaration turns out to be wrong, fix the header and tell the Lead in the same
message — do not leave a declaration unimplemented, because the final link of
`bin/cova` covers all of them.

## 5. Build & test commands

```bash
python3 scripts/devbuild.py list                 # modules, sources, tests
python3 scripts/devbuild.py module core          # build one module archive
python3 scripts/devbuild.py test-module core     # build + run that module's tests
python3 scripts/devbuild.py build                # link bin/cova (needs every module)
python3 scripts/devbuild.py test                 # build + run every test binary
python3 scripts/devbuild.py clean
```

Rules of the road:
* Verify with `test-module <yours>` before reporting done. It compiles your
  module *and its dependencies* in a scratch build dir; a green run means your
  archive links and your unit tests pass.
* `devbuild.py` globs `src/<module dir>/**/*.cpp`, so new files need no listing.
  Every `.cpp` in your scope must compile — no dead scratch files.
* Tests are auto-discovered from `tests/unit/*.cpp`; each test file must define
  `COVA_TEST_MAIN()` and is linked against the module inferred from its filename
  prefix (`platform_*.cpp` → platform). Use `COVA_TEST_MAIN()` exactly once per
  test binary.

### Test style

```cpp
#include "testing/test.hpp"
#include "platform/triple/triple.hpp"

COVA_TEST(parses_canonical_triple) {
  auto t = COVA_REQUIRE_OK(platform::Triple::parse("x86_64-unknown-linux-gnu"));
  COVA_CHECK_EQ(t.to_string(), std::string("x86_64-unknown-linux-gnu"));
}

COVA_TEST_MAIN()
```

* Use `cova::test::TempDir` for anything touching the filesystem, and
  `cova::test::ScopedEnv` to override `COVA_HOME`/`PATH`. Never touch the real
  `~/.cova`.
* Prefer table-driven tests for parsers. Every parser needs at least: a happy
  path, a normalisation case, and a rejection case asserting the error `code`.
* Integration tests that need a real compiler are allowed to *skip* by returning
  early when the compiler is absent — but on this host `g++` and `clang++` exist,
  so they should run.

## 6. C++ conventions

* Namespace `cova::<module>`; `platform::`, `toolchain::`, `build::`, `sysroot::`,
  `manifest::`, `workspace::`, `formatter::`, `lockfile::`, `cli::`, `proc::`,
  `fs::`, `str::`, `json::`, `toml::`, `config::`, `test::`.
* Types: `PascalCase`. Functions/variables: `snake_case`. Members: `snake_case`
  with a trailing `_` only for private members. Enumerators: `PascalCase`.
* Header guards: `#pragma once`. Include order: `<standard>`, blank line, `"cova"`
  headers, sorted by path. Include with project-relative paths (`"core/common.hpp"`,
  not `"../core/common.hpp"`); `-Isrc` is set for you.
* `#include <filesystem>` etc. in headers; put heavier includes in `.cpp`.
* Anonymous-namespace helpers over `static`. `constexpr`/`const` aggressively.
* No `using namespace std`. `std::` qualified explicitly.
* Prefer `std::string_view` parameters, `std::string` returns. Pass big structs by
  `const&`. Return `Result<T>` by value.
* 100-column soft limit; 2-space indent; the repo's `.clang-format` is authoritative
  — run `clang-format -i` on files you create if available.
* Log nothing from library code. Reporting is the CLI's job; library code returns
  diagnostics/errors and records decisions in `decisions` / `notes` vectors.

## 7. Behavioural contracts worth restating

**Target normalisation (TGT-001/007).** `Triple::parse` accepts 2–4 components,
normalises the vendor (`pc` ≡ `unknown` for Linux), canonicalises `amd64`→`x86_64`,
`arm64`→`aarch64`, and `to_string()` emits the canonical 4-component form. The
canonical string is a *stable serialisation*: changing it changes cache keys.

**Resolution (PRD §7.2).** System-first → managed → trusted download. Hard
constraints are filtered *before* source preference is applied. Exact version
requests never silently pick another version. `--offline` and `--no-install` must
produce a diagnostic naming the restriction, not a generic failure.

**Incremental builds (PRD §9.3).** Correctness beats hit rate. A cache key covers
action type, full argv, input file *contents*, toolchain fingerprint, sysroot
fingerprint, target, ABI signature and relevant environment. When in doubt,
rebuild.

**Safety (PRD §12.4/§18.1).** Deletion and extraction validate resolved absolute
paths. Symlinks are never followed out of a managed root. `cova clean` never
touches `~/.cova` unless `--all` is passed.

## 8. Reporting back

When you finish, message the Lead with:
1. What builds and passes (`python3 scripts/devbuild.py test-module <module>` output summary).
2. Any header change you made and why (this is a contract change).
3. Anything you deliberately left unimplemented, and the diagnostic it returns.
4. Any behaviour you believe contradicts the PRD.
