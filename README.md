<div align="center">

# zOS

### A modern, from-scratch operating system for AMD64

**UEFI-native · C++23 · NT-inspired · Built to become a real desktop operating system**

![Architecture](https://img.shields.io/badge/architecture-AMD64-4B6B88?style=flat-square)
![Firmware](https://img.shields.io/badge/firmware-UEFI-4B6B88?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B23-4B6B88?style=flat-square)
![Build](https://img.shields.io/badge/build-SCons-4B6B88?style=flat-square)
![Toolchain](https://img.shields.io/badge/toolchain-LLVM%2022.1.8-4B6B88?style=flat-square)
![Status](https://img.shields.io/badge/status-native%20multiprocess%20userspace-C88A36?style=flat-square)

</div>

---

zOS is a from-scratch, general-purpose operating system for modern AMD64 machines.

It is not a Unix clone, a Linux distribution, or a Windows reimplementation. zOS takes strong architectural inspiration from Windows NT: a distinction between low-level kernel mechanisms and higher-level policy, process and object models, handle-based resources, layered I/O, and user-mode system services. Inspiration is not binary compatibility, and the native zOS architecture must remain its own.

> Build an operating system that can eventually be used as an everyday desktop, without inheriting Unix as its native personality or depending on Microsoft to provide a Windows-style experience.

zOS is experimental. The kernel now boots through UEFI, sets up its own physical and virtual memory environment, initializes modern interrupt and time facilities, schedules kernel and user threads, and runs multiple independently loaded native ELF64 programs. A program can exit normally or trigger a fatal page fault without halting the kernel or unrelated processes.

## Current milestone

**Era II: Native Userspace is complete (Steps 1–20).** The inter-era architectural cleanup is complete. **Next: Step 21, Kernel Object and Handle Manager**, the first milestone of **Era III: Native Object and Service Model**.

Implemented and verified in QEMU/OVMF:

- UEFI x64 loader, static kernel ELF64 loading, and versioned, firmware-independent boot handoff
- Freestanding C++23 AMD64 kernel and permanent kernel-managed memory, heap, stacks, and metadata
- Physical and virtual memory allocators, W^X mapping enforcement, boot-memory reclamation, and isolated process page tables
- ACPI/MADT platform discovery, CPU feature discovery, x2APIC, interrupt vector policy, and legacy PIC quiescence
- HPET-referenced invariant-TSC clock, Local APIC timer, and GS-based processor-local state
- Kernel threads, cooperative switching, and timer-driven preemptive scheduling on the bootstrap processor
- Process and thread membership, independent CR3 roots, Ring 3 entry, user stacks with unmapped guards, and fatal user-fault containment
- Native AMD64 `SYSCALL` / `SYSRET` entry and separate syscall dispatch; current native calls include `GetProcessId` and `ExitProcess`
- Loading static, freestanding, non-PIE user ELF64 images into isolated processes
- Minimal C++ userspace runtime with `main()`, startup/termination, static constructors/destructors, basic memory functions, and syscall wrappers
- A reusable native process launcher and **First City**, which runs three independent executables: one exits, one faults, and one survives
- A development smoke test that boots QEMU headlessly and checks runtime and First City milestones

This is a **single-processor, experimental foundation**, not a complete desktop or general-purpose application environment. The kernel currently retains process resources after termination. No general user-mode object handles, blocking waits, IPC, filesystems, disk drivers, networking, or desktop services exist yet.

See [the roadmap](Documentation/Roadmap.md) for planned milestones and completion conditions.

## Design principles

### zOS stays zOS

Compatibility is a major long-term goal, but foreign operating systems must not define the native kernel architecture. Eventually, foreign APIs and ABIs should translate onto native zOS facilities at the highest practical layer.

> Foreign API or ABI → translate once → native zOS API.

See [Native Compatibility](Documentation/Compatibility.md).

### Modern first

The primary path targets UEFI, GPT, PCI Express ECAM, x2APIC where available, MSI/MSI-X, NVMe, xHCI, ACPI, and AMD64 protections such as NX, SMEP, SMAP, and PCID where supported. Some are currently used; others are future driver/platform milestones. Legacy mechanisms may exist as isolated compatibility or bring-up fallbacks, not as the platform-independent design center.

### Architecture before convenience

A feature is not finished merely because it works once. Ownership, dependency direction, lifetime, failure behavior, and a credible expansion path matter. Temporary scaffolding is acceptable when it is localized and explicit.

### Files represent concepts; directories represent ownership

Avoid both giant catch-all files and tiny abstractions created only to satisfy a pattern. A file should represent a concept you would naturally search for by name; a directory should represent a meaningful subsystem boundary.

### Freestanding C++ without pretending it is C

The kernel is C++23 with no assumption of a hosted runtime, RTTI, or exceptions. Modern language features are welcome when their runtime and ownership costs are understood. The early user runtime is separate from the kernel runtime, and neither is yet a complete libc or standard library.

## Architectural direction

```text
User-mode applications · services · runtimes · compatibility
                          ↓
      Executive facilities: objects · memory · I/O · security
                          ↓
       Kernel mechanisms: scheduling · dispatch · interrupts
                          ↓
      Platform: AMD64 · ACPI · APIC · paging · device support
```

The design is NT-inspired and hybrid rather than Unix-derived or a purity-driven microkernel. Privileged user-mode services will own policy where practical; the kernel retains mechanisms that require its protection boundary.

See [Architecture](Documentation/Architecture.md) and [Driver and I/O Direction](Documentation/DriverAndIO.md).

## Repository layout

The repository contains implemented components, not placeholder directories for speculative features:

```text
zOS/
├── ABI/                         # Native syscall identifiers and contracts
├── Boot/
│   ├── Protocol.hpp
│   └── UEFI/                    # BOOTX64.EFI and kernel handoff
├── BuildSystem/                # Toolchain and native user-image build helpers
├── Documentation/
├── Kernel/
│   ├── Architecture/AMD64/      # Paging, interrupts, CPU and syscall entry
│   ├── Diagnostics/
│   ├── Executables/            # Static ELF loader and image view
│   ├── Initialization/         # Bring-up orchestration
│   ├── Memory/
│   ├── Platform/
│   ├── Processes/
│   ├── Runtime/
│   ├── Scheduling/
│   ├── SystemCalls/
│   ├── Tests/                  # Bring-up tests and permanent test fixtures
│   └── Time/
├── User/
│   ├── Runtime/                # Minimal freestanding user C++ runtime
│   ├── FirstProgram/           # Step 18–19 regression executable
│   └── CityPrograms/           # Three First City executable variants
├── Tools/                      # Setup, verification and smoke testing
├── Build.py
├── SConstruct
└── README.md
```

## Development environment

The tested development host is **Ubuntu 22.04 under WSL2**, with the repository stored in the WSL Linux filesystem (for example, `~/Projects/zOS`). The setup script configures a repository-local LLVM **22.1.8** and SCons **4.10.1** toolchain.

```bash
chmod +x Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh --verify-only
```

## Building, running, and testing

```bash
./Build.py build
./Build.py build -j8
./Build.py build Configuration=Release
./Build.py verify
./Build.py run
./Build.py debug
python3 Tools/SmokeTest.py --configuration Debug
python3 Tools/SmokeTest.py --configuration Release
./Build.py clean
```

Outputs live under `Output/<Configuration>/AMD64/`. The smoke test builds the selected configuration, starts QEMU without a graphical display, captures the boot log, and verifies that Step 19, First City, and permanent runtime diagnostics appear without a kernel fatal error. It is a bring-up regression test, not exhaustive kernel validation.

See [Development](Documentation/Development.md).

## Long-term direction

```text
Execution foundation (complete)
        ↓
Native userspace / First City (complete)
        ↓
Objects · handles · synchronization · shared memory · IPC · services · security
        ↓
Modules · drivers · storage · filesystems · networking
        ↓
Everyday hardware · desktop · accelerated graphics
        ↓
Native app platform · Windows/Linux compatibility
        ↓
Installer · updates · recovery · hardening · daily-driver zOS
```

Storage-critical drivers should eventually be available from a small boot-module package; the full driver repository will become available after mounting a system volume. Filesystems will start with existing formats such as RAMFS, FAT and exFAT, with NTFS later. A custom native filesystem is deliberately not an early milestone. Graphics will progress from a basic display path to a user-mode graphics stack and hardware acceleration.

## Documentation

- [Architecture](Documentation/Architecture.md)
- [Long-term Roadmap](Documentation/Roadmap.md)
- [Development](Documentation/Development.md)
- [Native Compatibility](Documentation/Compatibility.md)
- [Driver, Storage, Networking, and Graphics Direction](Documentation/DriverAndIO.md)

Documentation must distinguish **implemented**, **next**, and **long-term design**. Future plans are not current capabilities.

## Project maturity

zOS is experimental software. Do not trust it with important data, security-sensitive workloads, or valuable bare-metal machines. QEMU/OVMF remains the primary bring-up environment, with real hardware introduced deliberately as drivers, storage, I/O, and recovery mature.

## Contributing

zOS is primarily a personal operating-system project in an active architectural phase. Technical discussion, review, experiments, and careful contributions are welcome. Please preserve modern-first design, explicit ownership, subsystem boundaries, freestanding portability, reproducible tooling, native zOS abstractions, and compatibility through translation rather than architectural takeover.

---

<div align="center">

### Built from first principles, one boundary at a time.

**Make it work. Make the boundary correct. Then make it bigger.**

</div>
