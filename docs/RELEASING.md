# Releasing Cova

Every release so far is a beta: there is no stable channel and no promise that
a beta keeps compatibility with the next one. The manifest schema and the main
CLI are the parts expected to settle first (PRD §26.2).

## Cutting a release

1. **Bump the version.** It lives in two places and both must agree:

   - `cova.toml` → `[package] version`
   - `src/cli/parse.cpp` → `kVersionString`

   For a pre-release use the semver form, e.g. `0.1.0-beta-1`.

2. **Verify the tree.**

   ```bash
   ./bootstrap/build.sh --test
   ```

   This is a clean-room build with only a system compiler, Ninja and Python. It
   runs the full unit and end-to-end suite.

3. **Commit, tag, push.**

   ```bash
   git commit -am "Release 0.1.0-beta-1"
   git tag -a v0.1.0-beta-1 -m "Cova 0.1.0-beta-1"
   git push origin main
   git push origin v0.1.0-beta-1
   ```

4. The tag runs `.github/workflows/release.yml`. It builds on native runners,
   packages, and publishes a GitHub **pre-release** with `SHA256SUMS` attached.
   The job can also be re-run by hand:

   ```bash
   gh workflow run release.yml -f tag=v0.1.0-beta-1
   ```

## What the pipeline builds

| Runner | Target | Assets |
|---|---|---|
| `ubuntu-24.04` | Linux x86_64 | `.deb`, `.rpm`, pacman `.pkg.tar.zst`, `.tar.gz` |
| `ubuntu-24.04-arm` | Linux aarch64 | `.deb`, `.rpm`, pacman `.pkg.tar.zst`, `.tar.gz` |
| `macos-15-intel` + `macos-15` | macOS universal | `.pkg`, `.dmg`, `.tar.gz` |
| `windows-latest` (MSYS2 UCRT64) | Windows x86_64 MinGW | `.msi` |
| `windows-11-arm` (MSYS2 CLANGARM64) | Windows Arm64 (WoA) | `.msi` |

Linux binaries are linked statically (`-static`), which is why one package runs
across distributions. The macOS binaries are built per-architecture and joined
with `lipo`; they link `libstdc++` statically but still use the system libc.
The x86_64 Windows binary is built by MSYS2's UCRT64 MinGW-w64 GCC and the
Arm64 binary by the CLANGARM64 Clang, both with `-static`, so neither needs
`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `libc++.dll`
or `libunwind.dll` beside it. The MSIs are not code-signed.

## Version spelling per package format

Each format has its own rules, and the pipeline maps the version rather than
inventing a different one per package:

| Format | `0.1.0-beta-1` becomes | Why |
|---|---|---|
| Tag / tarball / `.deb` | `0.1.0-beta-1` | Plain semver |
| `.rpm` | `0.1.0~beta.1` | `-` separates version from release; `~` sorts before the final release |
| pacman | `0.1.0beta1` | `-` separates pkgver from pkgrel |
| `.msi` | `0.1.0` | Windows Installer's ProductVersion is numeric; the beta suffix has no representation. `MajorUpgrade` allows same-version upgrades, so successive betas of `0.1.0` replace each other |

## Building packages locally

`scripts/package/build-all.sh` builds every family the host supports and writes
`dist/` plus `dist/SHA256SUMS`. Missing tools are reported and skipped, not
failed, so the same script works on a contributor machine:

```bash
./bootstrap/build.sh
COVA_BINARY=build/bootstrap/bin/cova bash scripts/package/build-all.sh
```

On a host without `dpkg-deb` or `rpmbuild` (and no root to install them), point
`COVA_RPM_ROOT` at an extracted `rpm-tools` tree; the script then uses its
private `rpmbuild` and config.

macOS packaging uses `pkgbuild` and `hdiutil`, so those steps only run on macOS.
Windows packaging runs under MSYS2 and needs the WiX CLI on `PATH`; without it
the step is reported and skipped. Install WiX as a .NET global tool, then run
the same script the release job uses:

```powershell
dotnet tool install --global wix --version 5.0.2
bash scripts/package/build-msi.sh
```

It produces the same `cova-<version>-windows-<arch>.msi` layout as the release
job. The release job runs `build-all.sh`, which selects `build-msi.sh` on
MSYS2.

## Signing

Neither the macOS `.pkg` nor the `.dmg` is signed with an Apple Developer ID,
and the binaries inside are ad-hoc signed only. Gatekeeper therefore asks for a
one-time override, which the README inside the image explains. Signing is a
separate step that needs a Developer ID certificate and is not automated here.

The Windows MSIs are not Authenticode-signed either, because the project has no
code-signing certificate. SmartScreen warns about an unknown publisher; a
release that adds signing should sign both the MSI and the `cova.exe` inside it.

## Installing from a release

`install.sh` in the repository root is the one-line installer for Linux and
macOS. It resolves the newest beta, verifies the checksum and installs the
binary. Because `latest` in the GitHub API ignores pre-releases, the script
lists releases and filters for one instead.

Windows has no shell installer: download the `.msi` for the machine's
architecture and run it. The per-machine installer puts `cova.exe` under
`Program Files\Cova` and appends that directory to the system `PATH`; it can
also be driven from a shell with `msiexec /i cova-<version>-windows-<arch>.msi`.
