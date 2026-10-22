<div align="center">

# zOS

### A modern, from-scratch operating system for AMD64

**UEFI-native · C++23 · NT-inspired · Built to become a real desktop operating system**

![Architecture](https://img.shields.io/badge/architecture-AMD64-4B6B88?style=flat-square)
![Firmware](https://img.shields.io/badge/firmware-UEFI-4B6B88?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B23-4B6B88?style=flat-square)
![Build](https://img.shields.io/badge/build-SCons-4B6B88?style=flat-square)
![Toolchain](https://img.shields.io/badge/toolchain-LLVM%2022.1.8-4B6B88?style=flat-square)
![Status](https://img.shields.io/badge/status-user%20mode%20online-C88A36?style=flat-square)

</div>

---

zOS is a from-scratch, general-purpose operating system for modern AMD64 machines.

It is not a Unix clone, a Linux distribution, or a Windows reimplementation. zOS takes strong architectural inspiration from Windows NT, especially its separation of low-level kernel mechanisms from higher-level policy, process and object model, handle-based resource management, layered I/O design, and willingness to treat user-mode subsystems as first-class parts of the operating system.

The long-term goal is ambitious but simple to state:

> Build an operating system that can eventually be used as an everyday desktop, without inheriting Unix as its native personality and without depending on Microsoft to provide the Windows-style experience.

The project is still experimental, but it has moved well beyond the boot-screen stage. zOS currently boots through UEFI, constructs its own memory environment, initializes modern interrupt and timer facilities, schedules kernel threads preemptively, creates independent process address spaces, enters AMD64 Ring 3, and contains fatal user-mode faults without taking down the kernel.

## Current milestone

The current development milestone is **Step 16: User Fault Isolation**.

Implemented today:

- UEFI x64 loader and versioned boot handoff
- Freestanding AMD64 C++23 kernel
- Physical and virtual memory management
- Permanent kernel address space, heap, stacks, and metadata ownership
- ACPI table discovery and MADT topology parsing
- AMD64 capability discovery
- x2APIC/local APIC support and legacy PIC quiescence
- HPET-assisted invariant-TSC monotonic clock
- Local APIC timer
- GS-based processor-local state
- Kernel threads and guarded stacks
- Cooperative and timer-driven preemptive scheduling
- Process objects and thread membership
- Independent per-process CR3 roots
- Ring 3 execution with TSS `RSP0`, `IRETQ`, and conditional `SWAPGS`
- User code/data/stack mappings with an unmapped guard page
- Fatal CPL3 exception containment
- Non-returning scheduler termination for a faulting user thread
- Process fault records and verified kernel survival after a user page fault

The next milestone is **Step 17: native `SYSCALL` / `SYSRET`**.

See [Documentation/Roadmap.md](Documentation/Roadmap.md) for the full roadmap.

## Design principles

### zOS stays zOS

Compatibility is a major long-term goal, but foreign operating systems must not define the native kernel architecture.

Windows and Linux software should eventually be translated onto native zOS facilities at the highest practical layer. The long-term module system is intended to allow a foreign logical module name, such as a Windows DLL identity or Linux shared-object identity, to resolve to a native zOS implementation without requiring a forest of forwarding libraries.

The rule is:

> Foreign API or ABI → translate once → native zOS API.

See [Documentation/Compatibility.md](Documentation/Compatibility.md).

### Modern first

Modern mechanisms are the primary path:

- UEFI rather than legacy BIOS
- GPT rather than MBR-oriented boot design
- x2APIC where available
- MSI-X/MSI for device interrupts
- PCI Express ECAM
- NVMe as an early real-storage target
- xHCI for USB
- ACPI for discovery and power management
- NX, SMEP, SMAP, PCID, and related processor protections where supported

Older mechanisms may exist for compatibility or bring-up, but must not shape platform-independent architecture.

### Architecture before convenience

A feature is not finished merely because it works once.

Subsystems should have explicit ownership, dependency direction, lifetime rules, failure behavior, and a believable expansion path. Temporary bring-up code is fine when it is localized and clearly temporary. Architectural debt is not.

### Files represent concepts; directories represent ownership

zOS intentionally avoids both giant catch-all files and directory forests full of tiny abstractions.

A file should represent something a developer would naturally search for by name. A directory should represent a real subsystem or ownership boundary.

### Freestanding C++ without pretending it is C

The kernel uses C++23 in a freestanding environment. Exceptions, RTTI, hosted startup code, and the ordinary host C/C++ runtime are not assumed.

Modern language features are welcome when their runtime costs and ownership implications are understood.

## Architectural direction

The current code is still compact, but the intended direction is:

```text
┌────────────────────────────────────────────────────┐
│                  User-mode system                  │
│ Applications · Services · Runtimes · Compatibility │
├────────────────────────────────────────────────────┤
│                    Executive                       │
│ Objects · Memory · Process · I/O · Security        │
├────────────────────────────────────────────────────┤
│                   Kernel core                      │
│ Scheduling · Dispatch · Interrupts · Timers        │
├────────────────────────────────────────────────────┤
│           Platform / architecture support          │
│ AMD64 · ACPI · APIC · Paging · Device plumbing     │
└────────────────────────────────────────────────────┘
```

The kernel is intended to remain hybrid rather than becoming either a giant monolith or a purity-driven microkernel.

Some high-risk or high-complexity facilities may eventually live in privileged user-mode services where that improves containment. Graphics is a likely example: a small trusted hardware-facing layer plus large user-mode graphics drivers and runtimes.

See [Documentation/Architecture.md](Documentation/Architecture.md) and [Documentation/DriverAndIO.md](Documentation/DriverAndIO.md).

## Repository layout

The repository currently reflects implemented components rather than speculative ones:

```text
zOS/
├── Boot/
│   ├── Protocol.hpp
│   └── UEFI/
├── BuildSystem/
├── Documentation/
├── Kernel/
│   ├── Architecture/AMD64/
│   ├── Diagnostics/
│   ├── Initialization/
│   ├── Memory/
│   ├── Platform/
│   ├── Processes/
│   ├── Runtime/
│   ├── Scheduling/
│   └── Time/
├── Tools/
├── Build.py
├── SConstruct
└── README.md
```

## Development environment

The tested host environment is Ubuntu 22.04 under WSL2, with the repository stored in the WSL Linux filesystem.

Recommended location:

```text
~/Projects/zOS
```

The setup script prepares a repository-local toolchain containing LLVM 22.1.8 and SCons 4.10.1.

```bash
chmod +x Tools/SetupDevEnv.sh
./Tools/SetupDevEnv.sh
```

Verify an existing setup:

```bash
./Tools/SetupDevEnv.sh --verify-only
```

or:

```bash
./Tools/VerifyDevEnv.py
```

## Building and running

```bash
./Build.py build
./Build.py build -j8
./Build.py build Configuration=Release
./Build.py verify
./Build.py run
./Build.py debug
./Build.py clean
```

Outputs are placed under:

```text
Output/<Configuration>/AMD64/
```

See [Documentation/Development.md](Documentation/Development.md) for details.

## Long-term direction

```text
CPU / kernel execution
        ↓
native userspace
        ↓
object + handle + IPC model
        ↓
module and driver architecture
        ↓
storage + filesystems
        ↓
networking
        ↓
everyday hardware
        ↓
desktop and software rendering
        ↓
accelerated graphics
        ↓
native application platform
        ↓
Windows / Linux native compatibility
        ↓
installer, updates, recovery, hardening
        ↓
daily-driver zOS
```

Driver modules are intended to be dynamically selected from hardware and dependency information. A small boot-module package will eventually provide the storage-critical drivers required before the system volume can be mounted; the full on-disk driver repository can then provide networking, USB, audio, graphics, and other hardware support.

Filesystems are expected to begin with RAMFS and FAT, followed by exFAT and eventually native NTFS support. A custom zOS filesystem is deliberately postponed until the operating system has enough real-world storage experience to know what it actually needs.

Graphics will begin with a basic framebuffer path and later grow into a split model with a standardized zOS GPU interface, hardware-facing system drivers, and substantial user-mode graphics implementations.

## Documentation

- [Architecture](Documentation/Architecture.md)
- [Long-term Roadmap](Documentation/Roadmap.md)
- [Development](Documentation/Development.md)
- [Native Compatibility](Documentation/Compatibility.md)
- [Driver, Storage, Networking, and Graphics Direction](Documentation/DriverAndIO.md)

Documentation should distinguish clearly between **implemented**, **next**, and **long-term design**. Future plans are not current capabilities.

## Project maturity

zOS is experimental software.

Do not currently trust it with important data, security-sensitive workloads, or valuable bare-metal machines.

QEMU/OVMF remains the primary bring-up environment. Real-hardware support will be introduced deliberately as the driver, I/O, storage, and recovery foundations mature.

## Contributing

zOS is primarily a personal operating-system project in an active architectural phase.

Technical discussion, review, experiments, and careful contributions are welcome. Large changes should fit the established direction rather than introducing isolated shortcuts that permanently dictate higher-level architecture.

Before proposing a major subsystem, preserve the project's core rules:

- modern-first platform design
- clear ownership and lifetime
- explicit subsystem boundaries
- freestanding compatibility
- reproducible tooling
- native zOS abstractions first
- foreign compatibility through translation, not architectural takeover
- implementation growth only when a real feature needs it

---

<div align="center">

### Built from first principles, one boundary at a time.

**Make it work. Make the boundary correct. Then make it bigger.**

</div>
