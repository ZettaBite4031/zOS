from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path

from SCons.Script import ARGUMENTS, Action, Alias, AlwaysBuild, Default, Dir, Environment, SConscript

repo_root = Path(Dir("#").abspath)
sys.path.insert(0, str(repo_root / "BuildSystem"))

from Environments import CreateKernelEnvironment, CreateUefiEnvironment
from Toolchain import Toolchain

configuration = ARGUMENTS.get("Configuration", "Debug")
if configuration not in {"Debug", "Release"}:
    raise ValueError("Configuration must be Debug or Release.")

architecture = "AMD64"
output_root = f"#Output/{configuration}/{architecture}"
toolchain = Toolchain.Load(repo_root)

uefi_environment = CreateUefiEnvironment(repo_root, toolchain, configuration)
kernel_environment = CreateKernelEnvironment(repo_root, toolchain, configuration)

loader = SConscript("Boot/UEFI/SConscript", exports={"uefi_environment": uefi_environment, "output_root": output_root})
kernel = SConscript("Kernel/SConscript", exports={"kernel_environment": kernel_environment, "output_root": output_root})

host_execution_environment = os.environ.copy()
host_execution_environment["PATH"] = f"{toolchain.Bin}{os.pathsep}{host_execution_environment.get('PATH', '')}"

host_environment = Environment(tools=[], ENV=host_execution_environment)
host_environment["LLVM_NM"] = str(toolchain.LlvmNm)
host_environment["LLVM_READOBJ"] = str(toolchain.LlvmReadObj)
host_environment["QEMU"] = str(toolchain.Qemu)
host_environment["OVMF_CODE"] = str(toolchain.OvmfCode)
host_environment["OVMF_VARIABLES_TEMPLATE"] = str(toolchain.OvmfVariablesTemplate)
host_environment["QEMU_ACCELERATOR"] = toolchain.QemuAccelerator

def CopyFile(target, source, env):
    destination = Path(str(target[0]))
    origin = Path(str(source[0]))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
    return 0

def VerifyUefi(target, source, env):
    image = Path(str(source[0]))
    output = subprocess.check_output(
        [env["LLVM_READOBJ"], "--file-headers", str(image)],
        text=True,
        stderr=subprocess.STDOUT,
    )

    required = ("Format: COFF-x86-64", "IMAGE_SUBSYSTEM_EFI_APPLICATION")
    missing = [value for value in required if value not in output]
    if missing:
        print(output)
        raise RuntimeError(f"Invalid UEFI image; missing {missing}")

    stamp = Path(str(target[0]))
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text("UEFI image verified.\n", encoding="utf-8")
    return 0

def VerifyKernel(target, source, env):
    image = Path(str(source[0]))
    headers = subprocess.check_output(
        [env["LLVM_READOBJ"], "--file-headers", str(image)],
        text=True,
        stderr=subprocess.STDOUT,
    )
    symbols = subprocess.check_output(
        [env["LLVM_NM"], "--defined-only", str(image)],
        text=True,
        stderr=subprocess.STDOUT,
    )

    required_headers = ("Format: elf64-x86-64", "Arch: x86_64")
    missing = [value for value in required_headers if value not in headers]
    if missing or "KernelMain" not in symbols:
        print(headers)
        print(symbols)
        raise RuntimeError("Invalid kernel ELF image or missing KernelMain entry.")

    stamp = Path(str(target[0]))
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text("Kernel ELF image verified.\n", encoding="utf-8")
    return 0

def RunQemu(target, source, env):
    accelerator = env["QEMU_ACCELERATOR"]

    output_directory = (
        repo_root 
        / "Output"
        / configuration
        / architecture
    )

    esp_directory = output_directory / "ESP"
    variables_path = (
        output_directory
        / "Firmware"
        / "OVMF_VARS.fd"
    )

    processor = "host" if accelerator == "kvm" else "max"

    command = [
        str(env["QEMU"]),
        "-machine",
        "q35",
        "-accel",
        accelerator,
        "-cpu",
        processor,
        "-m",
        "512M",
        "-drive",
        (
            "if=pflash,"
            "format=raw,"
            "readonly=on,"
            f"file={env['OVMF_CODE']}"
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

    print("Starting QEMU.")
    print("Press Ctrl+C once to stop the emulator.")

    process = subprocess.Popen(
        command,
        start_new_session=True,
    )

    try:
        return_code = process.wait()
    except KeyboardInterrupt:
        print("\nStopping QEMU...")

        try:
            os.killpg(process.pid, signal.SIGINT)
        except ProcessLookupError:
            pass

        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print("QEMU did not stop after SIGINT; terminating it.")

            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                print("QEMU did not terminate; killing it.")

                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

                process.wait()

        print("QEMU stopped.")
        return 0

    if return_code < 0:
        print(f"QEMU exited due to signal {-return_code}.")
        return 0

    return return_code
    return subprocess.run(command, check=False).returncode

loader_verification = host_environment.Command(
    target=f"{output_root}/Verification/UEFI.stamp",
    source=loader,
    action=Action(VerifyUefi, "Verifying UEFI image $SOURCE"),
)
kernel_verification = host_environment.Command(
    target=f"{output_root}/Verification/Kernel.stamp",
    source=kernel,
    action=Action(VerifyKernel, "Verifying kernel image $SOURCE"),
)

staged_loader = host_environment.Command(
    target=f"{output_root}/ESP/EFI/BOOT/BOOTX64.EFI",
    source=loader,
    action=Action(CopyFile, "Staging $TARGET"),
)
staged_kernel = host_environment.Command(
    target=f"{output_root}/ESP/zOS/Kernel.elf",
    source=kernel,
    action=Action(CopyFile, "Staging $TARGET"),
)
ovmf_variables = host_environment.Command(
    target=f"{output_root}/Firmware/OVMF_VARS.fd",
    source=str(toolchain.OvmfVariablesTemplate),
    action=Action(CopyFile, "Preparing OVMF variable store $TARGET"),
)

build_products = [
    loader_verification,
    kernel_verification,
    staged_loader,
    staged_kernel,
]

Default(build_products)
Alias("build", build_products)
Alias("verify", [loader_verification, kernel_verification])

run = host_environment.Alias(
    "run",
    [*build_products, ovmf_variables],
    Action(RunQemu, "Running zOS under QEMU"),
)
AlwaysBuild(run)

