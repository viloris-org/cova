# Cova — Toolchain Resolution

This document describes how `cova build` turns a target and a manifest into a
frozen `ResolvedToolchain`. The implementation is in
`src/toolchain/resolver/resolver.cpp`; the model is in
`src/toolchain/descriptor.hpp`.

## The chain (PRD §7.2)

1. **Discover.** `toolchain::discover()` scans explicit roots, `PATH`, the
   managed toolchain store and the standard system directories for `clang++` /
   `g++` drivers. Discovery reads the version banner but does not claim
   capabilities from it.
2. **Filter hard constraints.** Version requirements, compiler family, pinned
   identity/content hash and the manifest/CLI constraint list are applied before
   policy preference. An exact version never silently selects another version.
3. **Rank by policy.** `system-first`, `managed-first`, `system-only` and
   `managed-only` order the surviving candidates. For cross-compilation, Clang
   is preferred because it can target a different triple with `--target`.
4. **Probe.** A real compile, archive and link are attempted for the requested
   target (plus standard-library and C++26 baseline probes). Results are cached
   per toolchain fingerprint under `~/.cova/cache/probes/`. A candidate that
   cannot compile and link C++ is rejected with a `NotProbed` reason.
5. **Attach a sysroot.** Native targets need no sysroot. Cross targets look for
   an installed sysroot whose declared target/CRT is compatible. The path is
   passed to the compiler as `--sysroot=` during probing and build actions. If
   none is installed, the resolver reports `SysrootMissing` (or
   `OfflineRestriction` / `DownloadUnavailable` when installation would be
   required).
6. **Evaluate constraints.** A `CandidateEnvironment` is judged against the
   manifest and CLI constraints. Each rejection records a reason
   (`VersionMismatch`, `TargetUnsupported`, `SysrootMissing`,
   `RuntimeIncompatible`, `ConstraintUnsatisfied`, ...).
7. **Freeze.** The selected descriptor, probed capabilities, ABI signature,
   sysroot identity and a deterministic fingerprint become the immutable
   `ResolvedToolchain` handed to the planner.

## Identity and the lockfile

`ResolvedToolchain::fingerprint_input()` hashes the toolchain content
fingerprint, target, ABI signature and sysroot path/id. The default
`cova-toolchain.lock` records the identity, not only the version, so a different
build of "the same version" cannot be substituted silently.

Locked mode requires an entry for the requested target; a missing entry is an
error. A content hash or ABI mismatch is reported rather than ignored. Lockfile
writes are atomic (temporary file plus rename).

## Trust and honesty

- No registry is configured in this build, so managed toolchain and sysroot
  **download** paths return an explicit diagnostic. Cova does not simulate a
  download it cannot verify (PRD §18.1).
- A local sysroot archive is checksummed, extraction is traversal-checked and
  symlink escapes are rejected, then the installed tree is structurally
  verified.
- `--offline`, `--no-install` and `--frozen` are enforced before any install
  path would be considered.

## Failure diagnostic

On failure `build_failure_diagnostic()` renders:

```text
error: unable to resolve a toolchain
  target: aarch64-unknown-linux-musl
  required: clang >= 20, C++ compilation, executable linking
  found: clang 23.1.4 (system); gcc 16.2.1 (system)
  missing: a compatible aarch64-unknown-linux-musl sysroot
  suggested action: install a C++ compiler (clang or gcc), or provide a
    compatible sysroot with `cova sysroot install`
```

`cova explain` prints the ranked candidate decisions and rejections as JSON or
text without executing a build.
