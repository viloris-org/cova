# Cova — Toolchain Resolution

This document describes how `cova build` turns a target and a manifest into a
frozen `ResolvedToolchain`. The implementation is in
`src/toolchain/resolver/resolver.cpp`; the model is in
`src/toolchain/descriptor.hpp`.

## The chain (PRD §7.2)

1. **Discover.** `toolchain::discover()` scans explicit roots, `PATH`, the
   managed toolchain store and the standard system directories. It recognises
   versioned drivers (`g++-14`, `clang++-18`) and prefixed cross drivers
   (`aarch64-linux-musl-g++`), and infers the target from the prefix. Discovery
   reads the version banner but does not claim capabilities from it.
2. **Filter hard constraints.** Version requirements, compiler family, pinned
   identity/content hash, target support and the manifest/CLI constraint list are
   applied before policy preference. A host GCC is rejected for a cross target
   instead of being probed as if it could target it. An exact version never
   silently selects another version.
3. **Rank by policy.** `system-first`, `managed-first`, `system-only` and
   `managed-only` order the surviving candidates. For cross-compilation, Clang
   is preferred because it can target a different triple with `--target`.
4. **Probe.** A real compile, archive and link are attempted for the requested
   target (plus standard-library and C++26 baseline probes). The probe passes the
   resolved sysroot explicitly and its cache key includes target + sysroot, so a
   host probe can never be reused for a cross target. A candidate that cannot
   compile and link C++ is rejected with a `NotProbed` reason.
5. **Attach a sysroot.** Native targets need no sysroot, and a prefixed cross GCC
   is self-contained because it carries its target runtime. Other cross targets
   look for a compatible sysroot in the managed store and in system locations
   (`$COVA_SYSROOT`, `/opt/<triple>/sysroot`, `/usr/local/<triple>/sysroot`).
   The path is passed to the compiler as `--sysroot=` during probing and build
   actions. If none is available, the resolver reports `SysrootMissing` (or
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
fingerprint, target, ABI signature, sysroot path/id and the sysroot's content
fingerprint. Managed sysroots use their manifest/archive hash; system sysroots
are fingerprinted from their path, directory mtimes and key libc headers. The
default `cova-toolchain.lock` records the identity, not only the version, so a
different build of "the same version" cannot be substituted silently, and a
changed sysroot invalidates incremental build records.

Locked mode requires an entry for the requested target; a missing entry is an
error. A content hash or ABI mismatch is reported rather than ignored. Lockfile
writes are atomic (temporary file plus rename).

## Trust and honesty

- Managed components come from a **trusted local registry** (`~/.cova/registry`
  or `--registry <dir>`), whose `index.toml` records a SHA-256 (and optionally a
  size) for every archive. See [REGISTRY.md](REGISTRY.md). An `http(s)://`
  archive URL is rejected: this build has no network transport and will not
  simulate a download it cannot verify (PRD §18.1).
- An archive is checksummed and size-checked, copied into the digest-named
  download cache, extracted with traversal/symlink checks, and only then
  committed atomically. A toolchain's compiler banner is re-read from the
  committed tree and its family/version/target must match the registry entry.
- After a successful build, Cova inspects each primary binary (ELF, PE/COFF,
  Mach-O or WebAssembly) and fails the build on an architecture mismatch. A host
  compiler that ignored `--target` cannot pass as a cross build.
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
  suggested action: install a cross compiler with `cova toolchain install`, or
    provide a compatible sysroot with `cova sysroot install`
```

`cova explain` prints the ranked candidate decisions and rejections as JSON or
text without executing a build.
