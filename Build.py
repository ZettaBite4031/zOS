#!/usr/bin/env python3

from __future__ import annotations

import os
import signal
import subprocess
import sys
from pathlib import Path


def main() -> int:
    repository_root = Path(__file__).resolve().parent
    scons_path = (
        repository_root
        / ".zos"
        / "Toolchain"
        / "Python"
        / "bin"
        / "scons"
    )

    if not scons_path.is_file():
        print(
            "The repository-local SCons installation is missing.\n"
            "Run ./Tools/SetupDevelopmentEnvironment.sh first.",
            file=sys.stderr,
        )
        return 1

    arguments = sys.argv[1:] or ["build"]

    process = subprocess.Popen(
        [str(scons_path), *arguments],
        cwd=repository_root,
        start_new_session=True,
    )

    interrupted = False

    try:
        return_code = process.wait()
    except KeyboardInterrupt:
        interrupted = True

        try:
            os.killpg(process.pid, signal.SIGINT)
        except ProcessLookupError:
            pass

        try:
            return_code = process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

            try:
                return_code = process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

                return_code = process.wait()

    if interrupted and "run" in arguments:
        return 0

    if return_code < 0:
        return 128 + (-return_code)

    return return_code


if __name__ == "__main__":
    raise SystemExit(main())