# Cova — Trusted Component Registry

Cova's cross-compilation support is only useful if the components it installs
can be trusted. This build ships a **local registry** rather than a public
network service: a directory with an `index.toml`, the archives it names, and a
SHA-256 for every archive. The default location is `~/.cova/registry`, and
`--registry <dir>` selects another root.

Nothing here is fetched over the network. An entry whose `archive` is an
`http(s)://` URL is rejected with `cova::registry::download_unsupported`; the
operator is expected to mirror the archive into the registry and record its
hash.

## Index format

```toml
# index.toml
version = "1"

[[sysroot]]
id = "aarch64-linux-musl-1.2.5"
name = "aarch64-linux-musl"
target = "aarch64-unknown-linux-musl"
crt = "musl"                 # glibc | musl | uclibc | msvc | wasi | none
libcpp = "libstdc++"         # libstdc++ | libc++ | msvc-stl | none
archive = "archives/aarch64-linux-musl-1.2.5.tar.gz"
sha256 = "<64 hex characters>"
size-bytes = 48234112        # optional, verified when present
license = "MIT"
source = "https://example.invalid/musl-release.tar.gz"
provides = ["libc", "libstdc++", "crt"]

[[toolchain]]
id = "aarch64-linux-musl-gcc-14.2.0"
name = "aarch64-linux-musl-gcc"
family = "gcc"               # clang | gcc
version = "14.2.0"           # must match the driver's `--version`
target = "aarch64-unknown-linux-musl"
archive = "archives/aarch64-linux-musl-gcc-14.2.0.tar.gz"
sha256 = "<64 hex characters>"
size-bytes = 312004512       # optional
compiler = "bin/aarch64-linux-musl-g++"
license = "GPL-3.0-or-later"
```

Relative `archive` paths are resolved against the registry root. `file://`
paths are accepted for an operator-managed mirror. `size-bytes` and `sha256`
are checked before extraction; an entry without `sha256` is rejected as a
malformed registry.

## Install sequence

For both sysroots and toolchains:

1. Find an index entry whose name/id matches (and, for sysroots, whose target
   and CRT are compatible).
2. Refuse on `--offline` or `--no-install` before reading the archive.
3. Copy the archive into `~/.cova/downloads` under its digest name.
4. Verify SHA-256 and, when declared, the byte size.
5. Extract with traversal-checked, symlink-checked extraction.
6. Validate the extracted metadata and, for toolchains, run the compiler's
   `--version` and check the family/version/target.
7. Commit atomically into `~/.cova/sysroots/<id>` or
   `~/.cova/toolchains/<id>`, then write the component manifest.

A failed verification leaves no installed entry. Installing the same id again
re-verifies the installed tree rather than assuming it is intact.

## Sysroot archive layout

A sysroot archive should contain a `sysroot.toml` at its root (or in a single
top-level directory) and a normal sysroot layout:

```text
sysroot.toml
usr/
  include/
  lib/
```

`sysroot.toml` follows `SysrootManifest`:

```toml
id = "aarch64-linux-musl-1.2.5"
name = "aarch64-linux-musl"
target = "aarch64-unknown-linux-musl"
crt = "musl"
libcpp = "libstdc++"
include-dirs = ["usr/include"]
lib-dirs = ["usr/lib"]
dynamic-linker = "lib/ld-musl-aarch64.so.1"
license = "MIT"
```

An archive without a manifest is still accepted, but Cova synthesises a
minimal manifest from the target and the usual `usr/include`/`usr/lib`
layout; a manifest is strongly preferred because it records ABI facts the
resolver needs.

## Toolchain archive layout

```text
toolchain.toml
bin/aarch64-linux-musl-g++
bin/aarch64-linux-musl-gcc
bin/aarch64-linux-musl-ld
bin/aarch64-linux-musl-ar
```

The `compiler` field in the index points at the C++ driver. The driver name
must include its target prefix (`aarch64-linux-musl-g++`) so Cova can infer
what it targets; that is also how discovery distinguishes it from the host
compiler. A prefixed cross GCC is treated as self-contained: it already carries
its target runtime and does not require a separately installed Cova sysroot.

## What is deliberately absent

- No public registry, signing infrastructure, mirror rotation or retry policy.
- No network transport. The registry is a local directory by design in this
  milestone.
- Packages (`cova add`, `fetch`, `update`) do not use this registry yet; they
  are Milestone 3.

These are explicit limitations, not silent fallbacks. The goal is that every
component Cova claims to have installed has been verified end to end.
