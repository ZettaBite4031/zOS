# zOS Architecture

This document describes the architecture implemented through Step 20, the inter-era cleanup that followed it, and the boundaries future subsystems should preserve. Planned systems are identified as such; this is not a claim that the whole desktop OS exists.

## Identity

zOS is a native AMD64 operating system booting exclusively through UEFI. It is an original, NT-inspired hybrid-kernel design, not an NT clone or a Unix kernel. The relevant influences are mechanism/policy separation, explicit process and thread objects, typed resources and handles, layered I/O, higher-level Executive facilities, and user-mode services where appropriate.

## Implemented boot-to-runtime path

```text
UEFI firmware
    ↓
BOOTX64.EFI → ELF64 Kernel.elf → versioned BootEnvironment handoff
    ↓
KernelEntry / physical and virtual memory bootstrap
    ↓
Kernel-owned page tables, guarded permanent stack, heap and metadata
    ↓
GDT / TSS / IDT, ACPI / MADT, CPU features, APIC and clock
    ↓
GS-based processor-local state / kernel process / threads
    ↓
Round-robin scheduler and timer-driven preemption
    ↓
Isolated per-process CR3 roots and CPL3 entry
    ↓
Native SYSCALL / SYSRET and ELF64 user executable loading
    ↓
Minimal native C++ user runtime and independent process exit/fault handling
    ↓
First City: three simultaneously runnable native processes
    ↓
Permanent kernel runtime
```

Initialization advances through explicit kernel phases, verified by bring-up tests. The cleanup after Step 20 separated test cases into `Kernel/Tests/` and test ownership into `Kernel/Tests/Fixtures/` while keeping permanent kernel infrastructure in `KernelRuntime`. The bootstrap still runs these tests deliberately. This is not yet an opt-in or production-disabled test harness.

## Memory

The physical memory manager controls managed pages after the firmware handoff and reclamation of eligible boot-services memory. Bootstrap metadata is promoted to permanent storage. Runtime processes have independent page-table roots (CR3); upper-half kernel mappings can be shared, while user mappings are private. The still-low-linked kernel image requires transitional lower-half supervisor mappings within process address spaces.

The kernel enforces meaningful mapping permissions: executable user segments are not writable, writable data and stacks are non-executable, user addresses require user-accessible mappings, and stack guards are unmapped. Mapping and metadata validation logic have separate translation-unit boundaries after cleanup; they remain part of the same underlying page-map ownership model.

First City confirms that different processes can map the **same user virtual addresses** to **different physical backing**. It does not imply shared memory, copy-on-write, demand paging, or arbitrary memory mapping by applications. Those are future milestones.

The kernel should preserve the invariant that newly created kernel mappings needed by running processes are visible in their address spaces. Updating shared upper-half page-table structures and introducing new top-level branches require deliberate synchronization policies as virtual memory grows.

## CPU, interrupts, and time

On the bootstrap processor, zOS discovers AMD64 CPU features, enumerates platform ACPI and MADT information, configures Local APIC/x2APIC where available, and quiesces the legacy 8259 PIC. It establishes an interrupt-vector policy, kernel exception handling, and a timer interrupt used for scheduling.

HPET provides a calibration reference; invariant TSC is preferred for monotonic runtime time where supported. Processor-local GS state records the running thread, entry-stack pointer, and local scheduler. Interrupt entry and paging validation have been separated into dedicated implementation units; the shared architectural contracts remain unchanged.

`SYSCALL` transitions onto a kernel-owned thread stack without dereferencing an untrusted user stack. `SYSRET` restores validated user state. Architecture entry creates a syscall request while `Kernel/SystemCalls/` handles native dispatch. `SWAPGS` is used on user/kernel privilege transitions where required.

The current runtime is single-processor. Merely masking interrupts is not a future SMP lock; cross-CPU object and scheduler operations will need explicit synchronization.

## Processes and threads

`Process` owns or references a PID, lifecycle state, address space, thread membership, and separate normal-exit and fatal-fault records. PID 0 is the kernel process and borrows the kernel page map. Isolated processes own distinct address spaces.

A `Thread` has a kernel stack, saved context, scheduler state, and exactly one owning process. Current execution states are `Ready`, `Running`, and `Terminated`. The scheduler maintains a round-robin ready queue and switches CR3 and TSS `RSP0` as necessary. Blocking/waiting is not implemented yet.

`UserProcessInstance` combines ownership of one isolated process, its main thread and loaded image. The reusable launch path prepares the address space, loads a static executable, configures entry/stack, and registers the thread. Executable bytes are supplied through a non-owning image view, rather than coupling the launcher to embedded kernel storage. The current examples are still embedded, because no filesystem exists.

Launcher state explicitly distinguishes success and partial failure; **fully transactional rollback and recycling are not implemented**. Terminating a thread removes it from scheduling and records process completion, but it does not safely reclaim a still-active kernel stack. Current tests retain process objects, stacks, executable backing and page-table resources after termination. The object and handle model must build on that distinction rather than assuming a dead thread immediately implies an unreferenced object.

## Native userspace

The AMD64 native syscall ABI uses `RAX` for syscall number/return value; `RDI`, `RSI`, `RDX`, `R10`, `R8`, and `R9` for up to six arguments; and `RCX`/`R11` as architectural clobbers. A shared zOS ABI definition separates syscall numbering from kernel dispatch and user-mode wrappers.

The implemented native syscall surface is intentionally small:

- `GetProcessId`: query the calling process identity.
- `ExitProcess`: terminate the calling single-threaded user process and preserve its exit status.

The static ELF64 loader validates executable format and segment permissions, installs a user stack and guard page, and establishes a user entry point. The minimal freestanding `User/Runtime` supplies `_start`, C++ static initialization and termination, `main()`, syscall wrappers, and basic memory operations. It does not supply libc, heap allocation for user programs, dynamic modules, TLS, exceptions, a native SDK, or a generic process-creation syscall.

The Step 19 regression executable verifies basic user startup, syscall round trips, stack/register preservation, destructor execution, and a normal exit. First City exercises three independent ELFs: one exits with a known status, another faults in CPL3 without taking down the kernel, and a third continues making progress under timer preemption. These are development fixtures, not installed system services.

## Tests and ownership

`Kernel/Initialization` owns initialization ordering. `Kernel/Tests` owns the tests themselves, and `Kernel/Tests/Fixtures` holds their long-lived objects separate from permanent subsystem state. This separation does not yet mean fixtures can be safely unloaded: their lifetimes must remain valid while scheduling or diagnostics could reference them.

`Tools/SmokeTest.py` runs a noninteractive QEMU boot of a selected Debug/Release configuration and checks expected runtime and First City diagnostic milestones. It complements build-time ELF/PE verification. It does not prove general hardware compatibility, security, or sustained multiprocess stress stability.

## Next: native object and handle model (Era III)

Step 21 will introduce typed kernel-owned objects and process-local validated handles, not expose raw kernel pointers to user programs. Important distinct concepts include object identity, reference lifetime, handle ownership, handle generation/stale-handle protection, type validation, and rights checks. An object can outlive a handle, and a terminated process's observable identity can outlive its execution resources.

Subsequent planned Era III work introduces waitable synchronization objects, shared sections and mappings, IPC, supervised user-mode services, and identities/security enforcement. None of those facilities are implemented merely by completing First City.

## Modules, drivers, and I/O (planned)

The future module loader should distinguish logical module **identity** from physical implementation. Native DLLs, privileged driver modules, and compatibility implementation modules may share relocations/imports concepts without sharing trust rules.

Hardware consumers should depend on standardized capabilities (`BlockDevice`, `NetworkAdapter`, `GraphicsAdapter`, `InputDevice`, `AudioDevice`) rather than vendor-specific classes. Device discovery, driver selection, dependencies, DMA/IOMMU and the storage boot package belong to later Eras. See [Driver and I/O Direction](DriverAndIO.md).

## Files, namespaces, and compatibility (planned)

Native file objects should not inherit Unix inode semantics by default. Expected concepts include volumes, file and directory objects, streams, metadata, filesystem implementations, and a native object namespace. Windows/Linux paths and module names can later be presented as views over the same native facilities.

Foreign APIs should translate onto native zOS APIs rather than inserting Windows/Linux syscall tables into architecture-entry code or reimplementing a second scheduler, process model, or kernel. See [Native Compatibility](Compatibility.md).

## Repository rule

The repository should grow when implementation requires a genuine ownership boundary:

> A file represents a concept you naturally search for by name. A directory represents an ownership boundary.

Avoid speculative interface hierarchies and generic dumping grounds. Explicit ownership and failure behavior take priority over cosmetic abstraction.
