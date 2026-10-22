"""Construction environments for zOS boot, kernel, and native user images."""

from __future__ import annotations

import os
from pathlib import Path

from SCons.Script import Environment

from Toolchain import Toolchain


def _execution_environment(toolchain: Toolchain) -> dict[str, str]:
    environment = os.environ.copy()
    environment["PATH"] = f"{toolchain.Bin}{os.pathsep}{environment.get('PATH', '')}"
    return environment


def _append_configuration_flags(environment, config: str) -> None:
    if config == "Debug":
        environment.Append(CXXFLAGS=["-O0", "-g", "-fno-omit-frame-pointer"])
    else:
        environment.Append(CXXFLAGS=["-O2", "-g"])


def CreateUefiEnvironment(repo_root: Path, toolchain: Toolchain, config: str):
    environment = Environment(
        tools=["clang", "clangxx"],
        ENV=_execution_environment(toolchain),
        CC=str(toolchain.Clang),
        CXX=str(toolchain.ClangXX),
        LINK=str(toolchain.LldLink),
        OBJSUFFIX=".obj",
        CPPPATH=[str(repo_root)],
    )

    environment.Append(
        CXXFLAGS=[
            "--target=x86_64-pc-win32-coff",
            "-std=c++23",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-stack-protector",
            "-fno-threadsafe-statics",
            "-fno-use-cxa-atexit",
            "-fshort-wchar",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
        ]
    )

    _append_configuration_flags(environment, config)

    environment["UEFI_LINKFLAGS"] = [
        "/nologo",
        "/subsystem:efi_application",
        "/entry:EfiMain",
        "/nodefaultlib",
        "/fixed:no",
    ]
    return environment

def CreateKernelEnvironment(repo_root: Path, toolchain: Toolchain, config: str):
    environment = Environment(
        tools=["clang", "clangxx"],
        ENV=_execution_environment(toolchain),
        CC=str(toolchain.Clang),
        CXX=str(toolchain.ClangXX),
        LINK=str(toolchain.Lld),
        OBJSUFFIX=".o",
        CPPPATH=[str(repo_root)],
    )

    environment.Append(
        CXXFLAGS=[
            "--target=x86_64-unknown-none-elf",
            "-std=c++23",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-stack-protector",
            "-fno-threadsafe-statics",
            "-fno-use-cxa-atexit",
            "-fno-pic",
            "-fno-pie",
            "-ffunction-sections",
            "-fdata-sections",
            "-mno-red-zone",
            "-mno-mmx",
            "-mno-sse",
            "-mno-sse2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
        ]
    )

    _append_configuration_flags(environment, config)

    environment["KERNEL_LINKFLAGS"] = [
        "-nostdlib",
        "-static",
        "--gc-sections",
        "-z",
        "max-page-size=0x1000",
    ]
    return environment

def CreateUserEnvironment(repo_root: Path, toolchain: Toolchain, config: str):
    environment = Environment(
        tools=["clang", "clangxx"],
        ENV=_execution_environment(toolchain),
        CC=str(toolchain.Clang),
        CXX=str(toolchain.ClangXX),
        LINK=str(toolchain.Lld),
        OBJSUFFIX=".o",
        CPPPATH=[str(repo_root)],
    )

    environment.Append(
        CXXFLAGS=[
            "--target=x86_64-unknown-none-elf",
            "-std=c++23",
            "-ffreestanding",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-stack-protector",
            "-fno-threadsafe-statics",
            "-fno-use-cxa-atexit",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-fno-pic",
            "-fno-pie",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
        ]
    )

    _append_configuration_flags(environment, config)

    environment["USER_LINKFLAGS"] = [
        "-nostdlib",
        "-static",
        "--gc-sections",
        "-z",
        "max-page-size=0x1000",
    ]
    return environment
