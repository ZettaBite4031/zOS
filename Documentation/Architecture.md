# zOS Architecture

This document describes the architecture zOS has today and the direction future subsystems should preserve.

It is not a promise that every long-term subsystem already exists.

## Identity

zOS is a native AMD64 operating system with a UEFI-only boot architecture and an NT-inspired hybrid-kernel design.

"NT-inspired" means zOS deliberately learns from ideas such as:

- a small mechanism-oriented kernel core
- higher-level Executive-style policy
- typed kernel objects
- validated handles
- explicit process and thread objects
- layered I/O
- strong user/kernel separation
- user-mode services where appropriate
- hardware abstraction that does not leak vendor details upward

It does not mean binary-compatible NT internals, duplicated Windows kernel structures, or a requirement to copy Windows behavior.

zOS remains an original operating system.

## Current implemented architecture

As of Step 16, the boot-to-runtime path is:

```text
UEFI firmware
    ↓
BOOTX64.EFI
    ↓
ELF64 kernel loading
    ↓
versioned BootEnvironment handoff
    ↓
KernelEntry
    ↓
physical + virtual memory bootstrap
    ↓
kernel-owned page map / CR3
    ↓
interrupts + GDT + TSS + IDT
    ↓
permanent stack / heap / metadata
    ↓
ACPI + MADT + CPU capability discovery
    ↓
local APIC + timer + monotonic clock
    ↓
GS-based processor-local state
    ↓
kernel process + kernel threads
    ↓
round-robin scheduler + timer preemption
    ↓
isolated user process / private CR3
    ↓
Ring 3 execution
    ↓
user fault containment
    ↓
permanent runtime
```

The kernel explicitly tracks initialization phases. A phase advance is a statement that the preceding subsystem and its self-tests have completed successfully.

## Memory

The physical memory manager owns free physical pages after firmware handoff and boot-memory reclamation.

Boot-time metadata begins in controlled bootstrap storage and is promoted to permanent ownership as the kernel comes online.

Runtime process address spaces have independent CR3 roots. Permanent upper-half kernel mappings are shared by reference, while process-private lower-half mappings remain isolated.

The current low-linked kernel image requires transitional lower-half kernel mappings in each process address space. A future higher-half kernel image can remove this special case.

The design requires page permissions to mean something:

- executable user code is not writable
- writable user data and stacks are not executable
- user mappings are explicitly user-accessible
- kernel mappings remain supervisor-only
- user stack guard pages are intentionally unmapped

Future work should continue toward NX, SMEP, SMAP, ASLR, and related protections where supported.

## CPU, interrupts, and time

zOS currently supports modern AMD64 interrupt bring-up around the Local APIC and x2APIC where available.

The legacy 8259 PIC is quiesced rather than allowed to define the interrupt architecture.

Interrupt vectors are divided into deliberate ranges for CPU exceptions, device interrupts, Local APIC events, IPIs, and spurious delivery.

HPET is used as a calibration reference. The invariant TSC is preferred as the runtime monotonic clock source when available.

The Local APIC timer currently drives scheduler preemption.

## Processor-local state

The bootstrap processor has explicit processor-local state accessed through GS.

This state owns processor-specific runtime references such as the current thread and local scheduler.

Ring 3 transitions use the AMD64 `SWAPGS` model so kernel entry restores access to processor-local state and user return restores user GS state.

Future SMP work should extend this per processor rather than replacing it with global scheduler state.

## Processes and threads

A `Process` is not merely a PID.

It currently owns or references:

- process identity
- lifecycle state
- an address space
- intrusive thread membership
- fatal user-fault information

PID 0 is the kernel process and borrows the permanent kernel page map.

An isolated process owns an independent runtime page map.

Threads own execution state and kernel stacks. The scheduler operates on threads, while each thread belongs to exactly one process.

Current thread states are deliberately small:

```text
Ready
Running
Terminated
```

More states should appear only when a real blocking or lifecycle mechanism requires them.

## Scheduling

The current scheduler is a round-robin scheduler bound to processor-local state. It currently runs on the bootstrap processor; the ownership model is intended to extend per processor when SMP arrives.

It supports:

- explicit ready queues
- cooperative yield
- timer-driven preemption
- CR3 switching across process boundaries
- TSS `RSP0` synchronization with the running thread
- non-returning termination of a fatal user thread

Preemption saves a thread and makes it runnable again.

Termination abandons the faulting interrupt frame on the dead thread's kernel stack, marks the thread terminated, selects another ready thread, and never returns through `IRETQ` to invalid userspace state.

## Ring 3

zOS currently enters Ring 3 through an `IRETQ` frame containing user CS, SS, RSP, RIP, and RFLAGS.

User threads have:

- a validated executable entry mapping
- a validated writable non-executable stack
- TSS `RSP0` pointing at the thread's kernel stack
- conditional `SWAPGS` on privilege-changing interrupt entry and return

Step 16 proves that a deliberate user page fault can terminate the user process while the kernel and remaining scheduler continue running.

The next architectural addition is native `SYSCALL` / `SYSRET`.

## Native API direction

The native zOS ABI must remain smaller and more coherent than any compatibility surface built above it.

The AMD64 syscall entry path should gather architectural state into a neutral syscall context, determine the calling process, and dispatch through the appropriate ABI without embedding foreign syscall tables directly into architecture entry code.

Native zOS facilities should eventually include general primitives such as:

- objects and handles
- processes and threads
- sections and mapped memory
- files and devices
- events, semaphores, timers, and waits
- ports/messages for IPC
- asynchronous I/O
- security tokens and access checks

These primitives should be expressive enough that compatibility layers can translate foreign APIs onto them.

## Object and handle model

The long-term object manager is one of the most important NT-inspired pieces of zOS.

Kernel resources should become typed objects with explicit lifetime and access semantics. User mode should receive validated handles rather than kernel pointers.

Likely object categories include:

```text
Process
Thread
Section
File
Directory
Device
Driver
Event
Semaphore
Timer
Port
SecurityToken
ObjectDirectory
SymbolicLink
```

The exact type system should emerge from implemented features rather than be created as a speculative hierarchy all at once.

## Services and IPC

Not every operating-system facility belongs in the kernel.

zOS should eventually support privileged user-mode system services backed by strong IPC, shared sections, waitable objects, and security boundaries.

This is especially attractive for service management, desktop/session components, higher-level driver helpers, graphics services, and compatibility infrastructure.

The kernel should expose mechanisms; services can own policy where practical.

## Modules

The long-term module system should support a common low-level foundation for:

- native user DLLs
- kernel driver modules
- compatibility implementation modules

These module classes may have different trust rules and load paths even if they share concepts such as imports, exports, relocation, version metadata, and dependencies.

A central architectural rule is:

> Module identity is not required to equal physical implementation.

That rule enables future Windows and Linux compatibility contracts without creating one forwarding library for every foreign module name.

## Drivers

Hardware consumers should depend on standardized capabilities, not vendor classes.

Examples:

```text
BlockDevice
NetworkAdapter
GraphicsAdapter
InputDevice
AudioDevice
```

A driver binds to hardware and publishes one or more interfaces. Higher layers use those interfaces without checking PCI vendor IDs themselves.

A future device/driver manager will discover hardware, match drivers, resolve dependencies, and load the minimum required module set.

See `DriverAndIO.md`.

## Files and namespaces

The native file model should not inherit Unix inode semantics merely because they are familiar.

The intended vocabulary is closer to:

```text
Volume
FileObject
DirectoryObject
FileHandle
Stream
FileInformation
FileIdentity
Filesystem
```

Filesystem drivers implement storage formats beneath that abstraction.

The namespace may expose native object paths while compatibility layers present Windows or Linux path views over the same underlying objects.

## Compatibility boundary

Compatibility is intended to be one of zOS's defining features, but it must remain layered.

Preferred:

```text
foreign API / ABI
      ↓
translation
      ↓
native zOS API
      ↓
kernel / services
```

Avoid:

```text
foreign API
      ↓
fake foreign userspace
      ↓
fake foreign kernel
      ↓
zOS
```

Direct foreign syscalls may require ABI adapters, but they should still converge immediately onto native zOS operations.

See `Compatibility.md`.

## Repository rule

The repository should grow only when the implementation needs a new ownership boundary.

> A file represents a concept you naturally search for by name. A directory represents an ownership boundary.

Avoid generic dumping grounds such as `Helpers`, `Common`, or `Managers` unless they truly represent a coherent owned subsystem.
