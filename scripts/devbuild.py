#!/usr/bin/env python3
"""Cova development build driver (Stage 0 tooling).

Cova's own build must not require a third-party build system, so this script
generates a Ninja file and invokes Ninja. It supports per-module builds so that
a module can be compiled and unit-tested without the rest of the tree linking.

Usage:
    python3 scripts/devbuild.py            # build everything + all tests
    python3 scripts/devbuild.py build      # build the cova binary only
    python3 scripts/devbuild.py test       # run every unit/integration test
    python3 scripts/devbuild.py module platform          # build one module archive
    python3 scripts/devbuild.py test-module platform     # build+run one module's tests
    python3 scripts/devbuild.py clean
    python3 scripts/devbuild.py list
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Allows each agent to build into its own directory, avoiding Ninja lock
# contention when several agents compile in parallel:
#   COVA_BUILD_DIR=build/platform python3 scripts/devbuild.py module platform
BUILD = Path(os.environ.get("COVA_BUILD_DIR") or (ROOT / "build"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
NINJA_FILE = BUILD / "build.ninja"

CXX = os.environ.get("COVA_CXX", "g++")
CXXFLAGS = [
    "-std=c++26",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wno-unused-parameter",
    "-fdiagnostics-color=always",
    f"-I{ROOT / 'src'}",
]

# Module name -> source directory (relative to ROOT).
MODULES = {
    "core": ["src/core"],
    "testing": ["src/testing"],
    "platform": ["src/platform"],
    "toolchain": ["src/toolchain"],
    "sysroot": ["src/sysroot"],
    "manifest": ["src/manifest"],
    "workspace": ["src/workspace"],
    "build": ["src/build"],
    "formatter": ["src/formatter"],
    "package": ["src/package"],
    "cli": ["src/cli"],
    "app": ["src"],  # src/main.cpp only
}

# Static dependency edges between modules (used for linking).
DEPS = {
    "core": [],
    "testing": ["core"],
    "platform": ["core"],
    "toolchain": ["core", "platform"],
    "sysroot": ["core", "platform"],
    "manifest": ["core", "platform", "toolchain"],
    "workspace": ["core", "platform", "manifest"],
    "build": ["core", "platform", "toolchain", "manifest"],
    "formatter": ["core", "manifest"],
    "package": ["core", "platform", "toolchain", "manifest", "workspace"],
    "cli": [
        "core",
        "platform",
        "toolchain",
        "sysroot",
        "manifest",
        "workspace",
        "build",
        "formatter",
        "package",
        "testing",
    ],
    "app": ["cli"],
}

# Test binary -> modules it must link. Test files are matched by filename prefix;
# archives are only pulled in for objects that resolve needed symbols, so a
# focused test does not drag in the whole tree.
TEST_REQUIRES = {
    # unit tests
    "core": ["core", "testing"],
    "testing": ["core", "testing"],
    "platform": ["core", "testing", "platform"],
    "toolchain": ["core", "testing", "platform", "toolchain"],
    "sysroot": ["core", "testing", "platform", "sysroot"],
    "manifest": ["core", "testing", "platform", "toolchain", "manifest"],
    "workspace": ["core", "testing", "platform", "manifest", "workspace"],
    "build": ["core", "testing", "platform", "toolchain", "manifest", "build"],
    "formatter": ["core", "testing", "manifest", "formatter"],
    "package": ["core", "testing", "platform", "manifest", "workspace", "package"],
    # cli parsing/UI only needs core, because the driver is never pulled in.
    "cli": ["core", "testing", "cli"],
    # end-to-end tests exercise the whole CLI, so they link everything.
    "integration": [
        "core",
        "testing",
        "platform",
        "toolchain",
        "sysroot",
        "manifest",
        "workspace",
        "build",
        "formatter",
        "package",
        "cli",
        "app",
    ],
}

# Longest-prefix matching, so `integration_cli_build_test` maps to integration.
_TEST_MODULE_ORDER = sorted(TEST_REQUIRES, key=len, reverse=True)


def sources_for(module: str) -> list[Path]:
    out: list[Path] = []
    for base in MODULES[module]:
        for cpp in sorted(Path(ROOT / base).rglob("*.cpp")):
            rel = cpp.relative_to(ROOT)
            if module == "app":
                # Only the top-level entry point belongs to the app module.
                if len(rel.parts) != 2:
                    continue
            elif len(rel.parts) > 2 and False:
                continue
            out.append(cpp)
    return out


def obj_path(src: Path) -> Path:
    return BUILD / "obj" / src.relative_to(ROOT).with_suffix(".o")


def lib_path(module: str) -> Path:
    return BUILD / "lib" / f"libcova_{module}.a"


def built_modules(modules: list[str]) -> list[str]:
    """Filters out modules that have no sources yet (in-progress tree)."""
    return [m for m in modules if sources_for(m)]


def closing_link_order(modules: list[str]) -> list[str]:
    seen: list[str] = []
    order: list[str] = []

    def visit(m: str) -> None:
        if m in seen:
            return
        seen.append(m)
        for dep in DEPS.get(m, []):
            visit(dep)
        order.append(m)

    for m in modules:
        visit(m)
    return order


def discover_tests() -> dict[str, Path]:
    tests: dict[str, Path] = {}
    for base in ["tests/unit", "tests/integration"]:
        d = ROOT / base
        if d.is_dir():
            for cpp in sorted(d.glob("*.cpp")):
                tests[cpp.stem] = cpp
    return tests


def test_module_of(name: str) -> str:
    base = name[: -len("_test")] if name.endswith("_test") else name
    # The end-to-end tests live under tests/integration but their filename
    # prefix does not contain "integration"; link them with the whole tree.
    if base.startswith("e2e_") or base.startswith("integration_"):
        return "integration"
    for module in _TEST_MODULE_ORDER:
        if base == module or base.startswith(module + "_"):
            return module
    return "core"


def render() -> str:
    lines: list[str] = []
    lines.append("# Generated by scripts/devbuild.py — do not edit.\n")
    lines.append(f"cxx = {CXX}\n")
    lines.append("cxxflags = " + " ".join(CXXFLAGS) + "\n")
    lines.append("rule cxx_compile\n")
    lines.append("  command = $cxx $cxxflags -MMD -MF $out.d -c $in -o $out\n")
    lines.append("  depfile = $out.d\n")
    lines.append("  deps = gcc\n")
    lines.append("  description = CXX $in\n")
    lines.append("rule archive\n")
    lines.append("  command = rm -f $out && ar rcs $out $in\n")
    lines.append("  description = AR $out\n")
    lines.append("rule link_exe\n")
    lines.append("  command = $cxx $cxxflags $in -o $out $ldflags -Wl,--start-group $libs -Wl,--end-group\n")
    lines.append("  description = LINK $out\n")

    for module in MODULES:
        srcs = sources_for(module)
        if not srcs:
            continue
        objs = [obj_path(s) for s in srcs]
        lines.append(f"# --- module {module}\n")
        for src, obj in zip(srcs, objs):
            lines.append(f"build {obj.relative_to(BUILD)}: cxx_compile {src}\n")
        lines.append(
            f"build {lib_path(module).relative_to(BUILD)}: archive "
            + " ".join(str(o.relative_to(BUILD)) for o in objs)
            + "\n"
        )

    # cova binary
    app_libs = built_modules(closing_link_order(DEPS["app"] + ["app"]))
    libs = " ".join(f"lib/libcova_{m}.a" for m in app_libs)
    lines.append("# --- cova binary\n")
    lines.append("build bin/cova: link_exe " + libs + "\n")
    lines.append("  ldflags = -pthread\n")
    lines.append(f"  libs = {libs}\n")

    # tests
    tests = discover_tests()
    test_targets: list[str] = []
    for name, src in tests.items():
        module = test_module_of(name)
        mods = built_modules(closing_link_order(TEST_REQUIRES.get(module, ["core", "testing"])))
        libs = " ".join(f"lib/libcova_{m}.a" for m in mods)
        target = f"bin/{name}"
        test_targets.append(target)
        lines.append(f"# --- test {name} (module {module})\n")
        lines.append(f"build {target}: link_exe {src} {libs}\n")
        lines.append(f"  ldflags = -pthread\n")
        lines.append(f"  libs = {libs}\n")

    defaults = ["bin/cova"] + test_targets
    lines.append("default " + " ".join(defaults) + "\n")
    return "".join(lines)


def write_ninja() -> dict[str, Path]:
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "obj").mkdir(exist_ok=True)
    (BUILD / "lib").mkdir(exist_ok=True)
    (BUILD / "bin").mkdir(exist_ok=True)
    # Create obj subdirectories mirroring src layout.
    for module in MODULES:
        for src in sources_for(module):
            obj_path(src).parent.mkdir(parents=True, exist_ok=True)
    NINJA_FILE.write_text(render())
    return discover_tests()


def need_ninja() -> None:
    if shutil.which("ninja") is None:
        sys.exit("error: ninja is required to build Cova (install ninja-build)")


def invoke(targets: list[str], jobs: int, verbose: bool) -> int:
    need_ninja()
    write_ninja()
    cmd = ["ninja", "-C", str(BUILD)]
    if jobs > 0:
        cmd += ["-j", str(jobs)]
    if verbose:
        cmd += ["-v"]
    cmd += targets
    return subprocess.call(cmd)


def cmd_list() -> int:
    print("modules:")
    for module in MODULES:
        print(f"  {module:10s} {len(sources_for(module))} sources")
    print("tests:")
    for name, src in discover_tests().items():
        print(f"  {name:24s} {src.relative_to(ROOT)}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Cova dev build driver")
    parser.add_argument(
        "action",
        nargs="?",
        default="all",
        choices=[
            "all",
            "build",
            "test",
            "module",
            "test-module",
            "clean",
            "list",
            "regen",
        ],
    )
    parser.add_argument("module", nargs="?")
    parser.add_argument("-j", "--jobs", type=int, default=0)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--filter", default="", help="test name filter")
    parser.add_argument("--cxx", default="", help="override compiler")
    args = parser.parse_args(argv)

    global CXX
    if args.cxx:
        CXX = args.cxx

    if args.action == "list":
        return cmd_list()
    if args.action == "clean":
        if BUILD.exists():
            shutil.rmtree(BUILD)
            print(f"removed {BUILD}")
        return 0
    if args.action == "regen":
        write_ninja()
        print(f"wrote {NINJA_FILE}")
        return 0

    if args.action == "module":
        if not args.module or args.module not in MODULES:
            sys.exit(f"error: unknown module {args.module!r}; see --action list")
        module = args.module
        libs = built_modules(closing_link_order([module] + DEPS[module]))
        targets = [f"lib/libcova_{m}.a" for m in libs]
        return invoke(targets, args.jobs, args.verbose)

    if args.action == "test-module":
        if not args.module or args.module not in MODULES:
            sys.exit(f"error: unknown module {args.module!r}")
        module = args.module
        tests = discover_tests()
        targets = []
        # Link only what this module's tests need (see TEST_REQUIRES), so a module
        # can be verified before its peers are implemented.
        requires = TEST_REQUIRES.get(module, ["core", "testing", module])
        rc = invoke([f"lib/libcova_{m}.a" for m in built_modules(closing_link_order(requires))],
                    args.jobs, args.verbose)
        if rc != 0:
            return rc
        for name in tests:
            if test_module_of(name) == module:
                rc = invoke([f"bin/{name}"], args.jobs, args.verbose)
                if rc != 0:
                    return rc
                cmd = [str(BUILD / "bin" / name)]
                if args.filter:
                    cmd.append(args.filter)
                rc = subprocess.call(cmd)
                if rc != 0:
                    return rc
                targets.append(name)
        if not targets:
            print(f"note: no tests registered for module {module}")
        return 0

    if args.action == "build":
        return invoke(["bin/cova"], args.jobs, args.verbose)

    if args.action == "test":
        rc = invoke(["bin/cova"], args.jobs, args.verbose)
        if rc != 0:
            return rc
        rc = invoke([f"bin/{name}" for name in discover_tests()], args.jobs, args.verbose)
        if rc != 0:
            return rc
        failures = 0
        for name in discover_tests():
            cmd = [str(BUILD / "bin" / name)]
            if args.filter:
                cmd.append(args.filter)
            print(f"\n=== {name} ===", flush=True)
            rc = subprocess.call(cmd)
            if rc != 0:
                failures += 1
                print(f"--- {name}: FAILED (exit {rc})", flush=True)
        if failures:
            print(f"\n{failures} test binary/binaries failed")
            return 1
        return 0

    # all
    rc = invoke(["bin/cova"] + [f"bin/{n}" for n in discover_tests()], args.jobs, args.verbose)
    if rc != 0:
        return rc
    print(f"built {BUILD / 'bin' / 'cova'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
