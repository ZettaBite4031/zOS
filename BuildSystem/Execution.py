"""Host-side execution helpers for running zOS under QEMU."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from Toolchain import Toolchain

def CreateQemuCommand(repo_root: Path, toolchain: Toolchain, config: str, arch: str, *, debug: bool = False, gdb_port: int = 1234) -> list[str]:
    output_directory = repo_root / "Output" / config / arch

    esp_directory = output_directory / "ESP"
    variables_path = output_directory / "Firmware" / "OVMF_VARS.fd"

    processor = "host" if toolchain.QemuAccelerator == "kvm" else "max"

    command = [
        str(toolchain.Qemu),
        "-machine",
        "q35",
        "-accel",
        toolchain.QemuAccelerator,
        "-cpu",
        processor,
        "-m",
        "512M",
        "-drive",
        (
            "if=pflash,"
            "format=raw,"
            "readonly=on,"
            f"file={toolchain.OvmfCode}"
        ),
        "-drive",
        (
            "if=pflash,"
            "format=raw,"
            f"file={variables_path}"
        ),
        "-drive",
        f"format=raw,file=fat:rw:{esp_directory}",
        "-display",
        "gtk",
        "-debugcon",
        "stdio",
        "-global",
        "isa-debugcon.iobase=0xe9",
        "-nic",
        "none",
        "-no-reboot",
        "-no-shutdown",
    ]

    if debug:
        if gdb_port <= 0 or gdb_port > 65535:
            raise ValueError("The GDB port must be between 1 and 65535.")
        command.extend([ "-S", "-gdb", f"tcp:127.0.0.1:{gdb_port}" ])

    return command