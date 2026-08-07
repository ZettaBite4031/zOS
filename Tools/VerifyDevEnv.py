#!/usr/bin/env python3
"""Report and validate the repository-local zOS development toolchain."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> "NoReturn":
    print(f"[zOS error] {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_exports(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line.startswith("export ") or "=" not in line:
            continue

        key, raw_value = line[7:].split("=", 1)
        parts = shlex.split(raw_value)

        if len(parts) != 1:
            continue

        value = os.path.expandvars(parts[0])
        values[key] = value

    return values


def first_line(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    return completed.stdout.splitlines()[0]


def main() -> int:
    repository_root = Path(__file__).resolve().parents[1]
    toolchain_root = repository_root / ".zos" / "Toolchain"
    environment_file = toolchain_root / "Environment.sh"
    manifest_file = toolchain_root / "Manifest.json"

    if not environment_file.is_file():
        fail(
            "The toolchain environment has not been generated. Run "
            "./Tools/SetupDevelopmentEnvironment.sh first."
        )

    environment = parse_exports(environment_file)
    required = {
        "ZOS_LLVM_ROOT",
        "ZOS_PYTHON",
        "ZOS_SCONS",
        "ZOS_OVMF_CODE",
        "ZOS_OVMF_VARS_TEMPLATE",
        "ZOS_QEMU_ACCEL",
    }

    missing = sorted(required - environment.keys())
    if missing:
        fail(f"Environment file is missing: {', '.join(missing)}")

    commands = {
        "Clang": [str(Path(environment["ZOS_LLVM_ROOT"]) / "bin" / "clang"), "--version"],
        "LLD": [str(Path(environment["ZOS_LLVM_ROOT"]) / "bin" / "ld.lld"), "--version"],
        "SCons": [environment["ZOS_SCONS"], "--version"],
        "Python": [environment["ZOS_PYTHON"], "--version"],
        "QEMU": ["qemu-system-x86_64", "--version"],
        "GDB": ["gdb", "--version"],
    }

    print("zOS development environment")
    print(f"  Repository: {repository_root}")
    print(f"  Toolchain:  {toolchain_root}")

    for name, command in commands.items():
        try:
            print(f"  {name:<10} {first_line(command)}")
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            fail(f"{name} verification failed: {error}")

    for key in ("ZOS_OVMF_CODE", "ZOS_OVMF_VARS_TEMPLATE"):
        path = Path(environment[key])
        if not path.is_file():
            fail(f"{key} does not exist: {path}")

    print(f"  OVMF code  {environment['ZOS_OVMF_CODE']}")
    print(f"  OVMF vars  {environment['ZOS_OVMF_VARS_TEMPLATE']}")
    print(f"  QEMU accel {environment['ZOS_QEMU_ACCEL']}")

    if manifest_file.is_file():
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        print(f"  Manifest   schema {manifest.get('schema', 'unknown')}")
    else:
        print("  Manifest   missing")

    print("Environment verification passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())