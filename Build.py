#!/usr/bin/env python3

from __future__ import annotations

import os
import shutil
import signal
import socket
import subprocess
import sys
from pathlib import Path


def _run_scons(repository_root: Path, scons_path: Path, arguments: list[str]) -> int:
    process = subprocess.Popen([str(scons_path), *arguments], cwd=repository_root, start_new_session=True)

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


def _argument_value(arguments: list[str], name: str, default: str) -> str:
    prefix = f"{name}="

    for argument in arguments:
        if argument.startswith(prefix):
            return argument[len(prefix):]

    return default


def _validate_debug_port(port: int) -> bool:
    if port <= 0 or port > 65535:
        print("GdbPort must be between 1 and 65535.", file=sys.stderr)
        return False

    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        probe.bind(("127.0.0.1", port))
    except OSError as error:
        print(f"Cannot start the GDB server on 127.0.0.1:{port}: {error}", file=sys.stderr)
        return False
    finally:
        probe.close()

    return True


def _stop_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    try:
        process.wait(timeout=2)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return

    process.wait()


def _run_debug_session(repository_root: Path, configuration: str, gdb_port: int) -> int:
    build_system = repository_root / "BuildSystem"
    sys.path.insert(0, str(build_system))

    from Execution import CreateQemuCommand
    from Toolchain import Toolchain

    gdb = shutil.which("gdb")
    if gdb is None:
        print("GDB is unavailable. Run ./Tools/SetupDevEnv.sh first.", file=sys.stderr)
        return 1

    if not _validate_debug_port(gdb_port):
        return 1

    architecture = "AMD64"
    output_directory = repository_root / "Output" / configuration / architecture
    kernel_image = output_directory / "Kernel" / "Kernel.elf"

    if not kernel_image.is_file():
        print(f"The kernel debug image is missing: {kernel_image}", file=sys.stderr)
        return 1

    debug_directory = output_directory / "Debug"
    debug_directory.mkdir(parents=True, exist_ok=True)
    qemu_log_path = debug_directory / "Qemu.log"

    toolchain = Toolchain.Load(repository_root)
    qemu_command = CreateQemuCommand(repository_root, toolchain, configuration, architecture, debug=True, gdb_port=gdb_port)

    gdb_command = [
        gdb,
        "-q",
        str(kernel_image),
        "-ex",
        "set pagination off",
        "-ex",
        "set disassembly-flavor intel",
        "-ex",
        "set print pretty on",
        "-ex",
        "set disassemble-next-line auto",
        "-ex",
        "set tcp auto-retry on",
        "-ex",
        "set tcp connect-timeout 10",
        "-ex",
        f"target remote 127.0.0.1:{gdb_port}",
        "-ex",
        "thbreak KernelMain",
        "-ex",
        "continue",
    ]

    print(f"Starting QEMU paused with GDB server on 127.0.0.1:{gdb_port}.")
    print(f"QEMU debug-console output: {qemu_log_path}")
    print("GDB will stop automatically immediately before KernelMain executes.")
    print("Use Ctrl+C inside GDB to interrupt guest execution after continuing.")

    with qemu_log_path.open("w", encoding="utf-8") as qemu_log:
        qemu_process = subprocess.Popen(qemu_command, cwd=repository_root, stdin=subprocess.DEVNULL, stdout=qemu_log, stderr=subprocess.STDOUT, start_new_session=True)

        try:
            gdb_process = subprocess.Popen(gdb_command, cwd=repository_root)

            while True:
                try:
                    return_code = gdb_process.wait()
                    break
                except KeyboardInterrupt:
                    # GDB receives the terminal SIGINT as well. Do not turn a
                    # normal debugger interrupt into teardown of the session.
                    continue
        finally:
            _stop_process_group(qemu_process)

    if return_code < 0:
        return 128 + (-return_code)

    return return_code


def main() -> int:
    repository_root = Path(__file__).resolve().parent
    scons_path = repository_root / ".zos" / "Toolchain" / "Python" / "bin" / "scons"

    if not scons_path.is_file():
        print("The repository-local SCons installation is missing.\nRun ./Tools/SetupDevEnv.sh first.", file=sys.stderr)
        return 1

    arguments = sys.argv[1:] or ["build"]

    targets = [ argument for argument in arguments if "=" not in argument and not argument.startswith("-") ]

    if "debug" not in targets:
        return _run_scons(repository_root, scons_path, arguments)

    if targets != ["debug"]:
        print("The debug action cannot be combined with other build targets.", file=sys.stderr)
        return 1

    configuration = _argument_value(arguments, "Configuration", "Debug")

    if configuration not in {"Debug", "Release"}:
        print("Configuration must be Debug or Release.", file=sys.stderr)
        return 1

    try:
        gdb_port = int(_argument_value(arguments, "GdbPort", "1234"))
    except ValueError:
        print("GdbPort must be an integer.", file=sys.stderr)
        return 1

    scons_arguments = [ "debug-build", *[ argument for argument in arguments if ( argument.startswith("-") or "=" in argument ) and not argument.startswith("GdbPort=") ] ]

    build_result = _run_scons(repository_root, scons_path, scons_arguments)
    if build_result != 0:
        return build_result

    return _run_debug_session(repository_root, configuration, gdb_port)


if __name__ == "__main__":
    raise SystemExit(main())