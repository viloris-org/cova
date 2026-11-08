#!/usr/bin/env bash
# Cova — Fedora/RHEL/openSUSE package (.rpm).
#
# Produces dist/cova-<version>-1.<arch>.rpm. Uses rpmbuild with a private
# top-level directory so it never touches ~/rpmbuild.
#
# COVA_RPM_ROOT points at a prefix containing an extracted rpm-tools install
# (usr/bin/rpmbuild plus usr/lib/rpm). On a distro with rpmbuild already on
# PATH it is unnecessary; it exists so the package can be built and verified on
# a host where installing rpm would require root.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

resolve_defaults

rpm_args=()
rpmconfigdir=""
if [[ -n "${COVA_RPM_ROOT:-}" ]]; then
  export LD_LIBRARY_PATH="$COVA_RPM_ROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  rpmbuild_bin="$COVA_RPM_ROOT/usr/bin/rpmbuild"
  rpmconfigdir="$COVA_RPM_ROOT/usr/lib/rpm"
  [[ -x "$rpmbuild_bin" ]] || die "no rpmbuild under COVA_RPM_ROOT=$COVA_RPM_ROOT"
  rpm_args+=(--rcfile "$rpmconfigdir/rpmrc")
  rpm_args+=(--macros "$rpmconfigdir/platform/x86_64-linux/macros")
  rpm_args+=(--macros "$rpmconfigdir/macros")
  rpm_args+=(--define "_rpmconfigdir $rpmconfigdir")
else
  require rpmbuild
  rpmbuild_bin="$(command -v rpmbuild)"
fi

case "$TARGET_ARCH" in
  x86_64|amd64) RPM_ARCH=x86_64 ;;
  aarch64|arm64) RPM_ARCH=aarch64 ;;
  armv7l|armhf) RPM_ARCH=armv7hl ;;
  *) die "unsupported architecture for an .rpm: $TARGET_ARCH" ;;
esac

topdir="$(mktemp -d)"
trap 'rm -rf "$topdir"' EXIT
mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS,db}

# The payload is staged exactly as it will be installed; the spec copies it into
# the buildroot verbatim.
stage_tree "$topdir/payload"

# RPM treats `-` in a version as a release separator, so a pre-release tag uses
# `~`, which sorts before the final release (0.1.0~beta.1 < 0.1.0). Remaining
# semver identifiers join with `.`.
if [[ "$COVA_VERSION" == *-* ]]; then
  rpm_version="${COVA_VERSION%%-*}~${COVA_VERSION#*-}"
  rpm_version="${rpm_version//-/.}"
else
  rpm_version="$COVA_VERSION"
fi

cat > "$topdir/SPECS/cova.spec" <<EOF
Name:           cova
Version:        $rpm_version
Release:        1
Summary:        Cross-compilation-first build system for C and C++
License:        Apache-2.0
URL:            https://github.com/viloris-org/cova
BuildArch:      $RPM_ARCH
AutoReqProv:    no
%global debug_package %{nil}
%global _build_id_links none
%global __os_install_post %{nil}

%description
Cova is a build system, package manager and toolchain manager for C and C++,
inspired by Cargo. It treats cross compilation as a first-class command rather
than a configuration project.

%install
mkdir -p %{buildroot}
cp -a %{_sourcedir}/. %{buildroot}/

%files
%{_bindir}/cova
%doc %{_docdir}/cova/LICENSE
%doc %{_docdir}/cova/NOTICE

%changelog
* $(LC_ALL=C date '+%a %b %d %Y') The Cova Authors <rownix101@gmail.com> - $rpm_version-1
- Cova $COVA_VERSION
EOF

"$rpmbuild_bin" --quiet "${rpm_args[@]}" \
  --define "_topdir $topdir" \
  --define "_sourcedir $topdir/payload" \
  --define "_dbpath $topdir/db" \
  --define "_build_name_fmt %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
  -bb "$topdir/SPECS/cova.spec" >/dev/null

built="$(find "$topdir/RPMS" -name '*.rpm' -type f | head -1)"
[[ -n "$built" ]] || die "rpmbuild produced no package"

# The Version tag keeps `~` (it sorts correctly), but GitHub rewrites `~` to `.`
# in release asset names, which would invalidate the published checksum. Spell
# the *file name* with dots so the asset name and its checksum agree.
filename="$(basename "$built" | tr '~' '.')"
output="$OUT_DIR/$filename"
cp "$built" "$output"
note "built $output"
