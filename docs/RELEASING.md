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
| `windows-latest` (MSYS2) | Windows x86_64 MinGW | `.zip` |

Linux binaries are linked statically (`-static`), which is why one package runs
across distributions. The macOS binaries are built per-architecture and joined
with `lipo`; they link `libstdc++` statically but still use the system libc.
The Windows binary is built by MSYS2's MinGW-w64 GCC with `-static`, so it needs
no `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll` beside it.
The archive is not code-signed.

## Version spelling per package format

Each format has its own rules, and the pipeline maps the version rather than
inventing a different one per package:

| Format | `0.1.0-beta-1` becomes | Why |
|---|---|---|
| Tag / tarball / `.deb` | `0.1.0-beta-1` | Plain semver |
| `.rpm` | `0.1.0~beta.1` | `-` separates version from release; `~` sorts before the final release |
| pacman | `0.1.0beta1` | `-` separates pkgver from pkgrel |

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
Windows packaging uses `zip` and runs under MSYS2 (`build-windows.sh`); it
produces the same `cova-<version>-windows-<arch>.zip` layout as the release job.

## Signing

Neither the macOS `.pkg` nor the `.dmg` is signed with an Apple Developer ID,
and the binaries inside are ad-hoc signed only. Gatekeeper therefore asks for a
one-time override, which the README inside the image explains. Signing is a
separate step that needs a Developer ID certificate and is not automated here.

## Installing from a release

`install.sh` in the repository root is the one-line installer for Linux and
macOS. It resolves the newest beta, verifies the checksum and installs the
binary. Because `latest` in the GitHub API ignores pre-releases, the script
lists releases and filters for one instead.

Windows has no shell installer: download the `.zip` asset, extract it, and add
the extracted directory to `PATH`.
