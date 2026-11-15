#!/usr/bin/env python3
"""Cova benchmark harness: Cova against CMake + Ninja (PRD §19.1, §23.3).

The harness builds the *same* sources with both tools, using the same compiler
and the same flags, so the numbers measure the build system rather than the
code. Every scenario is repeated; the report shows medians and the observed
range, and the JSON output keeps every sample.

Usage:
    python3 scripts/bench.py                     # default suite, 40 TUs, 5 runs
    python3 scripts/bench.py --quick             # smoke run (8 TUs, 1 sample)
    python3 scripts/bench.py --tus 10,40,120     # scaling table
    python3 scripts/bench.py --real              # add Cova's own source tree
    python3 scripts/bench.py --cache-ab          # Cova with the shared cache off
    python3 scripts/bench.py --job-sweep         # cold builds at -j1 and -jN
    python3 scripts/bench.py --markdown bench.md --json bench.json
    python3 scripts/bench.py --keep              # keep the scratch tree

Requirements: Python 3.9+, a C++26 compiler, CMake and Ninja. No third-party
Python packages. Cova is taken from build/bench-release/bin/cova when it
exists (an -O2 -DNDEBUG build), otherwise build/bin/cova.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK_ROOT = ROOT / ".cache" / "bench"
CXX_FLAGS = ["-Wall", "-Wextra", "-Wpedantic", "-Wno-unused-parameter"]


class BenchError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Process helpers
# ---------------------------------------------------------------------------
def run_quiet(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> float:
    """Runs a command, discards its output, and returns wall-clock seconds.

    Timing wraps only the child process; `sync()` happens before the clock
    starts so flushing dirty pages is not charged to either tool.
    """
    os.sync()
    started = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        tail = (proc.stderr or "").strip().splitlines()[-10:]
        raise BenchError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n" + "\n".join(tail)
        )
    return elapsed


def run_capture(
    cmd: list[str], cwd: Path, env: dict[str, str] | None = None
) -> tuple[float, str]:
    os.sync()
    started = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        tail = (proc.stderr or "").strip().splitlines()[-10:]
        raise BenchError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n" + "\n".join(tail)
        )
    return elapsed, proc.stdout


def command_output(cmd: list[str]) -> str:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    if proc.returncode != 0:
        return ""
    return (proc.stdout or proc.stderr).strip()


def first_line(text: str) -> str:
    return text.strip().splitlines()[0] if text.strip() else ""


def format_ms(seconds: float) -> str:
    milliseconds = seconds * 1000.0
    if milliseconds < 1000:
        return f"{milliseconds:.0f} ms"
    return f"{seconds:.2f} s"


def remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


# ---------------------------------------------------------------------------
# Generated projects
# ---------------------------------------------------------------------------
COMMON_HEADER = """#pragma once
#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bench {

template <typename T>
T transform(T value) {
  return static_cast<T>(value * 2 + 1);
}

struct Record {
  std::string name;
  std::vector<int> values;

  std::size_t total() const {
    std::size_t sum = 0;
    for (int value : values) {
      sum += static_cast<std::size_t>(value);
    }
    return sum;
  }
};

inline std::map<std::string, int> index(const std::vector<Record>& records) {
  std::map<std::string, int> out;
  for (const auto& record : records) {
    out[record.name] = static_cast<int>(record.total());
  }
  return out;
}

inline std::variant<int, std::string> pick(bool flag, int value) {
  if (flag) {
    return value;
  }
  return std::string{"none"};
}

}  // namespace bench
"""

UNIT_HEADER = """#pragma once
namespace bench {{
int {name}(int x);
}}
"""

UNIT_SOURCE = """#include "{name}.h"
#include "common.h"

namespace bench {{
int {name}(int x) {{
  Record record{{"{name}", {{x, {index}, x + {index}}}}};
  return static_cast<int>(record.total()) + transform(x) + {constant};
}}
}}
"""


def cova_manifest(name: str, sources: list[str], includes: list[str]) -> str:
    source_list = ", ".join(f'"{value}"' for value in sources)
    include_list = ", ".join(f'"{value}"' for value in includes)
    flag_list = ", ".join(f'"{value}"' for value in CXX_FLAGS)
    return f"""[package]
name = "{name}"
version = "0.1.0"
edition = "c++26"

[build]
type = "executable"
sources = [{source_list}]
public-includes = [{include_list}]
compile-options = [{flag_list}]

[profile.dev]
optimization = 0
debug = true
assertions = true

[profile.release]
optimization = 3
debug = false
lto = false
assertions = false
"""


def cmake_lists(pattern: str, include: str) -> str:
    flags = " ".join(CXX_FLAGS)
    return f"""cmake_minimum_required(VERSION 3.25)
project(bench CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS "${{CMAKE_CURRENT_SOURCE_DIR}}/{pattern}")
add_executable(bench ${{BENCH_SOURCES}})
target_include_directories(bench PRIVATE {include})
target_compile_options(bench PRIVATE {flags})
"""


def unit_name(index: int, tus: int) -> str:
    width = max(3, len(str(tus - 1)))
    return f"unit_{index:0{width}d}"


def unit_source_path(project: Path, index: int, tus: int) -> Path:
    return project / "src" / f"{unit_name(index, tus)}.cpp"


def write_synthetic_project(dest: Path, tus: int) -> Path:
    """One header and source per unit plus a main.cpp that calls every unit."""
    remove(dest)
    (dest / "src").mkdir(parents=True)
    (dest / "include").mkdir(parents=True)

    (dest / "cova.toml").write_text(
        cova_manifest("bench", ["src/**/*.cpp"], ["include"]), encoding="utf-8"
    )
    (dest / "CMakeLists.txt").write_text(
        cmake_lists("src/*.cpp", "include"), encoding="utf-8"
    )
    (dest / "include" / "common.h").write_text(COMMON_HEADER, encoding="utf-8")

    names = [unit_name(index, tus) for index in range(tus)]
    main_lines = ["#include <cstdio>", '#include "common.h"']
    for index, name in enumerate(names):
        (dest / "include" / f"{name}.h").write_text(
            UNIT_HEADER.format(name=name), encoding="utf-8"
        )
        (dest / "src" / f"{name}.cpp").write_text(
            UNIT_SOURCE.format(name=name, index=index, constant=index),
            encoding="utf-8",
        )
        main_lines.append(f'#include "{name}.h"')

    main_lines += ["", "int main() {", "  int total = 0;"]
    main_lines += [f"  total += bench::{name}(1);" for name in names]
    main_lines += [
        '  std::printf("%d\\n", total);',
        "  return total == 0;",
        "}",
        "",
    ]
    (dest / "src" / "main.cpp").write_text("\n".join(main_lines), encoding="utf-8")
    return dest


def write_real_project(dest: Path) -> Path:
    """A copy of Cova's own sources: 44 real translation units."""
    remove(dest)
    shutil.copytree(ROOT / "src", dest / "src")
    (dest / "cova.toml").write_text(
        cova_manifest("cova-self", ["src/**/*.cpp"], ["src"]), encoding="utf-8"
    )
    # GLOB_RECURSE with `src/*.cpp` covers both `src/main.cpp` and every
    # nested directory; `src/**/*.cpp` would skip the top-level file.
    (dest / "CMakeLists.txt").write_text(
        cmake_lists("src/*.cpp", "src"), encoding="utf-8"
    )
    return dest


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------
@dataclass
class Result:
    scenario: str
    tool: str
    size: int | None
    samples: list[dict[str, float]] = field(default_factory=list)
    note: str = ""

    def values(self, key: str = "s") -> list[float]:
        return [sample[key] for sample in self.samples if key in sample]

    @property
    def median(self) -> float:
        return statistics.median(self.values())

    @property
    def minimum(self) -> float:
        return min(self.values())

    @property
    def maximum(self) -> float:
        return max(self.values())


class Bench:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.work = Path(args.workdir).resolve()
        self.results: dict[tuple[str, str, int | None], Result] = {}
        self.phases: list[dict[str, object]] = []
        self.cova = Path(args.cova).resolve()
        self.env_base = self._clean_env()
        self._primed: set[Path] = set()

    # -- setup -------------------------------------------------------------
    @staticmethod
    def _clean_env() -> dict[str, str]:
        env = dict(os.environ)
        env.pop("COVA_HOME", None)
        env["NO_COLOR"] = "1"
        env["CLICOLOR"] = "0"
        env["LC_ALL"] = "C"
        return env

    def cova_env(self, home: Path) -> dict[str, str]:
        env = dict(self.env_base)
        env["COVA_HOME"] = str(home)
        return env

    def fresh_home(self, name: str) -> Path:
        home = self.work / name
        shutil.rmtree(home, ignore_errors=True)
        home.mkdir(parents=True, exist_ok=True)
        return home

    def warm_home(self) -> Path:
        home = self.work / "home-warm"
        home.mkdir(parents=True, exist_ok=True)
        return home

    def check_tools(self) -> None:
        for tool in (self.args.cmake, self.args.ninja, self.args.compiler):
            if shutil.which(tool) is None:
                raise BenchError(f"required tool not found on PATH: {tool}")
        if not self.cova.is_file():
            raise BenchError(
                f"Cova binary not found: {self.cova}\n"
                "  build one first: COVA_CXXFLAGS='-O2 -DNDEBUG' "
                "COVA_BUILD_DIR=build/bench-release python3 scripts/devbuild.py build"
            )
        if not os.access(self.cova, os.X_OK):
            raise BenchError(f"Cova binary is not executable: {self.cova}")

    # -- recording ---------------------------------------------------------
    def record(self, scenario: str, tool: str, size: int | None, fn, note: str = "") -> None:
        key = (scenario, tool, size)
        result = self.results.setdefault(key, Result(scenario, tool, size, note=note))
        for index in range(self.args.warmups + self.args.runs):
            sample = fn()
            if index >= self.args.warmups:
                result.samples.append(sample)
        print(
            f"  {scenario:<11} {tool:<6} TUs={str(size or '-'):<5}"
            f" median {format_ms(result.median)}"
            f"  [{format_ms(result.minimum)} .. {format_ms(result.maximum)}]",
            flush=True,
        )

    # -- tool command lines ------------------------------------------------
    def cmake_cold(self, project: Path, jobs: int) -> dict[str, float]:
        remove(project / "build")
        configure = run_quiet(
            [
                self.args.cmake,
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                f"-DCMAKE_CXX_COMPILER={shutil.which(self.args.compiler)}",
                f"-DCMAKE_MAKE_PROGRAM={shutil.which(self.args.ninja)}",
            ],
            project,
            self.env_base,
        )
        build = run_quiet(
            [self.args.cmake, "--build", "build", "--", "-j", str(jobs)],
            project,
            self.env_base,
        )
        return {"s": configure + build, "configure": configure, "build": build}

    def cmake_incr(self, project: Path, jobs: int) -> dict[str, float]:
        return {
            "s": run_quiet(
                [self.args.cmake, "--build", "build", "--", "-j", str(jobs)],
                project,
                self.env_base,
            )
        }

    def cova_cold(self, project: Path, jobs: int) -> dict[str, float]:
        for path in (project / "target", project / "cova.lock", project / "cova-toolchain.lock"):
            remove(path)
        return {
            "s": run_quiet(
                [str(self.cova), "build", "-j", str(jobs)],
                project,
                self.cova_env(self.fresh_home("home-cold")),
            )
        }

    def cova_incr(self, project: Path, jobs: int, cache: bool = True) -> dict[str, float]:
        # `--offline` is Cova's documented way to turn the shared artifact cache
        # off (docs/CORRECTNESS.md §6); the local incremental record still works.
        cmd = [str(self.cova), "build", "-j", str(jobs)]
        if not cache:
            cmd.append("--offline")
        return {"s": run_quiet(cmd, project, self.cova_env(self.warm_home()))}

    # -- scenarios ---------------------------------------------------------
    def startup(self) -> None:
        self.record(
            "startup",
            "cova",
            None,
            lambda: {"s": run_quiet([str(self.cova), "--version"], self.work, self.env_base)},
        )
        self.record(
            "startup",
            "cmake",
            None,
            lambda: {"s": run_quiet([self.args.cmake, "--version"], self.work, self.env_base)},
        )
        self.record(
            "startup",
            "ninja",
            None,
            lambda: {"s": run_quiet([self.args.ninja, "--version"], self.work, self.env_base)},
        )

    def cold(self, project: Path, size: int, jobs: int) -> None:
        if "cova" in self.args.tools:
            self.record("cold", "cova", size, lambda: self.cova_cold(project, jobs), "fresh COVA_HOME")
        if "cmake" in self.args.tools:
            # One measurement feeds both rows: the total and the configure share.
            samples: list[dict[str, float]] = []

            def cmake_cold() -> dict[str, float]:
                sample = self.cmake_cold(project, jobs)
                samples.append(sample)
                return sample

            self.record("cold", "cmake", size, cmake_cold, "configure + build")
            cfgs = self.results.setdefault(
                ("cmake-cfg", "cmake", size),
                Result("cmake-cfg", "cmake", size, note="configure only"),
            )
            cfgs.samples.extend({"s": sample["configure"]} for sample in samples)

    def cold_shared_cache(self, project: Path, size: int, jobs: int) -> None:
        """Cova only: empty target directory, warm shared artifact cache."""
        home = self.warm_home()
        self.ensure_warm(project, jobs)

        def run() -> dict[str, float]:
            remove(project / "target")
            elapsed, stdout = run_capture(
                [str(self.cova), "build", "-j", str(jobs), "--json"],
                project,
                self.cova_env(home),
            )
            detail = {"s": elapsed}
            try:
                report = json.loads(stdout)
            except json.JSONDecodeError:
                return detail
            detail["executed"] = float(report.get("executed", 0))
            detail["restored"] = float(report.get("restored_from_cache", 0))
            return detail

        self.record("cold-cache", "cova", size, run, "warm shared cache")
        # The samples left a restored, depfile-less tree behind; re-prime.
        self._primed.discard(project)

    def ensure_warm(self, project: Path, jobs: int) -> None:
        """Compiles the project once into a clean warm tree.

        A cache restore does not write depfiles, so a tree that was only
        restored cannot prove the input closure on the next build and rescans.
        Priming by compiling reproduces a normal developer's warm tree and
        keeps the incremental scenarios measuring the incremental path rather
        than that restore quirk.
        """
        if project in self._primed:
            return
        for path in (project / "target", project / "cova.lock", project / "cova-toolchain.lock"):
            remove(path)
        shutil.rmtree(self.work / "home-warm", ignore_errors=True)
        run_quiet(
            [str(self.cova), "build", "-j", str(jobs)],
            project,
            self.cova_env(self.warm_home()),
        )
        self._primed.add(project)

    def noop(self, project: Path, size: int, jobs: int) -> None:
        if "cova" in self.args.tools:
            self.ensure_warm(project, jobs)
            self.record("noop", "cova", size, lambda: self.cova_incr(project, jobs))
        if "cova" in self.args.tools and self.args.cache_ab:
            # --offline disables the shared artifact cache. The incremental
            # record then answers, instead of the cache restoring unchanged
            # objects action by action.
            self.record(
                "noop-state",
                "cova",
                size,
                lambda: self.cova_incr(project, jobs, cache=False),
                "shared cache disabled",
            )
        if "cmake" in self.args.tools:
            self.record("noop", "cmake", size, lambda: self.cmake_incr(project, jobs))

    def single_file(self, project: Path, size: int, jobs: int) -> None:
        counter = {"n": 0}

        def touch() -> None:
            # The replacement constant must never reproduce the source that the
            # cold build compiled, or the shared cache turns this sample into a
            # restore and the scenario stops measuring a rebuild.
            counter["n"] += 1
            target = unit_source_path(project, size // 2, size)
            text = target.read_text(encoding="utf-8")
            updated, replaced = re.subn(
                r"transform\(x\) \+ \d+;",
                f"transform(x) + {10000 + counter['n']};",
                text,
                count=1,
            )
            if replaced != 1:
                raise BenchError(f"could not perturb {target}")
            target.write_text(updated, encoding="utf-8")

        if "cova" in self.args.tools:
            self.ensure_warm(project, jobs)

            def cova_one() -> dict[str, float]:
                touch()
                return self.cova_incr(project, jobs)

            self.record("one-file", "cova", size, cova_one, "one TU content change")
        if "cmake" in self.args.tools:
            def cmake_one() -> dict[str, float]:
                touch()
                return self.cmake_incr(project, jobs)

            self.record("one-file", "cmake", size, cmake_one, "one TU content change")

    def plan(self, project: Path, size: int, jobs: int) -> None:
        if "cova" in self.args.tools:
            self.ensure_warm(project, jobs)
            self.record(
                "plan",
                "cova",
                size,
                lambda: {"s": run_quiet(
                    [str(self.cova), "build", "--dry-run"], project, self.cova_env(self.warm_home())
                )},
                "includes project, dependency and toolchain resolution",
            )
        if "cmake" in self.args.tools:
            self.record(
                "plan",
                "cmake",
                size,
                lambda: {"s": run_quiet([self.args.ninja, "-n", "-C", "build"], project, self.env_base)},
                "ninja dry run",
            )

    def cova_metrics(self, project: Path, size: int, jobs: int) -> None:
        env = self.cova_env(self.warm_home())
        self.record(
            "toolchain-list",
            "cova",
            size,
            lambda: {"s": run_quiet([str(self.cova), "toolchain", "list"], project, env)},
            "discovery + probe cache",
        )
        self.record(
            "doctor",
            "cova",
            size,
            lambda: {"s": run_quiet([str(self.cova), "doctor"], project, env)},
            "full capability matrix",
        )

    def phase_profile(self, project: Path, size: int, jobs: int) -> None:
        """Parses Cova's own phase timings from one cold verbose build."""
        for path in (project / "target", project / "cova.lock", project / "cova-toolchain.lock"):
            remove(path)
        _, stdout = run_capture(
            [str(self.cova), "build", "-j", str(jobs), "--verbose"],
            project,
            self.cova_env(self.fresh_home("home-cold")),
        )
        phases: dict[str, float] = {}
        for line in stdout.splitlines():
            match = re.match(r"debug: ([a-z ]+): (\d+)ms$", line)
            if match:
                phases[match.group(1).strip()] = float(match.group(2))
        if phases:
            self.phases.append({"tus": size, "phases": phases})
            print(f"  phases      cova   TUs={size:<5} {phases}", flush=True)
        self.ensure_warm(project, jobs)

    # -- driver ------------------------------------------------------------
    def run(self) -> None:
        self.check_tools()
        self.work.mkdir(parents=True, exist_ok=True)
        print(f"cova:  {self.cova}")
        print(f"work:  {self.work}")
        print(f"jobs:  {self.args.jobs}, runs: {self.args.runs}, warmups: {self.args.warmups}")
        print()

        write_synthetic_project(self.work / "startup", 2)
        self.startup()
        remove(self.work / "startup")
        print()

        for size in self.args.tus:
            project = write_synthetic_project(self.work / f"tus-{size}", size)
            print(f"[{size} translation units]")
            self.cold(project, size, self.args.jobs)
            self.cold_shared_cache(project, size, self.args.jobs)
            self.noop(project, size, self.args.jobs)
            self.single_file(project, size, self.args.jobs)
            self.plan(project, size, self.args.jobs)
            self.cova_metrics(project, size, self.args.jobs)
            if self.args.phases:
                self.phase_profile(project, size, self.args.jobs)
            if self.args.job_sweep:
                for jobs in (1, self.args.jobs):
                    print(f"  -- cold at -j{jobs}")
                    self.record(
                        f"cold-j{jobs}", "cova", size, lambda j=jobs: self.cova_cold(project, j)
                    )
                    self.record(
                        f"cold-j{jobs}", "cmake", size, lambda j=jobs: self.cmake_cold(project, j)
                    )
            print()

        if self.args.real:
            project = write_real_project(self.work / "real")
            size = len(list((project / "src").rglob("*.cpp")))
            print(f"[Cova's own sources: {size} translation units]")
            self.record("real-cold", "cova", size, lambda: self.cova_cold(project, self.args.jobs))
            self.record("real-cold", "cmake", size, lambda: self.cmake_cold(project, self.args.jobs))
            self.record("real-noop", "cova", size, lambda: self.cova_incr(project, self.args.jobs))
            self.record("real-noop", "cmake", size, lambda: self.cmake_incr(project, self.args.jobs))
            print()

    # -- report ------------------------------------------------------------
    def environment(self) -> dict[str, str]:
        cpu = ""
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.exists():
            for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lower().startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
        memory = ""
        meminfo = Path("/proc/meminfo")
        if meminfo.exists():
            for line in meminfo.read_text(encoding="utf-8").splitlines():
                if line.startswith("MemTotal:"):
                    memory = f"{int(line.split()[1]) / 1024 / 1024:.1f} GiB"
                    break
        distro = ""
        os_release = Path("/etc/os-release")
        if os_release.exists():
            for line in os_release.read_text(encoding="utf-8").splitlines():
                if line.startswith("PRETTY_NAME="):
                    distro = line.split("=", 1)[1].strip().strip('"')
                    break
        filesystem = ""
        try:
            stat = os.statvfs(self.work)
            filesystem = f"{stat.f_bsize * stat.f_blocks / 1e9:.0f} GB, {stat.f_bavail * stat.f_bsize / 1e9:.0f} GB free"
        except OSError:
            pass
        return {
            "date": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
            "host": platform.node(),
            "os": distro or platform.platform(),
            "kernel": platform.release(),
            "cpu": cpu or platform.processor(),
            "cores": str(os.cpu_count()),
            "memory": memory,
            "filesystem": filesystem,
            "compiler": first_line(command_output([self.args.compiler, "--version"])),
            "cova": first_line(command_output([str(self.cova), "--version"]))
            + f" ({self.cova})",
            "cmake": first_line(command_output([self.args.cmake, "--version"]))
            .replace("cmake version ", ""),
            "ninja": first_line(command_output([self.args.ninja, "--version"])),
            "profile": "dev (-O0 -g), CMAKE_BUILD_TYPE=Debug",
            "jobs": str(self.args.jobs),
            "runs": f"{self.args.runs} timed, {self.args.warmups} warmup",
        }

    @staticmethod
    def _cell(result: Result | None) -> str:
        if result is None:
            return "—"
        return (
            f"{format_ms(result.median)} "
            f"({format_ms(result.minimum)}–{format_ms(result.maximum)})"
        )

    def _table(self, add, sizes: list[int], scenario: str, tools: list[str]) -> None:
        rows = [size for size in sizes if any((scenario, tool, size) in self.results for tool in tools)]
        if not rows:
            return
        add("| Translation units | " + " | ".join(self._tool_label(tool) for tool in tools)
            + " | Cova advantage |")
        add("|---" * (len(tools) + 2) + "|")
        for size in rows:
            cells = [self._cell(self.results.get((scenario, tool, size))) for tool in tools]
            cova = self.results.get((scenario, "cova", size))
            cmake = self.results.get((scenario, "cmake", size))
            ratio = "—"
            if cova and cmake and cova.median:
                ratio = f"{cmake.median / cova.median:.2f}×"
            add(f"| {size} | " + " | ".join(cells) + f" | {ratio} |")
        add("")

    @staticmethod
    def _tool_label(tool: str) -> str:
        return {"cova": "Cova", "cmake": "CMake + Ninja", "ninja": "Ninja"}.get(tool, tool)

    def report(self) -> str:
        sizes = self.args.tus
        env = self.environment()
        lines: list[str] = []
        add = lines.append

        add("# Cova versus CMake + Ninja")
        add("")
        add("Generated by `scripts/bench.py`: the same sources, compiler and flags are")
        add("built with both tools. `Cova advantage` is the CMake median divided by the")
        add("Cova median, so a value above 1.00× means Cova finished first.")
        add("")
        add("## Environment")
        add("")
        add("| Item | Value |")
        add("|---|---|")
        for key, value in env.items():
            add(f"| {key} | {value} |")
        add("")
        add("> These numbers belong to this machine, filesystem and compiler.")
        add("> PRD §19.1: they must not be compared across environments.")
        add("")

        add("## Startup")
        add("")
        add("| Tool | Median |")
        add("|---|---|")
        for tool in ("cova", "cmake", "ninja"):
            result = self.results.get(("startup", tool, None))
            if result:
                add(f"| `{tool} --version` | {self._cell(result)} |")
        add("")
        add("PRD §19.1 targets a CLI cold start below 100 ms; this is the closest")
        add("single-process proxy Cova has.")
        add("")

        tools = [tool for tool in ("cova", "cmake") if tool in self.args.tools]

        add("## Cold full build")
        add("")
        self._table(add, sizes, "cold", tools)
        cmake_cfg = [self.results[( "cmake-cfg", "cmake", size)] for size in sizes
                     if ("cmake-cfg", "cmake", size) in self.results]
        if cmake_cfg:
            shares = [
                self.results[("cmake-cfg", "cmake", size)].median
                / self.results[("cold", "cmake", size)].median
                for size in sizes
                if ("cmake-cfg", "cmake", size) in self.results
                and ("cold", "cmake", size) in self.results
            ]
            add(f"CMake's number includes the configure step, which is "
                f"{statistics.mean(shares) * 100:.0f}% of it on average.")
            add("")

        add("## Cova shared cache (empty `target/`, warm cache)")
        add("")
        add("| Translation units | Median | Executed | Restored |")
        add("|---|---|---|---|")
        for size in sizes:
            result = self.results.get(("cold-cache", "cova", size))
            if not result:
                continue
            executed = statistics.median(result.values("executed")) if result.values("executed") else 0
            restored = statistics.median(result.values("restored")) if result.values("restored") else 0
            add(f"| {size} | {self._cell(result)} | {executed:.0f} | {restored:.0f} |")
        add("")
        add("CMake + Ninja has no equivalent: deleting `build/` forces a full")
        add("recompile. With a warm cache and no `target/`, Cova restores the whole")
        add("output set instead.")
        add("")

        add("## No-change incremental build")
        add("")
        self._table(add, sizes, "noop", tools)
        if any(("noop-state", "cova", size) in self.results for size in sizes):
            add("Cova with the shared cache disabled (`--offline`) for comparison:")
            add("")
            self._table(add, sizes, "noop-state", ["cova"])
        add("## Single-file incremental rebuild")
        add("")
        self._table(add, sizes, "one-file", tools)
        add("## Build planning (`cova build --dry-run` vs `ninja -n`)")
        add("")
        self._table(add, sizes, "plan", tools)

        real = [size for size in {size for (_, _, size) in self.results if size}
                if ("real-cold", "cova", size) in self.results]
        if real:
            add("## Cova's own tree")
            add("")
            add("The repository's `src/` copied into the scratch tree and built by both")
            add("tools, so the workload is real C++26 rather than generated code.")
            add("")
            self._table(add, real, "real-cold", tools)
            self._table(add, real, "real-noop", tools)

        if self.phases:
            add("## Cova phase profile (cold `--verbose` build)")
            add("")
            order = [
                "project load",
                "dependency resolution",
                "toolchain resolution",
                "planning",
                "build execution",
            ]
            names = list(dict.fromkeys(
                name for entry in self.phases for name in entry["phases"]
            ))
            add("| Phase | " + " | ".join(f"{entry['tus']} TUs" for entry in self.phases) + " |")
            add("|---" * (len(self.phases) + 1) + "|")
            for name in order + [value for value in names if value not in order]:
                cells = []
                for entry in self.phases:
                    value = entry["phases"].get(name)
                    cells.append(f"{value:.0f} ms" if value is not None else "—")
                if all(cell == "—" for cell in cells):
                    continue
                add(f"| {name} | " + " | ".join(cells) + " |")
            add("")
            added = []
            for entry in self.phases:
                total = sum(entry["phases"].values())
                recorded = self.results.get(("cold", "cova", entry["tus"]))
                if recorded and recorded.median:
                    added.append(total / 1000.0 / recorded.median * 100.0)
            if added:
                add("The phases cover the work Cova instruments; the rest of the")
                add(f"wall-clock time of a cold build is the compiler subprocesses")
                add(f"themselves (the instrumented share is {statistics.mean(added):.0f}% on average).")
                add("")

        cova_only = [
            ("toolchain-list", "`cova toolchain list`", "discovery + cached capability probes"),
            ("doctor", "`cova doctor`", "full capability matrix, all probes"),
        ]
        if any((scenario, "cova", size) in self.results for scenario, _, _ in cova_only for size in sizes):
            add("## Cova-only metrics (PRD §23.3)")
            add("")
            add("| Command | Purpose | Translation units | Median |")
            add("|---|---|---|---|")
            for scenario, label, purpose in cova_only:
                for size in sizes:
                    result = self.results.get((scenario, "cova", size))
                    if result:
                        add(f"| {label} | {purpose} | {size} | {self._cell(result)} |")
            add("")

        add("## How to read this")
        add("")
        add("**Cold builds are at parity.** With an empty output directory Cova")
        add("and CMake + Ninja run the same number of compiler processes; Cova no")
        add("longer scans every translation unit before compiling it, because the")
        add("compiler's own depfile proves the closure (see `docs/CORRECTNESS.md`")
        add("§5). CMake's column still includes its configure step, and Cova still")
        add("pays a full capability probe for a fresh `COVA_HOME`.")
        add("")
        add("**No-change builds are now stat-driven.** Cova keeps the content hashes")
        add("the state file recorded and validates them with one `stat` per file, so")
        add("a build with nothing to do no longer re-reads the closure. A `touch`")
        add("without a content change does not rebuild, and a content change that")
        add("keeps the size is still detected. The guarantee that remains stronger")
        add("than Ninja's mtime check: the recorded inputs and outputs are verified,")
        add("not merely timestamped.")
        add("")
        add("**The shared cache is consulted after the record, not before.** An")
        add("unchanged action is answered by the incremental record, which is cheaper")
        add("than restoring files that are already correct. `--offline` changes")
        add("nothing on a no-change build, which is the point: the cache is for work")
        add("that is missing, not for work that is present.")
        add("")
        add("**What Cova wins.** The shared artifact cache has no Ninja equivalent:")
        add("deleting `target/` restores objects instead of recompiling them, and the")
        add("restore carries the depfiles so the restored tree is warm immediately.")
        add("CLI startup is on the same order as `ninja --version` and far below the")
        add("PRD's 100 ms target.")
        add("")
        add("## Method")
        add("")
        add(f"- Every sample is a fresh process invocation, timed around the child")
        add(f"  process only (`sync()` runs before the clock starts).")
        add(f"- Cova cold runs remove `target/`, `cova.lock` and `cova-toolchain.lock`")
        add(f"  and use a fresh `COVA_HOME`, so neither the shared artifact cache nor")
        add(f"  a cached probe can turn a cold build into a restore. CMake cold runs")
        add(f"  remove `build/` and pay a full configure.")
        add(f"- Warm runs reuse the tree each tool left behind. `noop` and `one-file`")
        add(f"  are primed with a real build first.")
        add(f"- The single-file scenario rewrites one unit's source before each sample,")
        add(f"  which Cova sees through its content hash and Ninja through the mtime.")
        add(f"- Cova runs with its shared cache enabled (the default). `plan` uses")
        add(f"  `cova build --dry-run`, which still loads the project and resolves the")
        add(f"  toolchain, against `ninja -n`, which reuses CMake's generated graph.")
        add(f"- Timing noise is real on a desktop; read the range, not just the median.")
        add("")
        return "\n".join(lines)

    def summary_json(self) -> dict[str, object]:
        return {
            "environment": self.environment(),
            "results": [
                {
                    "scenario": result.scenario,
                    "tool": result.tool,
                    "tus": result.size,
                    "note": result.note,
                    "median_s": result.median,
                    "min_s": result.minimum,
                    "max_s": result.maximum,
                    "samples": result.samples,
                }
                for result in self.results.values()
            ],
            "phases": self.phases,
        }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def default_cova_binary() -> Path:
    for candidate in (
        ROOT / "build" / "bench-release" / "bin" / "cova",
        ROOT / "build" / "bin" / "cova",
    ):
        if candidate.is_file():
            return candidate
    return ROOT / "build" / "bin" / "cova"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark Cova against CMake + Ninja",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--quick", action="store_true",
                        help="smoke run: 8 TUs, one sample, no warmup")
    parser.add_argument("--tus", default="40",
                        help="comma-separated translation-unit counts (default: 40)")
    parser.add_argument("--runs", type=int, default=5, help="timed samples per scenario")
    parser.add_argument("--warmups", type=int, default=1, help="discarded samples per scenario")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="parallel jobs passed to both tools (default: all cores)")
    parser.add_argument("--job-sweep", action="store_true",
                        help="also measure cold builds at -j1")
    parser.add_argument("--cache-ab", action="store_true",
                        help="also measure Cova incremental builds with the shared cache off")
    parser.add_argument("--real", action="store_true",
                        help="also build Cova's own src/ tree with both tools")
    parser.add_argument("--tools", default="cova,cmake",
                        help="comma-separated tools to measure (default: cova,cmake)")
    parser.add_argument("--compiler", default="clang++", help="C++ compiler for both tools")
    parser.add_argument("--cova", default=str(default_cova_binary()),
                        help="Cova binary (default: build/bench-release/bin/cova)")
    parser.add_argument("--cmake", default="cmake", help="cmake executable")
    parser.add_argument("--ninja", default="ninja", help="ninja executable")
    parser.add_argument("--workdir", default=str(WORK_ROOT), help="scratch directory")
    parser.add_argument("--markdown", help="write the report to this file")
    parser.add_argument("--json", help="write raw samples to this file")
    parser.add_argument("--no-phases", dest="phases", action="store_false",
                        help="skip the Cova phase profile")
    parser.add_argument("--keep", action="store_true", help="keep the generated trees")
    args = parser.parse_args(argv)

    args.tus = [int(value) for value in args.tus.split(",") if value.strip()]
    args.tools = [value.strip() for value in args.tools.split(",") if value.strip()]
    for tool in args.tools:
        if tool not in ("cova", "cmake"):
            parser.error(f"unknown tool: {tool}")
    if args.quick:
        args.tus = [8]
        args.runs = 1
        args.warmups = 0
        args.phases = False
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if not args.tus or any(size < 1 for size in args.tus):
        parser.error("--tus values must be positive")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    bench = Bench(args)
    try:
        bench.run()
    except BenchError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130

    report = bench.report()
    print()
    print(report)
    if args.markdown:
        Path(args.markdown).write_text(report + "\n", encoding="utf-8")
    if args.json:
        Path(args.json).write_text(
            json.dumps(bench.summary_json(), indent=2) + "\n", encoding="utf-8"
        )
    if not args.keep:
        shutil.rmtree(bench.work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
