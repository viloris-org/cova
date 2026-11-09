# Build correctness: environment, identity, ABI and publication

This document is the contract for the P0 correctness work requested in the
build-environment audit (FR-01 through FR-15). It describes what the tree
actually enforces today, how each guarantee is tested, and which parts remain
open. It is deliberately narrower than the audit: an item listed under
"Remaining work" must not be treated as delivered.

## 1. Explicit build environment (FR-01)

`cova::proc::collect_environment()` builds the environment of every build and
probe action. The default is **explicit**, not inherited:

| Policy | Meaning |
|---|---|
| `Explicit` | Set by the caller; not inherited from the parent. |
| `InheritedTracked` | Inherited from the parent from the audited allow-list. |
| `Rejected` | Deliberately not visible to the child. |
| `Untracked` | Passed, but its value is not part of the identity. |

The audited allow-list covers compiler/linker search paths (`CPATH`,
`C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`, `LIBRARY_PATH`, `GCC_EXEC_PREFIX`,
`COMPILER_PATH`), tool execution (`PATH`), reproducibility
(`SOURCE_DATE_EPOCH`, `ZERO_AR_DATE`), locale (`LANG`, `LC_*`), temporary
directories, dynamic-loader paths, macOS SDK/deployment selection, and the
Windows system variables required by `CreateProcessW`. Locale names are matched
by prefix, so a new `LC_*` value cannot silently escape the identity.

Flag bags (`CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `CPPFLAGS`) and tool overrides
(`CC`, `CXX`, `LD`, `AR`, `AS`, ...) are rejected: Cova does not read them, and
forwarding them would create an untracked input. Sensitive names (segments or
substrings such as `SECRET`, `PASSWORD`, `TOKEN`, `API_KEY`, `CREDENTIAL`) are
never forwarded, never written to cache metadata, and never included in an
identity. Attempting to add one is an error.

Isolation and reuse:

| Isolation | Produced by | Shared/remote cache | Local incremental |
|---|---|---|---|
| `Hermetic` | Explicit audited environment, every value tracked | allowed | allowed |
| `TrackedEnvironment` | Extra tracked values outside the audited list | denied | allowed |
| `UntrackedEnvironment` | `--inherit-env` or untracked additions | denied | denied |

`ActionEnvironment::environment` is the **exact** child environment, and the
executor passes it through `ProcessSpec::environment`; children never inherit
the parent implicitly. Program lookup uses the child's `PATH`, not the parent's.
Values are part of the action key. An untracked value is not hashed (only its
name is), which is safe because untracked actions are never reused.

Tests: `core_environment_test`, `core_process_test` (`process_explicit_*`), and
the CPATH/LIBRARY_PATH invalidation cases exercised by the build identity tests.

## 2. Build action identity (FR-02)

`kActionIdentitySchemaVersion` (`cova.action-identity/2`) prefixes every key. A
key covers:

* executable identity (explicit compiler path hashed; bare names rely on the
  toolchain fingerprint, which hashes every component),
* the full argument vector, in order, with response files expanded for identity
  purposes (raw bytes plus nested `@file` contents; no sorting, merging or
  dropping of unknown arguments),
* working directory, action kind and execution platform,
* input path, role, origin (`declared`, `discovered`, `observed`), symlink
  target and content hash,
* include-directory listings for compile actions (so a new or renamed header
  in `-I`, `-isystem`, `-iquote` or `CPATH` cannot be missed before a depfile is
  regenerated),
* toolchain and sysroot fingerprints, target triple and effective ABI,
* the explicit environment, and the input-closure flag,
* output paths and primary flags.

Response files are hashed as real inputs: content, order and nested files all
participate. Nested `@file` paths are resolved against the driver's working
directory, which is what GCC (libiberty `expandargv`) and Clang actually do;
resolving them against the containing response file would hash the wrong file.
A missing response file is a distinct identity, so its later appearance changes
the key. Recursive `@file` cycles are detected, recorded and marked incomplete;
both GCC and Clang reject them, so no artifact is published. Linker/assembler
response files passed through `-Wl,`, `-Wa,` or `-Wp,` are expanded too, and
plugin, specs, linker-script and symbol files named on the command line are
content-hashed because the driver or its subprograms read them separately.

Path normalisation beyond the raw representation is intentionally not
performed: without a provider-specific proof of equivalence, the original
representation is preserved.

`BuildState` and the artifact cache carry their own schema versions
(`cova.build-state/2`, `cova.artifact-cache/2`). An older state file is ignored;
an older or malformed cache entry is a miss.

## 3. ABI identity (FR-03, FR-04, FR-15 baseline)

`AbiSignature` is versioned by `cova.abi-signature/2`. Facts carry a state
(`Known`, `Unknown`, `Unsupported`, `Conflicting`) and an evidence scope
(`Toolchain`, `CompileAction`, `TranslationUnit`, `ObjectFile`,
`ExportedInterface`, `LinkedArtifact`). The default for an unprobed fact is
`Unknown`; it is never promoted to a harmless default.

The toolchain probe dumps predefined macros (`-dM -E`) from a translation unit
that includes real standard headers. It records, among others,
`__EXCEPTIONS`, `__GXX_RTTI`, `_GLIBCXX_USE_CXX11_ABI`, `_GLIBCXX_DEBUG`,
`_LIBCPP_ABI_VERSION`, `_LIBCPP_ABI_NAMESPACE`, `__GXX_ABI_VERSION` and the
data-layout macros. `apply_abi_facts()` applies only observed facts; missing
ones stay `Unknown`. `check_abi_compatibility()` returns `Unknown` whenever a
required fact is unproven, even for two otherwise identical signatures.

`classify_abi_argument()` separates `AbiAffecting`, `AbiNeutral`,
`PotentiallyAbiAffecting`, `Unsupported` and `Conflicting`. Known
layout/exception/RTTI/link-mode/stdlib switches are applied in argv order
(`-fexceptions -fno-exceptions` resolves to disabled). `-O*`, `-g`, `-W*` and
include paths are neutral and do not pollute the effective ABI. Unknown switches
and non-ABI `-D` macros are potentially affecting: they contribute an opaque
fingerprint and make the effective state `Unknown`; they are never silently
neutral. The classifier has its own version (`cova.abi-classifier/1`).

The build action identity embeds the **effective** ABI (baseline + ordered
arguments), not the raw flag list. ABI identity alone is never a full artifact
key.

Tests: `platform_abi_test`, `toolchain_resolver_test` (`abi_probe_*`).

## 4. Probe cache versioning (FR-05)

The probe cache key contains:

`probe schema version`, `probe logic version`, `probe source manifest hash`,
Cova probe implementation version, compiler identity (content hash/id, family,
version), resource directory, default sysroot, target/sysroot variant, effective
probe environment identity, timeout, and the sorted required-probe list.

The entry stores schema/logic/source versions, its own probe identity, toolchain
identity, target, sysroot, environment identity, capabilities, status,
diagnostics (notes/failures) and ABI facts. A mismatch in any version or
identity is a cache **miss**, never silent compatibility. Every probe process
receives the explicit environment, so a probe's answer is tied to the
environment that produced it.

Tests: `toolchain.probe_cache_is_versioned_and_rejects_mismatch`.

## 5. Dependency discovery and input closure (FR-13 baseline)

Inputs distinguish three origins:

* **DeclaredInputs** — named by the planner (source, object, library).
* **DiscoveredInputs** — returned by the provider's dependency scan.
* **ObservedInputs** — seen in the post-execution depfile but not in the scan.

Before a compile action is executed after a cache/state miss, Cova runs the
provider's dependency scan (`-MM` with the same effective flags and
environment). The scan replaces previously discovered headers (so another
target's or another include path's headers cannot accumulate), sets
`input_closure_complete`, writes the canonical `.d` file and recomputes the
final action identity. A cache/state check is then retried with that identity;
a cache entry produced by a different closure is not used.

After execution:

* every declared input is re-hashed (`Action::inputs_stable`); a change between
  discovery and execution discards the result,
* every post-execution depfile input must already be in the scanned closure; an
  `ObservedInput` makes the closure unverified,
* unverified closures are neither recorded in `state.toml` nor published to the
  artifact cache.

Include-directory listings and symlink targets are part of the identity, so a
new higher-priority same-name header, a renamed/deleted header or a changed
symlink target changes the key even before a scan. A missing include directory
is complete evidence (its listing is empty) and a later appearance changes the
key. An enumeration error (unreadable directory, I/O failure) is **not**
treated as a partial listing: the identity is marked incomplete, the action may
run, but no cache or incremental record is reused or published. A missing or
corrupt depfile causes a fresh scan; it cannot cause a false hit. Actions with an
unproven closure may run, but they are not marked hermetic and cannot enter
shared caches.

A dependency scan is only paid when the closure is genuinely unknown. A hot
build is answered by the recorded identity and state. If the recorded identity
and all input hashes still match but only an output is missing, the previous
closure is reused without a scan (`BuildState::inputs_match`). A change to one
translation unit scans only that unit. A cold build has no previous closure and
therefore scans every compile action before its first execution; that is the
price of proving the closure before publishing, and it is the main remaining
performance cost. The verbose log reports each `dependency scan:` so the cost is
visible.

Remaining limitation: there is no persistent scan-result cache and inputs are
not snapshotted; the post-execution hash re-check is a detection mechanism, not
a filesystem transaction.

## 6. Execution and artifact publication (FR-14 baseline)

An action moves through:

```
Prepared -> Executing -> Validating -> Committed
                     \-> Failed / Discarded
```

`ActionOutcome` exposes `succeeded` (ExecutionSucceeded),
`artifact_validated` (every declared output exists after a successful process)
and `share_eligible`. `Committed` is set by the caller that publishes the cache
entry.

Publication happens only after the whole build succeeded and the produced
binaries passed target/architecture verification. The cache entry is written
into an atomically created directory; blobs are content-addressed and written
before the manifest, so a crash or disk-full failure leaves either no manifest
(hence a miss) or a complete entry. A second writer for the same key with
different output content records `nondeterminism.toml` and returns
`cova::build::cache_nondeterminism` instead of overwriting the first entry.
Restore validates the schema, key, output manifest, blob hashes and the exact
output set before staging files and renaming them into place. A corrupted entry
is a miss. `cova build` reports "restored from cache" separately from
"executed", so a cache hit is never presented as a successful re-execution.

## 7. Performance work

Build performance was measured on a synthetic 41-translation-unit project on
the development host. The numbers are indicative, not a portable benchmark, but
they show where the time went and what changed:

| Scenario | Before this pass | After |
|---|---|---|
| Cold build (fresh `COVA_HOME`, 41 TUs) | 6.3 s | 1.3 s |
| No-change incremental build | 1.9 s | 0.6 s |
| Single-file incremental build | 2.0 s | 0.7 s |
| Toolchain discovery (`cova toolchain list`) | 1.35 s | 0.23 s |

The changes:

* **Hard-link alias grouping.** Distribution aliases (`g++`, `c++`,
  `x86_64-pc-linux-gnu-g++`) are usually hard links to one binary. Discovery
  groups them by file identity, describes one representative per group with the
  version/resource-dir/sysroot queries, and materializes the alias descriptors
  by replacing only the driver path and id.
* **Parallel discovery and subprogram enrichment.** Representative compiler
  descriptions and the selected toolchain's `cc1`/`cc1plus`/assembler/... queries
  run concurrently; results are consumed in deterministic order.
* **Scoped probes.** A normal build asks only for C++ compilation, linking and
  ABI facts. The full capability matrix (C, preprocessing, archives, C++26,
  shared libraries, LTO, sanitizers, coverage, threads) is requested by
  `cova doctor` and `cova explain`. Probe cache keys include the requested
  scope, so the two reports never masquerade as each other.
* **Parallel cold dependency scans.** When there is no incremental state and no
  artifact-cache entry, there is nothing to reuse; the compile actions' closure
  scans run concurrently before the executor. `Action::input_closure_checked`
  prevents the executor from scanning the same action again.
* **No duplicate executor scan.** Warm builds and evidence-backed rebuilds use
  the recorded closure; the `BuildState::inputs_match` fast path avoids a scan
  when only an output is missing.
* **Observability.** `--verbose` reports project load, dependency resolution,
  toolchain resolution, planning, build execution, and each dependency scan's
  duration, so future bottlenecks are measurable without a profiler.

A cold build still contains one preprocessing pass per compile action and the
compiler's own compile pass. Removing that duplication would require a
filesystem snapshot or a compiler integration that exposes the dependency
closure during compilation; Cova deliberately keeps the pre-execution scan for
cache correctness.

## 8. Caching qualification summary

| Condition | Shared artifact cache | Local incremental state |
|---|---|---|
| Hermetic environment, verified closure, artifact validated | yes | yes |
| Tracked environment (`TrackedEnvironment`) | no | yes |
| Untracked environment (`--inherit-env`) | no | no |
| Closure unverified / scan failed / input changed during execution | no | no |
| Schema, key or blob integrity mismatch | miss | n/a |

## 9. Remaining work (not claimed as delivered)

* Remote/shared cache transport, eviction and signed metadata.
* Layered ABI evidence (Layer B per compile action, Layer C artifact evidence)
  and exported-interface inspection. The model and scopes exist; only toolchain
  baseline evidence is produced today.
* Full compiler-subprogram identity is improved for the selected toolchain:
  `cc1`, `cc1plus`, the assembler, `collect2`, `lto-wrapper`, `specs` and a
  linker fallback are queried and content-hashed into the build fingerprint.
  The descriptor's lockfile `content_hash` is deliberately unchanged, so
  existing lockfiles remain valid; build keys use `compute_fingerprint()`, which
  includes the enriched components. Plugins, specs, linker scripts and symbol
  files named on the action command line are hashed into the action identity.
  Toolchain-supplied plugins discovered outside the command line remain a
  follow-up.
* A scan-result cache and filesystem snapshots/overlay to close the
  discovery-to-execution TOCTOU window rather than only detect it.
* Windows MSVC probing (the current probe matrix is GCC/Clang oriented) and
  independent Windows/macOS CI runs for the environment matrix.
* User-facing `[build.env]` configuration; today the policy is the audited
  default plus `--inherit-env`, with the API available for a future config layer.
