# zOS Development

This document describes the current repository, development toolchain, build commands, and verification workflow after the inter-era cleanup following Step 20.

## Tested host

The primary development host is Ubuntu 22.04 under WSL2, x86-64, with the repository in the Linux filesystem (for example, `~/Projects/zOS`). Building from `/mnt/c/...` is discouraged because Windows-mounted filesystems can be slower and less predictable for Linux tooling. Native Ubuntu may work but is not the primary verified setup.

## Toolchain and setup

`Tools/SetupDevEnv.sh` prepares repository-local state under `.zos/`:

- LLVM / Clang 22.1.8
- SCons 4.10.1
- supporting QEMU, OVMF, GDB and Python tooling

`.zos/` is local developer state and must not be committed.

```bash
chmod +x Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh --verify-only
# Alternatively:
./Tools/VerifyDevEnv.py
# Re-download LLVM when required:
./Tools/SetupDevEnv.sh --force-llvm
```

## Build and run commands

```bash
./Build.py build
./Build.py build -j8
./Build.py build Configuration=Release
./Build.py verify
./Build.py run
./Build.py debug
./Build.py debug GdbPort=1235
./Build.py clean
```

Debug is the default configuration. `debug` builds the image, starts QEMU paused with its GDB server, attaches GDB to `Kernel.elf`, and breaks at `KernelMain`; the default port is `1234`.

Run the automated boot regression test with:

```bash
python3 Tools/SmokeTest.py --configuration Debug
python3 Tools/SmokeTest.py --configuration Release
python3 Tools/SmokeTest.py --configuration Debug --timeout 60
```

The smoke test builds the requested configuration, starts QEMU headlessly, captures boot diagnostics, and checks for the native runtime, First City, and permanent runtime success markers while rejecting kernel fatal diagnostics. The guest intentionally remains running; a QEMU timeout is expected, provided all required milestones were observed. The smoke test is not a comprehensive stress or real-hardware test.

## Build outputs

Generated files live under:

```text
Output/<Configuration>/AMD64/
├── Boot/UEFI/BOOTX64.EFI
├── Kernel/Kernel.elf
├── User/FirstProgram/FirstProgram.elf
├── User/CityExit/CityExit.elf
├── User/CityFault/CityFault.elf
├── User/CitySurvivor/CitySurvivor.elf
├── ESP/EFI/BOOT/BOOTX64.EFI
├── ESP/zOS/Kernel.elf
└── Verification/
```

Object and generated-source subdirectories may also appear. QEMU boots using the staged ESP tree and OVMF.

## Separate target environments

`SConstruct` sets up distinct UEFI, kernel and native userspace construction environments in `BuildSystem/Environments.py`.

### UEFI application

The loader is compiled for `x86_64-pc-win32-coff`, freestanding C++23 and the UEFI ABI (`-fshort-wchar`), without host runtime dependencies. LLD's COFF linker produces a PE32+ EFI application with `EfiMain` as its entry point.

### AMD64 kernel

The kernel is compiled for `x86_64-unknown-none-elf`, freestanding C++23, without exceptions, RTTI, stack protector, hosted startup, PIC or PIE. It uses `-mno-red-zone` and avoids MMX/SSE use in the baseline execution environment, with warnings promoted to errors. LLD produces a static ELF64 image from `Kernel/Linker.ld` with entry symbol `KernelEntry`.

### Native user executables

`User/Runtime` is the small, separately compiled userspace support library. `BuildSystem/UserImages.py` consolidates native user ELF build rules and runtime-object reuse, while `SConstruct` wires the FirstProgram regression executable and the three City variants into the build. A generalized kernel embedding generator packages the resulting ELF files into the kernel for early bring-up. Embedded binaries are a temporary image source, not a requirement of the general process-launching model.

The first user programs are static AMD64 ELF64 executables linked at fixed virtual addresses. The loader enforces segment permissions and expects a freestanding ABI. They do not rely on dynamic libraries or a host libc. User-image build flags differ from kernel flags where appropriate; the native C++ runtime provides `_start` and other required helpers.

## Debug and Release

Debug currently adds `-O0 -g -fno-omit-frame-pointer`. Release adds `-O2 -g`; Release still retains debug symbols. Build and smoke-test both configurations after changes that affect calling conventions, memory maps, scheduling or executable startup.

## Verification layers

**Build-time format verification** checks the EFI application format/subsystem, the kernel ELF64 format and `KernelEntry`, and each native user ELF format and `_start` symbol.

**Kernel self-tests** verify allocator operations, W^X and page-table behavior, boot-memory reclamation, memory-metadata promotion, exception entry, CPU-local access, scheduling and timer preemption, per-process memory isolation, user syscall return, native C++ startup/shutdown, normal exit, fatal CPL3 page-fault containment, and survival of unrelated processes.

**QEMU smoke testing** checks that the integrated boot reaches expected milestones. A successful smoke run does not replace code review, failure-path tests, extensive concurrency stress, or physical-hardware validation.

## Initialization, tests, and fixtures

After the inter-era cleanup, code is organized by responsibility:

```text
Kernel/Initialization/          # State establishment, dependency ordering, boot orchestration
Kernel/Tests/                   # Bring-up and regression test cases
Kernel/Tests/Fixtures/          # Long-lived test-only process and thread ownership
Kernel/Processes/               # Process lifecycle and reusable launching
Kernel/Executables/             # Executable image view and ELF loading
Kernel/Architecture/AMD64/      # CPU-specific entry, interrupts, mapping and instructions
```

Tests still run during bootstrap. Their ownership is deliberately separate from production kernel services, but test processes and their resources must remain alive if scheduler or diagnostic references remain. The current kernel does not yet have safe general teardown or reclamation of terminated user process resources.

## Coding direction

Prefer explicit dependencies, ownership, error enums, clear architectural boundaries, deliberate failure paths, and regression tests for critical invariants. Use compact freestanding C++ rather than framework-style abstractions. Avoid speculative interfaces, generic helper repositories, surprising allocation, exceptions in kernel control flow, and assumptions that a local interrupt mask is sufficient for SMP synchronization.

The low-level runtime uses consolidated AMD64 helpers where appropriate. A launch failure state is explicit, but partial-resource rollback is not yet a universal guarantee. When modifying lifecycle logic, distinguish process termination, thread descheduling, object reference lifetime, and reclamation.

## Commit discipline

Keep milestone commits easy to scan:

```text
Add native multiprocess execution

- Add reusable user process launching
- Build and embed independent ELF64 applications
- Verify memory isolation and process lifecycle behavior
- Confirm scheduling survives normal exits and fatal user faults
```

Save longer retrospectives for release or Era-level documentation rather than turning ordinary commits into changelogs.
