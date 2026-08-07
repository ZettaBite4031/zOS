<div align="center">

# zOS

### A modern, from-scratch operating system for AMD64

**UEFI-native · Modern C++ · NT-inspired · Built for learning, experimentation, and eventual everyday use**

<br>

![Architecture](https://img.shields.io/badge/architecture-AMD64-4B6B88?style=flat-square)
![Firmware](https://img.shields.io/badge/firmware-UEFI-4B6B88?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B23-4B6B88?style=flat-square)
![Build](https://img.shields.io/badge/build-SCons-4B6B88?style=flat-square)
![Toolchain](https://img.shields.io/badge/toolchain-LLVM%20%2F%20Clang-4B6B88?style=flat-square)
![Status](https://img.shields.io/badge/status-early%20development-C88A36?style=flat-square)

</div>

---

> zOS is a hobby operating-system project with a serious engineering goal: to grow from a small experimental kernel into a coherent, usable, and maintainable general-purpose operating system.

zOS is being developed entirely from scratch for modern AMD64 systems. It boots through UEFI, is written primarily in modern C++, and is designed around explicit subsystem boundaries, deterministic tooling, and long-term expandability.

The project is not intended to be a Unix clone, a Linux distribution, or a reimplementation of Windows. Its architecture takes inspiration from the Windows NT kernel family—particularly its object model, Executive subsystems, handle-based resource management, layered I/O system, and separation between hardware-specific and platform-independent code—while remaining an original operating system without compatibility constraints.

## Why zOS?

Operating systems are often built incrementally until early shortcuts become permanent architecture.

zOS takes the opposite approach.

The project is being restarted from a previous custom operating system implementation so that its foundations can be designed deliberately using newer coding practices, stronger architectural boundaries, and a clearer understanding of the technical debt that tends to accumulate in low-level systems.

The primary motivation is still curiosity and enjoyment. zOS exists because operating systems are interesting to design, build, debug, and understand.

The long-term ambition, however, is larger than a bootable demonstration:

> zOS should eventually become a genuinely usable operating system, while remaining understandable enough for one developer to reason about and maintain.

## Project goals

zOS aims to become:

* A native 64-bit operating system for AMD64 hardware
* Fully UEFI-based, without a legacy BIOS boot path
* Modern-first in its use of hardware and platform standards
* Structured around an NT-inspired hybrid-kernel architecture
* Written with modern, freestanding C++ where appropriate
* Capable of supporting user-mode applications and system services
* Designed for asynchronous and layered I/O
* Built around typed kernel objects and validated handles
* Multiprocessor-aware from its architectural foundations
* Reproducible to build from a clean development environment
* Suitable for both virtual machines and real hardware
* Usable as more than an academic or boot-screen-only kernel

zOS is also intended to remain enjoyable to work on. Features such as release codenames, custom tooling, and original subsystem designs are welcome when they improve the identity of the project without compromising technical quality.

## Design philosophy

### Modern first

Modern platform mechanisms are the primary implementation path.

Examples include:

* UEFI instead of legacy BIOS
* GPT instead of MBR-oriented booting
* x2APIC where available
* MSI-X and MSI for device interrupts
* PCI Express ECAM
* NVMe as the primary storage target
* xHCI for USB
* ACPI for platform discovery and power management
* NX, SMEP, SMAP, PCID, and other processor protections where supported

Older mechanisms may exist as compatibility paths or temporary bring-up tools, but they must remain isolated and must not define the architecture of higher-level systems.

### Architecture before convenience

zOS avoids introducing major facilities solely because they are easy to make work quickly.

Subsystems should have clear:

* Responsibilities
* Interfaces
* Ownership rules
* Dependency directions
* Synchronization models
* Failure semantics
* Expansion paths

A working implementation is important, but a working implementation that permanently damages the architecture is not considered complete.

### Explicit ownership

Kernel resources should have clear ownership and lifetime behavior.

The long-term kernel object model is expected to include objects such as:

* Processes
* Threads
* Jobs
* Sections
* Files
* Devices
* Drivers
* Events
* Semaphores
* Timers
* Ports
* Security tokens
* Object directories
* Symbolic links

User mode will interact with these resources through validated handles rather than direct kernel pointers.

### No accidental host dependencies

The build must not depend on whichever compiler, standard library, SDK, linker, or utility happens to be installed globally.

The project uses a repository-local, version-pinned toolchain and records the tools used to produce each build.

### Focused repository growth

The repository begins with only the components that currently exist.

Directories are added when they represent real ownership boundaries, not merely to make the project appear larger or more mature than it is.

Headers and translation units are colocated by component rather than separated into mirrored `src/` and `include/` hierarchies.

## Architecture direction

The long-term architecture is expected to contain four major layers.

```text
┌───────────────────────────────────────────────┐
│                User-mode system               │
│  Applications · Services · Native runtime     │
├───────────────────────────────────────────────┤
│                    Executive                  │
│ Object · Memory · Process · I/O · Security    │
├───────────────────────────────────────────────┤
│                  Kernel core                  │
│ Scheduling · Dispatch · Interrupts · Timers   │
├───────────────────────────────────────────────┤
│      HAL and AMD64 architecture mechanisms    │
│ ACPI · APIC · SMP · Paging · CPU facilities   │
└───────────────────────────────────────────────┘
```

The exact implementation will evolve as the system develops, but several architectural principles are already established:

* The kernel will be hybrid rather than purely monolithic or microkernel-based.
* The Executive will contain most operating-system policy.
* The kernel core will remain comparatively small.
* The HAL will isolate machine-level platform behavior.
* Drivers will use a layered I/O architecture.
* Executable loading will remain separate from process creation.
* Security and access rights will be part of the object model rather than a late addition.
* User-mode environments may eventually expose different APIs without changing the kernel into a Unix-like design.

More detailed architectural decisions belong in the project documentation rather than this README.

## Modern C++ without a hosted runtime

zOS uses modern C++, beginning with a C++23 language baseline.

The project intends to use appropriate standard facilities such as:

```cpp
std::variant
std::optional
std::expected
std::span
std::array
std::tuple
std::string_view
```

The kernel is freestanding and does not depend on Newlib.

Instead, zOS will provide a controlled compiler-support runtime and selectively validate standard-library facilities before approving them for kernel use.

Exceptions and RTTI are initially disabled in kernel mode. This does not prevent the use of modern C++ language features; it avoids introducing complex runtime behavior before the necessary memory management, unwinding, and failure infrastructure exists.

A fuller C and C++ runtime may later be provided for user-mode applications.

## Current status

zOS is in early development.

The current repository establishes the first build and boot foundation:

* Repository-local LLVM and SCons toolchain
* UEFI x64 application build
* Freestanding ELF64 AMD64 kernel build
* OVMF boot under QEMU
* Headless debug-port output
* Optional graphical QEMU display
* Binary-format verification
* Separate UEFI and kernel build environments
* Staged EFI System Partition directory
* Debug and Release configurations

The UEFI loader currently proves that firmware can locate and execute `BOOTX64.EFI`.

The kernel is built as an independent ELF64 image with a defined entry point.

The next major milestone is for the UEFI loader to locate, validate, load, and transfer control to the kernel.

## Near-term roadmap

The initial development path is intentionally narrow.

### Boot foundation

* Read the kernel from the EFI System Partition
* Parse and validate the ELF64 image
* Load `PT_LOAD` segments
* Construct a versioned boot environment
* Capture the final UEFI memory map
* Exit UEFI boot services
* Enter the kernel

### Kernel foundation

* Establish structured logging
* Initialize physical memory management
* Define the virtual address-space layout
* Initialize page-table ownership
* Add exception handling
* Establish CPU-local state
* Introduce the first HAL boundaries
* Prepare for symmetric multiprocessing

### Executive foundation

* Kernel object model
* Handle tables
* Dispatcher objects
* Threads and scheduler
* Processes and address spaces
* Native system-call entry
* First user-mode process

### I/O foundation

* Driver and device objects
* I/O request packets
* PCI Express discovery
* NVMe storage
* Partition management
* Initial filesystem
* Namespace-based file access

The roadmap will expand as each foundational layer becomes stable.

## Repository layout

The initial repository remains intentionally small:

```text
zOS/
├── Boot/
│   └── UEFI/
│       ├── Loader.cpp
│       ├── UEFI.hpp
│       └── SConscript
│
├── BuildSystem/
│   ├── Environments.py
│   └── Toolchain.py
│
├── Documentation/
├── Kernel/
│   ├── Kernel.cpp
│   ├── Linker.ld
│   └── SConscript
│
├── Tools/
├── Build.py
├── SConstruct
└── README.md
```

Future directories such as `Executive`, `HAL`, `Architecture`, `Drivers`, and `Runtime` will be added only when their first concrete responsibilities are implemented.

## Development environment

The current development environment is:

* Ubuntu 22.04 under WSL2
* LLVM and Clang
* LLD and `lld-link`
* SCons
* QEMU
* OVMF
* GDB
* Python 3

The repository should be stored inside the WSL Linux filesystem for better build performance and more predictable Linux filesystem behavior.

Recommended location:

```text
~/Projects/zOS
```

Avoid building from `/mnt/c/...` unless necessary.

## Setting up the toolchain

The setup script installs the required Ubuntu host utilities and prepares the repository-local LLVM and SCons environment.

```bash
chmod +x Tools/SetupDevelopmentEnvironment.sh
./Tools/SetupDevelopmentEnvironment.sh
```

To activate the environment manually:

```bash
source .zos/Toolchain/Environment.sh
```

To verify the existing installation:

```bash
./Tools/SetupDevelopmentEnvironment.sh --verify-only
```

Or:

```bash
./Tools/VerifyDevelopmentEnvironment.py
```

Generated toolchain files are stored under:

```text
.zos/Toolchain/
```

The `.zos` directory is local development state and should not be committed.

## Building zOS

Build the default Debug configuration:

```bash
./Build.py build
```

Build with multiple jobs:

```bash
./Build.py build -j8
```

Build the Release configuration:

```bash
./Build.py build Configuration=Release
```

Verify the generated UEFI and kernel images:

```bash
./Build.py verify
```

Run zOS under QEMU:

```bash
./Build.py run
```

Clean generated outputs:

```bash
./Build.py clean
```

Build products are placed under:

```text
Output/<Configuration>/AMD64/
```

For example:

```text
Output/Debug/AMD64/
├── Boot/
│   └── UEFI/
│       └── BOOTX64.EFI
├── Kernel/
│   └── Kernel.elf
├── ESP/
├── Firmware/
└── Verification/
```

## Build-system policy

SCons is the canonical build system.

The build is divided into separate construction environments for:

* Host tools and tests
* The UEFI loader
* The freestanding kernel

This prevents UEFI-specific ABI and linker behavior from leaking into kernel code.

The build system should remain structured:

```text
SConstruct
    Top-level graph, aliases, and configuration

BuildSystem/
    Shared toolchain and environment definitions

Component/SConscript
    Component-owned sources and dependencies
```

Production source lists should generally be explicit rather than automatically globbing every source file in a directory.

## Versioning

zOS uses Semantic Versioning.

Early development releases will remain under major version zero:

```text
0.1.0-alpha.1
0.1.0
0.2.0
1.0.0
```

Important protocols and compatibility surfaces will also have independent versions, including:

* Boot protocol
* Kernel ABI
* Driver ABI
* Native user-mode API

Meaningful milestone releases may receive codenames.

For example:

```text
zOS 0.5.0 "Wayfinder"
zOS 1.0.0 "First Light"
```

Codenames are descriptive only and do not affect compatibility.

## Project maturity

zOS is experimental software.

It is not currently suitable for:

* Production use
* Storing important data
* Security-sensitive environments
* Running directly on valuable hardware
* Replacing an existing operating system

Development currently targets QEMU and OVMF first. Real-hardware support will be introduced deliberately as the boot, memory, interrupt, and driver foundations mature.

## Documentation

Project documentation is maintained under:

```text
Documentation/
```

Current and planned documents include:

* Architecture and Development Charter
* Development Environment
* Initial Repository Design
* Boot Protocol
* Coding Standard
* Kernel Object Model
* Memory Architecture
* I/O Architecture
* Driver Model
* Native System API
* Platform Profiles

The README provides orientation. Detailed design decisions belong in their respective documents.

## Contributing

zOS is currently primarily a personal hobby project and remains in a highly formative architectural stage.

Issues, technical discussion, design review, and careful experimentation are welcome. Large implementation contributions should align with the project’s established architecture rather than introducing isolated “make it work” solutions.

Before proposing substantial changes, review the documentation and ensure that the change preserves:

* Modern-first platform design
* Clear subsystem ownership
* Explicit lifetime behavior
* Freestanding compatibility
* Toolchain reproducibility
* Long-term architectural expandability

## Acknowledgements

zOS is informed by operating-system architecture, firmware standards, processor documentation, prior zOS development experience, and the study of existing systems.

The project takes conceptual inspiration from Windows NT’s subsystem organization while remaining an independent design.

It is also shaped by the broader hobby operating-system community and the many developers who publish low-level research, documentation, experiments, and debugging techniques.

---

<div align="center">

### Built from first principles, one boundary at a time.

**zOS is not trying to become large quickly. It is trying to become correct deliberately.**

</div>
