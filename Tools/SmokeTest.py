#!/usr/bin/env python3
"""Boot the staged Debug/Release zOS image headlessly and check milestone output.

Usage: python3 Tools/SmokeTest.py --configuration Debug [--timeout 45]
Requires Build.py, the configured repo-local toolchain, and QEMU.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "BuildSystem"))

from Execution import CreateQemuCommand
from Toolchain import Toolchain

REQUIRED = (
    "[zOS/Runtime] Native zOS user runtime verified.",
    "[zOS/City] First City verified.",
    "[zOS/Object] Kernel object and handle model verified.",
    "[zOS/Kernel] Permanent kernel runtime entered.",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    parser.add_argument("--timeout", type=float, default=45.0)
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("timeout must be positive")

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "Build.py"),
            "build",
            f"Configuration={args.configuration}",
        ],
        cwd=ROOT,
        check=True,
    )

    toolchain = Toolchain.Load(ROOT)
    staged = ROOT / "Output" / args.configuration / "AMD64"
    vars_path = staged / "Firmware" / "OVMF_VARS.fd"

    if not vars_path.exists():
        vars_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(toolchain.OvmfVariablesTemplate, vars_path)

    command = CreateQemuCommand(
        ROOT,
        toolchain,
        args.configuration,
        "AMD64",
    )

    index = command.index("-display")
    command[index + 1] = "none"

    print("Running non-interactive zOS smoke test...", flush=True)

    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
            check=False,
        )
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode(errors="replace")

    print(
        output[-16000:],
        end="" if output.endswith("\n") else "\n",
    )

    if "FATAL:" in output:
        print(
            "FAIL: kernel fatal diagnostic",
            file=sys.stderr,
        )
        return 1

    missing = [
        item for item in REQUIRED
        if item not in output
    ]

    if missing:
        print(
            "FAIL: missing milestones: " +
            ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    print(
        "PASS: native runtime, First City, Step 21 handles, and permanent runtime reached"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
