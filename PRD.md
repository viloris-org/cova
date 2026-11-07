# Cova — Product Requirements Document (PRD)

**Version:** 1.0  
**Status:** Draft / Architecture design phase  
**Product name:** Cova  
**Product type:** C/C++ build system, package manager and cross-platform toolchain manager  
**Core implementation language:** C++26  
**Product positioning:** Cross-Compilation-First, Cargo-Inspired C/C++ Development Toolchain

---

# 1. Product Overview

## 1.1 Product Introduction

Cova is an open-source project management, build, dependency management and toolchain management system for modern C/C++ development.

Cova aims to solve the long-standing problems of the C/C++ ecosystem: complex build configuration, fragmented dependency management, difficult cross-compilation, inconsistent toolchain environments and high project migration costs.

Cova draws on the unified development workflow of Rust's Cargo, but does not directly copy Cargo's underlying design. Instead, it establishes a native architecture for C/C++ ABIs, compiler differences, linking models, system dependencies and cross-platform requirements.

Users should be able to create projects, install dependencies, compile, run, test, format, clean and perform cross-platform builds with a small number of commands.

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

Cross-compilation should be equally simple:

```bash
cova build --target aarch64-unknown-linux-musl
```

Cova automatically discovers system toolchains and, when a compatible toolchain, Sysroot or Runtime is missing, downloads the missing components from trusted sources and installs them into a user directory managed exclusively by Cova.

Cova should not modify system toolchains or the system environment without authorisation.

## 1.2 Product Vision

Bring the development experience of C/C++ projects close to that of modern language ecosystems:

- Creating a project does not require writing build scripts by hand.
- Everyday development does not require working with the compiler and linker directly.
- Dependency declaration, version resolution and installation are managed uniformly.
- Cross-compilation no longer depends on extensive manual configuration.
- A consistent command-line experience across platforms.
- The build process is explainable, traceable and reproducible.
- Developers can freely choose between system toolchains and managed toolchains.
- The tool itself is implemented in C++ and can bootstrap itself.

**Long-term vision: become the unified entry point for the C/C++ development workflow.**

## 1.3 Product Design Principles

| Principle | Definition |
|---|---|
| Cross-Compilation First | Cross-compilation is a core capability, not an add-on |
| Convention over Configuration | Common projects need zero configuration; complex projects configure explicitly |
| System First | Prefer a system toolchain that satisfies the constraints by default |
| Managed Fallback | Automatically install Cova-managed versions when components are missing |
| Non-invasive | Never modify the system environment or system toolchains without authorisation |
| Reproducibility | Support locking dependencies, toolchains and the build environment |
| Transparency | Every automatic selection and download is explainable |
| Extensibility | Compilers, package sources and build backends are extensible |
| Self-hosting | Cova can build itself |
| Security by Default | Download verification, path isolation, trusted sources and safe cleanup |

---

# 2. User Groups and Use Cases

## 2.1 Target Users

**Individual C/C++ developers**

Want to create, compile and manage projects quickly without maintaining complex build scripts.

**Cross-platform application developers**

Need to build applications on Linux, Windows, macOS or other environments.

**Embedded and systems software developers**

Need to generate code for ARM, RISC-V or bare-metal targets, and manage specific compilers, Sysroots and linker scripts.

**Open-source library maintainers**

Need to manage dependencies, test matrices, different C++ standards and different ABI combinations.

**CI/CD and infrastructure engineers**

Need stable, reproducible and automated cross-platform build environments.

## 2.2 Core User Stories

- US-001: As a developer, I want to create a runnable C++ project with a single command.
- US-002: As a developer, I want to add dependencies by package name without manually configuring header files and link libraries.
- US-003: As a developer, I want to automatically use a compatible compiler that is already installed on the machine.
- US-004: As a developer, when the machine lacks a toolchain for the target platform, I want Cova to install the missing components automatically.
- US-005: As a cross-platform developer, I want to build Windows or ARM Linux programs from Linux.
- US-006: As a developer, I want to specify an exact compiler version and get consistent selection results.
- US-007: As a developer, I want to clean project build artifacts without deleting downloaded dependencies and toolchains.
- US-008: As a team member, I want to use a unified formatting tool and code style.
- US-009: As a CI engineer, I want to verify the build environment with a Lockfile and toolchain fingerprints.
- US-010: As a systems developer, I want to know why Cova chose a particular compiler or rebuilt a particular file.
- US-011: As a Cova maintainer, I want to use Cova to build Cova itself.
- US-012: As a library developer, I want explicit control over a dependency's public headers, compile options and link interface.

---

# 3. Product Scope

## 3.1 Core Capabilities

Cova is divided into eight major functional domains:

1. Project and Workspace management.
2. Building, running, testing and cleaning.
3. Dependency resolution, fetching and building.
4. Target and cross-compilation management.
5. Toolchain Resolver.
6. Sysroot and SDK management.
7. Formatting, diagnostics and development tool integration.
8. Caching, reproducibility and self-hosting.

## 3.2 Non-Goals

The following are outside the initial delivery scope:

- Developing a C/C++ compiler front end from scratch.
- Developing a complete C++ standard library from scratch.
- Implementing a code formatting algorithm from scratch.
- Fully replacing the operating system's SDK management mechanisms.
- Unconditionally supporting every Target on every Host.
- Establishing a complete public package repository in the first version.
- Implementing a distributed build cluster in the first version.
- Guaranteeing C++ Modules compatibility across all compilers in the first version.
- Guaranteeing byte-for-byte reproducible build artifacts in the first version.

---

# 4. CLI Product Design

## 4.1 Command System

Cova uses a unified command-line entry point:

```bash
cova <command> [options]
```

### Project Management

| Command | Description | Priority |
|---|---|---|
| `cova new` | Create a new project | P0 |
| `cova init` | Initialise an existing directory | P0 |
| `cova metadata` | Output project metadata | P1 |
| `cova workspace` | Workspace management | P1 |

### Building and Development

| Command | Description | Priority |
|---|---|---|
| `cova build` | Build the project | P0 |
| `cova run` | Build and run | P0 |
| `cova clean` | Clean build artifacts | P0 |
| `cova fmt` | Format code | P0 |
| `cova test` | Build and execute tests | P1 |
| `cova check` | Run compilation checks that do not require final linking | P1 |
| `cova graph` | Inspect the build graph | P2 |
| `cova explain` | Explain build and cache decisions | P2 |

### Dependency Management

| Command | Description | Priority |
|---|---|---|
| `cova add` | Add a dependency | P1 |
| `cova remove` | Remove a dependency | P1 |
| `cova fetch` | Fetch dependency sources | P1 |
| `cova update` | Update dependency resolution results | P1 |
| `cova tree` | Inspect the dependency tree | P1 |
| `cova vendor` | Export dependency sources into the project | P2 |

### Toolchain Management

| Command | Description | Priority |
|---|---|---|
| `cova toolchain list` | List toolchains | P0 |
| `cova toolchain install` | Install a managed toolchain | P0 |
| `cova toolchain remove` | Uninstall a managed toolchain | P1 |
| `cova toolchain explain` | Explain toolchain selection | P1 |
| `cova toolchain gc` | Reclaim unused managed components | P2 |
| `cova sysroot list` | List Sysroots | P0 |
| `cova sysroot install` | Install a Sysroot | P0 |
| `cova sysroot verify` | Verify a Sysroot | P1 |
| `cova target list` | List supported Targets | P1 |
| `cova doctor` | Diagnose the development environment | P1 |

### Release and Maintenance

| Command | Description | Priority |
|---|---|---|
| `cova install` | Install a built program | P2 |
| `cova package` | Create a release package | P2 |
| `cova publish` | Publish a package to the Registry | P3 |
| `cova cache gc` | Reclaim build caches | P2 |

## 4.2 Global Options

```bash
--target <triple>
--profile <name>
--compiler <family@version>
--toolchain <id>
--offline
--locked
--frozen
--no-install
--verbose
--quiet
--jobs <number>
--color <auto|always|never>
```

Semantics:

- `--offline`: forbid network access.
- `--locked`: forbid modification of the existing Lockfile; report an error when entries are missing or inconsistent.
- `--frozen`: require both offline and locked state.
- `--no-install`: forbid installing new components, while allowing existing local resources to be used.
- `--target`: specify the target platform for the artifacts.
- `--compiler`: add a compiler family and version constraint.
- `--toolchain`: specify a known toolchain identity.

All commands must consistently honour the applicable semantics of these options.

---

# 5. Project Initialisation

## 5.1 Creating a Project

```bash
cova new hello
cova new math --lib
cova new engine --workspace
```

Default project structure:

```text
hello/
├── cova.toml
├── .gitignore
├── .clang-format
├── src/
│   └── main.cpp
└── tests/
```

A C++26 project is generated by default.

The initial source code should be as simple as possible and should avoid introducing unnecessary dependencies.

## 5.2 Initialising an Existing Project

```bash
cova init
```

Requirements:

- Do not overwrite existing files unless the user explicitly confirms.
- Detect existing source directories.
- Recognise common layouts for executable and library projects.
- Detect existing build configurations and provide migration advice.
- Do not automatically delete existing configurations such as CMake or Meson.

## 5.3 Manifest Format

Cova uses TOML as its declarative configuration format.

Example:

```toml
[package]
name = "hello"
version = "0.1.0"
edition = "c++26"

[build]
type = "executable"
sources = ["src/**/*.cpp"]

[dependencies]
fmt = "11"

[toolchain]
policy = "system-first"
compiler = "clang"
version = ">=20, <23"
auto-install = true

[profile.dev]
optimization = 0
debug = true

[profile.release]
optimization = 3
lto = true

[format]
tool = "clang-format"
style = "file"
```

The syntax above is a draft of the intended design; the Manifest Schema specification is authoritative.

## 5.4 Configuration Precedence

The following order is recommended:

1. Explicit CLI options.
2. Project configuration.
3. Workspace configuration.
4. User global configuration.
5. Cova default configuration.

The Lockfile is not an ordinary configuration override layer; it is a record of constraints on an already-resolved environment. In locked mode, every selection must satisfy the locked result.

---

# 6. Target Model

## 6.1 Design Goals

The Target Model must be independent of any specific compiler.

The same Target may be realised by multiple toolchain combinations.

Targets are divided into:

- Target Identity.
- Target Constraints.
- Resolved Target Environment.

## 6.2 Target Identity

Use an identifier scheme similar to the LLVM Target Triple:

```text
x86_64-unknown-linux-gnu
aarch64-unknown-linux-musl
x86_64-pc-windows-gnu
x86_64-pc-windows-msvc
wasm32-wasi
riscv64-unknown-linux-gnu
```

Internally, a structured data model must be used rather than repeatedly parsing strings in business logic.

## 6.3 Target Constraints

Must support expressing:

- CPU architecture.
- CPU microarchitecture.
- Instruction set extensions.
- OS and ABI.
- C Runtime.
- C++ standard library requirements.
- Minimum deployment OS version.
- Static or dynamic linking strategy.
- Exception and RTTI requirements.
- Threading model.
- SDK version constraints.

A Target Triple should not directly contain a compiler's concrete installation path.

## 6.4 Target Resolution Requirements

**TGT-001**: Parse and normalise supported Target Triples.

**TGT-002**: Unknown or unsupported Targets must produce a clear diagnostic.

**TGT-003**: Detect basic compatibility between the target platform and the Sysroot.

**TGT-004**: Support user-defined Target Specs.

**TGT-005**: Support Target-specific dependencies and build configuration.

**TGT-006**: Distinguish the build platform, the tool execution platform and the artifact target platform.

**TGT-007**: Normalised Target results must have a stable serialisation form.

---

# 7. Toolchain Resolver

## 7.1 Core Goals

Resolve a complete and verified compilation environment from the Target, compiler version, ABI, standard library and other constraints declared by the user.

Cova should not merely look for a compiler path; it should resolve a compatible combination of toolchain components.

## 7.2 Default Policy

Use:

**System First → Managed Fallback → Trusted Download**

Specific rules:

1. Discover system toolchains.
2. Verify whether the system toolchain satisfies the constraints.
3. If it does, prefer it.
4. If it does not, look for a toolchain already managed by Cova.
5. If it still does not, obtain installable components from trusted Registries.
6. Verify compatibility of the toolchain with the Sysroot and Runtime.
7. Freeze the resolution result.
8. Hand the resolution result to the Build Graph.

For exact version requirements, it must not choose another version on its own.

For a locked exact component identity, it must not substitute another toolchain merely because the version number is the same.

## 7.3 Policy Types

```toml
[toolchain]
policy = "system-first"
```

Four policies are supported:

- `system-first`
- `managed-first`
- `system-only`
- `managed-only`

`system-first` is the default.

Source priority applies only after a candidate satisfies all hard constraints.

## 7.4 Mixed-Source Support

Cova must support combining toolchain components from different sources.

For example:

```text
Compiler:    /usr/bin/clang++
Linker:      /usr/bin/ld.lld
Sysroot:     ~/.cova/sysroots/aarch64-musl/
Runtime:     ~/.cova/store/...
```

Requirements:

- Do not re-download an existing compiler just because the Sysroot is missing.
- Do not assume the environment is complete just because a system compiler exists.
- Verify compatibility among the compiler, linker, Runtime and Sysroot.
- Record the source and identity of each component.

## 7.5 Toolchain Descriptor

Must include:

- Toolchain ID.
- Compiler family and version.
- Executable paths.
- Host platform.
- Supported Targets.
- Compiler resource directory.
- Linker and archiver.
- Runtime information.
- Sysroot constraints.
- Capability declarations.
- Content hash.
- Source and trust level.

## 7.6 Capability Probing

Capabilities must not be judged solely by version number.

At minimum, probe:

- C/C++ compilation.
- The specified target architecture.
- Object file format.
- Static library creation.
- Executable linking.
- Availability of standard library headers.
- Language features required by C++26.
- The specified ABI and Runtime.
- Required link options.

Capabilities such as C++ Modules, LTO and Sanitizers should be probed independently.

## 7.7 Failure Diagnostics

Example:

```text
error: unable to resolve toolchain

Target:
  aarch64-unknown-linux-musl

Required:
  Clang >= 20
  libc: musl
  C++ standard: C++26

Found:
  System Clang 19
  Managed Clang 18

Missing:
  Compatible compiler
  AArch64 musl sysroot

Suggested action:
  cova toolchain install ...
```

Diagnostics must distinguish:

- Version mismatch.
- Unsupported Target.
- Missing Sysroot.
- Incompatible Runtime.
- Download unavailable.
- Offline restrictions.
- Registry verification failure.

---

# 8. Sysroot Manager

## 8.1 Product Goals

The Sysroot Manager is responsible for discovering, installing, verifying, composing and managing the system compilation resources of the target platform.

It must not treat a Sysroot as merely a folder path.

## 8.2 Managed Resources

This includes:

- System headers.
- C Runtime.
- C++ Standard Library.
- Compiler Runtime.
- Startup Objects.
- System link libraries.
- SDK metadata.
- Platform ABI information.
- Minimum deployment version constraints.

## 8.3 Storage Layout

A user-level data directory is recommended:

```text
~/.cova/
├── toolchains/
├── sysroots/
├── store/
├── downloads/
├── registry/
├── cache/
└── config.toml
```

Actual paths must follow the user data directory conventions of the respective operating system and may be overridden through configuration.

## 8.4 Immutable Component Store

Managed components use content addressing or an equivalent immutable storage mechanism.

Requirements:

- Installed components must not be modified silently.
- The installation process must include integrity verification.
- Support component reuse.
- Support safe garbage collection.
- Do not affect SDKs installed on the system.
- Do not modify system directories without authorisation.

## 8.5 Sysroot Compatibility

The following must be considered:

- Target Triple.
- libc family and version.
- ABI.
- CPU architecture.
- Runtime.
- Minimum deployment OS version.
- Dynamic linker.
- Compiler runtime dependencies.

For Linux glibc targets, pay attention to symbol versions and minimum runtime environment requirements.

## 8.6 Automatic Installation

When a compatible compiler already exists on the system but the Sysroot is missing, install only the missing components.

Installation must:

1. Fetch metadata from trusted sources.
2. Resolve dependent components.
3. Show or record the download plan.
4. Download to a temporary directory.
5. Verify content integrity.
6. Unpack safely.
7. Atomically commit to the managed store.
8. Perform compatibility verification.
9. Update installation records.

For large downloads, licensing requirements or interactive usage scenarios, provide confirmation and policy configuration.

---

# 9. Build System

## 9.1 Build Model

Cova uses an independent Build Graph IR.

The build graph contains at least the following Actions:

- Compile.
- Scan Dependencies.
- Archive.
- Link.
- Generate.
- Copy.
- Install.
- Test.

The Build Graph must not depend directly on a specific compiler command-line format.

## 9.2 Build Backend

Ninja is recommended as the execution backend initially.

At the same time, keep the build graph model independent to leave room for a future native executor.

Cova should not require end users to write Ninja files by hand.

## 9.3 Incremental Builds

Requirements:

- Track source file changes.
- Track header dependencies.
- Track generated files.
- Track changes in compilation arguments.
- Track toolchain changes.
- Track Sysroot changes.
- Track dependency library changes.
- Avoid unnecessary recompilation.

Incremental build correctness takes precedence over cache hit rate.

## 9.4 Parallel Builds

Support:

```bash
cova build --jobs 8
```

By default, choose parallelism based on available CPUs and resource limits.

Build scheduling must respect the dependency graph; a consumer Action must never run before its dependency files have been generated.

## 9.5 Host Tools and Target Artifacts

The build system must support:

- Code generators that run on the build platform.
- Libraries compiled for the target platform.
- Cross-platform dependency graphs.
- Transitive dependencies on generated source code.
- Coexistence of toolchains for different platforms.

For example, run a code generator on x86-64 Linux, then use an AArch64 toolchain to compile its output.

## 9.6 C++ Modules

Long-term goals include:

- Module Interface Unit identification.
- Module dependency scanning.
- BMI generation.
- Compilation order computation.
- Compiler-specific BMI management.
- Modules cache isolation.

The first version does not promise cross-compiler BMI compatibility.

---

# 10. Dependency Management

## 10.1 Goals

Implement a Cargo-like dependency declaration and workflow while correctly handling C/C++ ABI and platform differences.

## 10.2 Core Commands

```bash
cova add fmt
cova add spdlog@1.15
cova add catch2 --dev

cova remove fmt
cova update
cova fetch
cova tree
```

## 10.3 Dependency Sources

Supported initially:

- Local path dependencies.
- Git dependencies.
- Trusted Registry dependencies.

Supported later:

- System packages.
- CMake Package Config.
- pkg-config.
- vcpkg/Conan ecosystem adapters.

## 10.4 Dependency Stages

Dependency management is split into:

1. Dependency Resolution.
2. Source Fetching.
3. Source Verification.
4. Package Build Planning.
5. Target-specific Compilation.
6. Artifact Caching.

## 10.5 Dependency Identity

Distinguish:

**Source Identity**

- Package name.
- Version.
- Source.
- Source hash.

**Binary Identity**

- Source Identity.
- Target.
- Compiler.
- Standard library.
- ABI.
- Features.
- Build configuration.
- Sysroot / Runtime constraints.

Two dependencies with the same version must not be assumed to share a binary build artifact.

## 10.6 Dependency Propagation

Must support:

- Public Includes.
- Private Includes.
- Interface Compile Definitions.
- Public Link Dependencies.
- Private Link Dependencies.
- Build Dependencies.
- Development Dependencies.

## 10.7 Version Resolution

Support semantic version constraints and deterministic resolution.

Must handle explicitly:

- Transitive dependency conflicts.
- Feature constraints.
- Platform-conditional dependencies.
- Optional dependencies.
- ABI-related Features.
- Limits on multiple versions of the same package coexisting.

Cargo's Feature unification rules cannot be copied directly, because C++ macros and compile options can affect the ABI.

## 10.8 Registry Strategy

The first version prioritises building a unified Package Provider interface.

It does not require maintaining a complete public Registry immediately.

Existing package recipes and build ecosystems can be reused first, but Cova itself should own independent Manifest, resolution model and Lockfile semantics.

---

# 11. Formatting

## 11.1 Commands

```bash
cova fmt
cova fmt --check
cova fmt --diff
cova fmt --all
```

## 11.2 Formatting Backend

Uses clang-format by default.

Resolution strategy:

1. Find a system clang-format that satisfies the requirements.
2. Find a Cova-managed clang-format.
3. If allowed, download a compatible version automatically.
4. Run formatting.

The formatter version is managed independently of the C++ compiler version.

## 11.3 File Scope

By default, includes project sources and headers.

Excluded by default:

- Build output.
- Third-party dependency sources.
- Generated files.
- Cova caches.
- Directories explicitly ignored by the user.

## 11.4 CI Behaviour

`cova fmt --check`:

- Does not modify files.
- Exits with code 0 when formatting is correct.
- Returns a non-zero exit code when formatting is incorrect.
- Outputs files that do not conform to the formatting requirements.
- Supports machine-readable diagnostic results.

---

# 12. Cleanup and Caching

## 12.1 Clean Command

```bash
cova clean
cova clean --release
cova clean --target aarch64-unknown-linux-musl
cova clean --all
```

By default, cleans only the current project's build artifacts.

Must not delete the following by default:

- Downloaded dependency sources.
- Cova-managed toolchains.
- Sysroots.
- Global shared caches.

## 12.2 Cache Categories

- Project build cache.
- Dependency source cache.
- Binary dependency cache.
- Toolchain component store.
- Sysroot component store.
- Registry metadata cache.

## 12.3 Cache Identity

A build Action cache key must cover:

- Action type.
- Complete command arguments.
- Actual input files.
- Toolchain identity.
- Sysroot identity.
- Target.
- ABI.
- Build configuration.
- Relevant environment variables.

## 12.4 Safe Cleanup

Requirements:

- Clean only registered output paths.
- Prevent path traversal.
- Prevent out-of-bounds deletion caused by symbolic links.
- Do not delete data the user has not authorised.
- Use reference and reachability checks for shared caches.
- Support previewing before cleanup.

---

# 13. Testing and Running

## 13.1 Local Run

```bash
cova run
cova run -- arg1 arg2
```

Cova must automatically build the required targets and pass application arguments to the target program.

## 13.2 Testing

```bash
cova test
cova test --workspace
```

Initially, support building and executing test programs declared by Cova.

Later, adapt testing frameworks such as GoogleTest, Catch2 and doctest.

## 13.3 Cross-Compilation Runner

Cross-compilation does not mean the artifacts can run directly on the build machine.

Must support:

- Native Runner.
- QEMU.
- Wine.
- Emulators.
- Remote execution devices.

If the Target cannot execute in the current environment and no Runner is configured, `cova run` and `cova test` must report a clear error and must not attempt to run the program incorrectly.

---

# 14. Workspace

## 14.1 Workspace Manifest

```toml
[workspace]
members = [
    "packages/core",
    "packages/network",
    "apps/server"
]
```

## 14.2 Functional Requirements

- Multi-Package builds.
- Workspace dependency resolution.
- Shared Lockfile.
- Shared toolchain policy.
- Inter-Package dependencies.
- Workspace-level builds and tests.
- Single-Package builds.
- Cross-platform dependency graphs.

Support:

```bash
cova build --workspace
cova test --workspace
cova fmt --workspace
```

---

# 15. Lockfile and Reproducibility

## 15.1 Lockfiles

Recommended files:

```text
cova.lock
cova-toolchain.lock
```

The former records dependency source resolution results.

The latter records the identity of toolchains, Sysroots, SDKs, Runtimes and related components.

## 15.2 Locking Strategy

The default development mode allows preferring the system toolchain when it satisfies the constraints.

Locked mode must obey the Lockfile constraints.

For system toolchains, record their identity, version, related components and a verifiable fingerprint.

For managed toolchains, prefer content hashes to identify exact resources.

## 15.3 Reproducibility Levels

Three levels are defined:

**Level 1: Dependency Reproducibility**

Dependency sources and versions are identical.

**Level 2: Environment Reproducibility**

Toolchain, Sysroot, Runtime and build arguments are constrained.

**Level 3: Artifact Reproducibility**

Build artifacts achieve verifiable determinism in a controlled environment.

Cova initially targets Levels 1 and 2.

Level 3 is a long-term capability and should not be promised before it is verified.

---

# 16. Self-Hosting Design

## 16.1 Goal

Cova is implemented in C++26 and will ultimately build itself using Cova.

## 16.2 Bootstrap Stages

**Stage 0**

Build the initial Cova with an existing C++ compiler and a minimal Bootstrap entry point.

**Stage 1**

Build Cova with the Cova produced by Stage 0.

**Stage 2**

Build again with the Cova produced by Stage 1, and run tests and consistency checks.

## 16.3 Bootstrap Constraints

- Do not depend on a complete Cova Registry.
- Do not depend on online package downloads.
- Do not depend on a Cova that has not yet been built.
- Minimise third-party dependencies.
- State the initial compiler version requirements explicitly.
- Provide Bootstrap approaches suitable for platforms such as Linux and Windows.

## 16.4 C++26 Usage Policy

The Cova core targets C++26.

However, it must distinguish:

- C++26 language features.
- C++26 standard library capabilities.
- Experimental compiler support.
- Features required for Bootstrap.
- Optional optimisation features.

A compiler must not be considered to satisfy all Cova build requirements merely because it accepts `-std=c++26`.

Advanced capabilities such as static reflection and Modules should be adopted in stages according to actual toolchain support.

---

# 17. Software Architecture

## 17.1 Module Structure

```text
cova/
├── bootstrap/
├── src/
│   ├── core/
│   │   ├── diagnostics/
│   │   ├── filesystem/
│   │   ├── process/
│   │   └── hashing/
│   │
│   ├── manifest/
│   ├── workspace/
│   │
│   ├── platform/
│   │   ├── target/
│   │   ├── triple/
│   │   ├── abi/
│   │   └── constraints/
│   │
│   ├── toolchain/
│   │   ├── discovery/
│   │   ├── resolver/
│   │   ├── registry/
│   │   ├── probe/
│   │   └── providers/
│   │
│   ├── sysroot/
│   │   ├── manager/
│   │   ├── manifest/
│   │   ├── store/
│   │   └── verifier/
│   │
│   ├── package/
│   │   ├── resolver/
│   │   ├── fetcher/
│   │   ├── builder/
│   │   └── lockfile/
│   │
│   ├── build/
│   │   ├── graph/
│   │   ├── action/
│   │   ├── scheduler/
│   │   ├── executor/
│   │   └── cache/
│   │
│   ├── formatter/
│   ├── testing/
│   ├── bootstrap/
│   └── cli/
│
├── tests/
├── examples/
├── docs/
└── cova.toml
```

## 17.2 Architecture Boundaries

Must guarantee:

- The Target Model does not depend on the CLI.
- The Toolchain Resolver does not depend on the Build Executor.
- The Sysroot Manager does not depend on a specific project.
- The Package Resolver does not manipulate compiler command lines directly.
- The Build Graph uses a unified Action model.
- Compiler Providers are responsible for generating concrete commands.
- The CLI is responsible only for argument parsing, workflow orchestration and user interaction.

## 17.3 Core Data Models

The following are recommended:

```cpp
TargetSpec
TargetConstraints

ToolchainDescriptor
ToolchainCapabilities
ResolvedToolchain

SysrootManifest
ResolvedSysroot

CompilationEnvironment

PackageManifest
DependencyGraph
PackageLock

BuildAction
BuildGraph
BuildArtifact
BuildResult
```

`CompilationEnvironment` must be frozen before entering the build execution phase.

---

# 18. Security Requirements

## 18.1 Download Security

- Trust only explicitly configured Registries by default.
- Verify the hash of downloaded content.
- Support future extension with trusted signature mechanisms.
- Forbid unsafe archive path traversal.
- Prevent archive files from overwriting files outside the managed directory.
- A failed download must not leave an installation that could be mistaken for valid.
- The installation process should support atomic commits.

## 18.2 Build Script Security

Build scripts of third-party dependencies may execute arbitrary code.

Cova must clearly distinguish:

- Purely declarative builds.
- Trusted build recipes.
- Execution of external build scripts.

In the long term, support build sandboxes, filesystem access restrictions and network access restrictions.

Initially, Cova must not claim that third-party builds are completely safe without actual isolation capabilities.

## 18.3 System Isolation

- Do not modify system compilers.
- Do not overwrite system SDKs.
- Do not automatically modify the global PATH.
- Do not use administrator privileges for ordinary toolchain installation.
- Do not delete system files without user authorisation.

---

# 19. Non-Functional Requirements

## 19.1 Performance

Initial targets:

- CLI cold start target below 100 ms, subject to the test platform.
- Manifest parsing target below 50 ms for small projects.
- Incremental builds with no changes should avoid actual compilation and linking.
- Support multi-core parallel builds.
- The download cache should avoid fetching the same content repeatedly.

Performance metrics require benchmarks with fixed hardware, operating system, filesystem and project; they must not be compared directly across environments.

## 19.2 Stability

- A build failure must not corrupt existing valid toolchains.
- An interrupted download must not leave a valid status marker.
- Lockfile writes should use a safe replacement mechanism.
- Concurrent builds must not cause cache data races.
- An incorrect cache hit is a high-priority correctness defect.

## 19.3 Observability

Support:

```bash
cova build --verbose
cova toolchain explain
cova doctor
cova explain
```

Long-term support:

- Structured JSON output.
- Build timing statistics.
- Cache hit rate.
- Dependency graph export.
- Build trace information.

## 19.4 IDE Integration

Prioritise support for:

```text
compile_commands.json
```

Support later:

- VS Code.
- clangd.
- Visual Studio.
- CLion.
- Other development environments based on the Language Server Protocol.

---

# 20. Platform Support Plan

## 20.1 MVP Platform

The first combination to validate in depth:

**Build Host: Linux x86-64**

**Targets:**

- Linux x86-64 GNU.
- Linux AArch64 musl.

Prefer Clang/LLVM as the compiler.

This validates native builds, genuine cross-compilation, Sysroot resolution and mixed-source toolchains at the same time.

## 20.2 Follow-up Platforms

Phase two:

- Windows x86-64 GNU.
- WASI wasm32.
- Linux AArch64 GNU.
- Native Windows builds.

Phase three:

- Windows MSVC.
- Native macOS builds.
- Android NDK.
- RISC-V.
- Apple platform SDKs.
- Embedded bare metal.

Platform support must clearly distinguish:

- Can generate target code.
- Can complete target linking.
- Can run target programs.
- Can execute target tests.
- Can publish deployable artifacts.

These five capabilities must not be conflated.

---

# 21. Version Roadmap

## Milestone 0 — Architecture Foundation

**Goal: establish a verifiable cross-platform core model.**

Deliverables:

- Target Triple Parser.
- Target Constraints.
- Toolchain Descriptor.
- System toolchain discovery.
- Toolchain Capability Probe.
- Resolved Compilation Environment.
- Structured diagnostic system.

Acceptance criteria:

- Can discover system Clang/GCC.
- Can identify toolchain versions.
- Can determine basic Target compatibility.
- Can output a complete resolution report.
- The Resolver can be tested independently of an actual project build.

## Milestone 1 — Cova Build MVP

**Goal: form a minimally usable native build tool.**

Deliverables:

- `cova new`
- `cova init`
- `cova build`
- `cova run`
- `cova clean`
- `cova fmt`
- TOML Manifest.
- Build Graph.
- Ninja execution backend.
- Basic incremental builds.
- clang-format integration.

Acceptance criteria:

- Can create and build a C++ project.
- Can build executables and static libraries.
- Modifying a header correctly triggers recompilation.
- Does not perform unnecessary compilation when nothing has changed.
- Clean does not delete global shared resources.

## Milestone 2 — Cross-Compilation MVP

**Goal: implement genuine automated cross-compilation.**

Deliverables:

- Sysroot Manager.
- Managed toolchain installation.
- System-first Resolver.
- Mixed-source toolchains.
- AArch64 Linux musl Target.
- Target-specific build output.
- Toolchain integrity verification.
- `--offline` and `--no-install`.

Acceptance criteria:

- Build AArch64 Linux programs from Linux x86-64.
- Do not re-download when a compatible Clang already exists on the system.
- Can install a managed Sysroot when it is missing.
- Refuse incorrect fallback when an incompatible version is specified.
- Verify artifact architecture with target file inspection tools.
- Complete runtime verification on an available Runner or target device.

## Milestone 3 — Package Management

**Goal: complete a Cargo-style dependency management loop.**

Deliverables:

- `cova add`
- `cova remove`
- `cova fetch`
- `cova update`
- `cova tree`
- Local and Git dependencies.
- Initial Registry Provider.
- Dependency Lockfile.
- Target-specific dependency builds.
- Dependency binary cache.

Acceptance criteria:

- Can add and build a real third-party C++ library.
- Can resolve transitive dependencies.
- Can lock source versions.
- Can rebuild target dependencies for AArch64.
- Does not incorrectly link Host-architecture libraries.
- Offline mode can use cached dependencies.

## Milestone 4 — Workspace & Self-hosting

**Goal: support real medium-sized projects and Cova self-hosting.**

Deliverables:

- Workspace.
- Test workflow.
- Multi-Package builds.
- Bootstrap Stage 0.
- Cova self-build.
- Toolchain Lockfile.
- More complete build diagnostics.

Acceptance criteria:

- Cova can build itself.
- Stage 0 and the self-hosting stages can be run repeatedly.
- Multi-Package projects can share dependencies.
- CI can complete builds using a locked environment.

## Milestone 5 — Ecosystem Expansion

**Goal: evolve from a usable tool into cross-platform development infrastructure.**

Planning:

- Windows/MSVC.
- WASI.
- Android NDK.
- CMake project adaptation.
- Remote builds.
- Remote caching.
- C++ Modules.
- Package Publishing.
- Registry services.
- Advanced reproducible builds.

---

# 22. Quality Assurance and Test Plan

## 22.1 Unit Tests

Coverage:

- Target parsing.
- Version constraints.
- Toolchain candidate filtering.
- ABI compatibility determination.
- Manifest parsing.
- Lockfile serialisation.
- Build graph topological sorting.
- Cache key computation.

## 22.2 Integration Tests

Coverage:

- System compiler discovery.
- Real compilation and linking.
- Sysroot installation.
- Dependency fetching.
- Cross-platform builds.
- Incremental builds.
- Formatting.
- Clean safety.

## 22.3 Fault Injection Tests

Must simulate:

- Interrupted downloads.
- Registry unavailable.
- Hash verification failure.
- Missing compiler.
- Version mismatch.
- Incomplete Sysroot.
- Insufficient disk space.
- Concurrent installation of the same toolchain.
- Cache corruption.
- Filesystem permission errors.
- Unsafe archive files.
- Inability to execute target platform programs.

## 22.4 Cross-Compilation Acceptance

At minimum, include:

1. Target architecture verification.
2. Target ABI verification.
3. Successful linking verification.
4. Runtime dependency checks.
5. Execution on a Runner or a real target device.
6. Cross-compilation of third-party dependencies.
7. Mixed-platform builds involving Host code generators.

---

# 23. Success Metrics

## 23.1 Developer Experience Metrics

- Creating a simple new project does not require writing build scripts by hand.
- A typical project can go from creation to running in no more than three core commands.
- Cross-compilation does not require users to manually assemble compiler arguments.
- When toolchain selection fails, provide a clear reason and actionable remediation advice.

## 23.2 Correctness Metrics

- In covered tests, 100% of cross-platform build artifacts have the correct architecture.
- Incorrect cache hits are not permitted in covered tests.
- Host/Target library mixing is not permitted in covered tests.
- Out-of-bounds cleanup is not permitted in covered tests.
- All managed downloads are integrity-verified.

## 23.3 Performance Metrics

Track in a fixed benchmark project and test environment:

- CLI startup time.
- Full build time.
- No-change incremental build time.
- Build graph generation time.
- Toolchain resolution time.
- Cache hit rate.
- Duplicate download avoidance rate.

## 23.4 Ecosystem Metrics

Track later:

- Number of supported platform combinations.
- Number of compatible third-party packages.
- Number of real open-source projects that build successfully.
- Toolchain installation success rate.
- CI environment reproduction success rate.
- CMake project migration success rate.

---

# 24. Risks and Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Incomplete C++26 compiler support | High | Establish a Bootstrap feature baseline and probe each item |
| ABI differences between compilers | High | ABI constraints, toolchain fingerprints and compatibility tests |
| Sysroot distribution and licensing restrictions | High | Trusted sources, licence metadata and platform-specific Providers |
| Complexity of building third-party dependencies | High | Support mature recipe ecosystems in stages |
| Differences in C++ Modules builds | High | Implement independently later; do not block the MVP |
| Incorrect reuse of build caches | High | Full input tracking and a conservative caching strategy |
| Changes to system toolchains | Medium-high | Fingerprint checks and Locked mode |
| Supply-chain risk of automatic downloads | High | Content verification, source restrictions and safe unpacking |
| Complexity of Windows/MSVC | Medium-high | Independent Provider and phased support |
| Scope too large | High | Strict acceptance by Milestone |
| Bootstrap dependency cycle | High | Minimal Stage 0 and independent Bootstrap |
| Competition with existing tools | Medium-high | Focus on cross-compilation, zero configuration and environment management |

---

# 25. Open Design Questions

The following must be decided before the architecture is formally frozen.

**OD-001: Build Execution Backend**

Should the first version commit to Ninja, or develop a native Executor in parallel?

Recommendation: use Ninja first and retain the independent Build Graph IR.

**OD-002: Automatic Toolchain Downloads**

Should automatic downloads require interactive confirmation?

Recommendation: interactive environments may confirm based on download size and trust policy; CI provides configuration to explicitly allow or forbid installation.

**OD-003: Dependency Registry**

Should we build our own public Registry immediately?

Recommendation: implement the Provider interface and lockfiles first, then decide the maintenance scope of the Registry.

**OD-004: CMake Compatibility**

How should the compatibility layer map a CMake Target's Includes, Compile Definitions and Link Dependencies?

Recommendation: design the adaptation layer independently so that CMake concepts do not contaminate Cova's core Build IR.

**OD-005: Toolchain Locking**

How should the System-first default policy be balanced against strict environment reproducibility?

Recommendation: distinguish Flexible and Locked modes, and define explicit rules for verifying system toolchain identity.

**OD-006: C++26 Feature Baseline**

Which C++26 features are hard requirements for building Cova?

Recommendation: define them through a compiler capability matrix rather than relying only on compiler version numbers.

**OD-007: Remote Execution Model**

How should the relationship between the execution platform and the target platform be expressed in the future?

Recommendation: reserve independent platform fields in Build Actions starting with the MVP.

---

# 26. Product Release Criteria

## 26.1 Alpha

Must have:

- Project initialisation.
- Manifest parsing.
- Native C++ builds.
- Basic incremental builds.
- Clean.
- Format.
- System toolchain discovery.

Interfaces and the Manifest Schema may still change.

## 26.2 Beta

Must have:

- Verifiable cross-compilation.
- Sysroot Manager.
- Managed toolchain installation.
- System-first policy.
- Basic dependency management.
- Lockfile.
- Basic Workspace.
- CI integration tests.

Beta should begin providing Manifest and CLI compatibility commitments.

## 26.3 Stable 1.0

It is recommended that at least the following be satisfied:

- Declared supported platform combinations pass the full CI.
- Real projects can complete dependency installation, building and testing.
- Cova can self-host.
- The Toolchain Resolver has complete error diagnostics.
- Toolchain and Sysroot installation have integrity guarantees.
- Incremental build correctness has been systematically verified.
- Lockfile semantics are stable.
- The main CLI and Manifest Schema are stable.
- User documentation, migration guides and a version compatibility policy are available.

---

# 27. Final Product Definition

**Cova is a modern C/C++ build and development environment management system with genuine cross-compilation at its core, implemented in C++26 and capable of self-hosting.**

It provides a Cargo-style project workflow, but is specifically designed for the toolchain, ABI, Sysroot, standard library and multi-platform build requirements of C/C++.

Cova's default behaviour is:

1. Prefer a system environment that satisfies the constraints.
2. When the system environment does not satisfy them, prefer reusing components already managed by Cova.
3. When components are still missing, install the required components from trusted sources.
4. Do not modify system toolchains or SDKs without authorisation.
5. Generate a complete compilation environment through the independent Target Model, Toolchain Resolver and Sysroot Manager.
6. Use a unified Build Graph to execute native builds and cross-compilation.
7. Manage dependencies and the build environment through the Manifest, Lockfile and content verification.
8. Provide creation, build, run, test, format, clean and release workflows through a unified CLI.

**Core value proposition:**

*One project. One workflow. Multiple platforms.*

One project, one workflow, for multiple target platforms.

---

**End of document — Cova PRD v1.0**
