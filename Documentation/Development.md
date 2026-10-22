# zOS Development

This document describes the development environment and build behavior of the current repository.

## Tested host

The primary development host is:

```text
Ubuntu 22.04
WSL2
x86-64 host
repository stored in the WSL Linux filesystem
```

Recommended checkout location:

```text
~/Projects/zOS
```

Building from `/mnt/c/...` is discouraged because the Windows-mounted filesystem is slower and has less predictable Linux filesystem behavior.

Native Ubuntu may work, but WSL2 on Ubuntu 22.04 is the currently tested setup.

## Toolchain

`Tools/SetupDevEnv.sh` creates repository-local development state under:

```text
.zos/
```

The current pinned major tools are:

```text
LLVM / Clang 22.1.8
SCons 4.10.1
```

The setup also locates or installs supporting host tools such as QEMU, OVMF, GDB, and Python 3.

`.zos/` is development-machine state and must not be committed.

## Setup

```bash
chmod +x Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh
```

Verify an existing environment without reinstalling:

```bash
./Tools/SetupDevEnv.sh --verify-only
```

or:

```bash
./Tools/VerifyDevEnv.py
```

Force a clean LLVM download when necessary:

```bash
./Tools/SetupDevEnv.sh --force-llvm
```

## Build commands

Default Debug build:

```bash
./Build.py build
```

Parallel build:

```bash
./Build.py build -j8
```

Release build:

```bash
./Build.py build Configuration=Release
```

Verify image formats:

```bash
./Build.py verify
```

Run in QEMU:

```bash
./Build.py run
```

Start the debugger workflow:

```bash
./Build.py debug
```

The debug action builds the image, starts QEMU paused with a GDB server, launches GDB against `Kernel.elf`, sets a temporary breakpoint at `KernelMain`, connects, and continues to the breakpoint.

The default GDB port is `1234`.

```bash
./Build.py debug GdbPort=1235
```

Clean outputs:

```bash
./Build.py clean
```

## Build outputs

Artifacts are generated under:

```text
Output/<Configuration>/AMD64/
```

A normal build contains at least:

```text
Boot/UEFI/BOOTX64.EFI
Kernel/Kernel.elf
ESP/EFI/BOOT/BOOTX64.EFI
ESP/zOS/Kernel.elf
Verification/
```

QEMU runs use the staged ESP tree and OVMF firmware.

## Build separation

The UEFI loader and kernel intentionally use different construction environments.

### UEFI target

The loader is compiled as x86-64 COFF/PE with UEFI-specific ABI requirements.

Important compiler properties include:

```text
--target=x86_64-pc-win32-coff
-std=c++23
-ffreestanding
-fshort-wchar
-fno-exceptions
-fno-rtti
-fno-stack-protector
```

It is linked as an EFI application with `lld-link`.

### Kernel target

The kernel is a freestanding ELF64 AMD64 target.

Current baseline flags include:

```text
--target=x86_64-unknown-none-elf
-std=c++23
-ffreestanding
-fno-exceptions
-fno-rtti
-fno-stack-protector
-fno-threadsafe-statics
-fno-use-cxa-atexit
-fno-pic
-fno-pie
-ffunction-sections
-fdata-sections
-mno-red-zone
-mno-mmx
-mno-sse
-mno-sse2
-Wall
-Wextra
-Wpedantic
-Werror
```

The kernel is linked statically with LLD using `Kernel/Linker.ld` and `KernelEntry` as the ELF entry symbol.

## Debug and Release

Debug adds:

```text
-O0
-g
-fno-omit-frame-pointer
```

Release currently adds:

```text
-O2
-g
```

Release still retains debug information.

## Verification

The build verifies generated artifacts rather than trusting linker success alone.

The UEFI image is checked for:

- `COFF-x86-64`
- EFI application subsystem

The kernel is checked for:

- `elf64-x86-64`
- `x86_64`
- the expected entry symbol

These checks are part of the normal build dependency graph.

## QEMU-first development

QEMU and OVMF are the primary bring-up environment.

Temporary QEMU-specific mechanisms are acceptable when they are localized bring-up tools rather than architecture requirements.

When a subsystem becomes permanent, its interfaces should remain suitable for real hardware even if QEMU is the first implementation target.

## Coding direction

The kernel uses compact, explicit C++ rather than framework-style abstraction.

Prefer:

- clear ownership
- small public APIs
- explicit error enums where failure is expected
- no allocation hidden inside surprising helpers
- architecture-specific code inside architecture boundaries
- runtime validation during bring-up
- self-tests that prove important invariants

Avoid:

- generic `Helper` collections
- speculative interfaces with no current implementation
- tiny-file bureaucracy
- exception-driven kernel control flow
- host-runtime dependencies
- cleverness that hides lifetime or privilege transitions

## Self-tests

Current initialization intentionally performs substantial runtime validation, including:

- allocator and mapping validation
- permanent metadata promotion
- boot-memory reclamation
- interrupt entry/return
- processor-local access
- thread context switching
- scheduler rotation
- timer preemption
- process address-space isolation
- Ring 3 execution
- CPL3 timer entry
- user page-fault containment

These tests are executable statements of architectural invariants and should remain useful as the kernel evolves.

## Commit discipline

Milestone commits should be short enough to scan.

Preferred:

```text
Add user fault isolation

- Contain fatal CPL3 exceptions without halting the kernel
- Terminate the faulting process and thread
- Verify scheduling continues after the user fault
```

Avoid changelog-sized commit bodies for ordinary milestones.
