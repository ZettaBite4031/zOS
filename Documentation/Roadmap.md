# zOS Long-Term Roadmap

This roadmap is directional rather than contractual. Each numbered Step is intended to be a concrete feature milestone with an observable completion condition. Later Steps may move when implementation teaches us something new.

**Current repository baseline (after inter-era cleanup):**

```text
Era I:  Execution Foundation — complete (Steps 1–16)
Era II: Native Userspace — complete (Steps 17–20)
Inter-era architectural cleanup — complete
Step 20 complete: First City
Next: Step 21, Kernel Object and Handle Manager (Era III)
```

Step numbers refer to **feature milestones**, not commits or file sets. Completion means the milestone's relevant behavior was exercised and verified. This does not imply production readiness, comprehensive hardware support, or full resource reclamation.

## Era I — Execution foundation

### 1. MADT topology ✅
- Discover processors, Local APICs, I/O APICs, and IRQ overrides.

### 2. AMD64 CPU capability discovery ✅
- Detect x2APIC, TSC, NX, SMEP, SMAP, PCID, and related capabilities.

### 3. Local APIC controller ✅
- Establish modern local interrupt delivery.

### 4. Legacy PIC quiescence ✅
- Mask the 8259 and prevent it from shaping the modern interrupt architecture.

### 5. Interrupt-vector policy ✅
- Reserve clean ranges for exceptions, devices, LAPIC events, IPIs, and spurious delivery.

### 6. Monotonic clock ✅
- Calibrate the invariant TSC against HPET and expose stable monotonic time.

### 7. Local APIC timer ✅
- Deliver periodic timer interrupts suitable for scheduling.

### 8. Processor-local state ✅
- Install GS-based per-CPU state.

### 9. Kernel threads ✅
- Create guarded kernel stacks and independent thread execution contexts.

### 10. Cooperative context switching ✅
- Switch voluntarily between kernel threads.

### 11. First scheduler ✅
- Maintain a runnable ready queue and round-robin scheduling.

### 12. Preemptive scheduling ✅
- Drive scheduling from timer interrupts.

### 13. Process abstraction ✅
- Add PIDs, process lifecycle state, and thread membership.

### 14. Per-process address spaces ✅
- Give isolated processes independent CR3 roots while preserving kernel mappings.

### 15. Ring 3 execution ✅
- Enter CPL3 with user mappings, TSS `RSP0`, `IRETQ`, and conditional `SWAPGS`.

### 16. User fault isolation ✅
- Terminate a faulting user process/thread without halting the kernel.
- Verify scheduling and the kernel continue after a deliberate user page fault.

## Era II — Native userspace

### 17. Native `SYSCALL` / `SYSRET` ✅
- Establish the first intentional userspace-to-kernel ABI.
- Switch safely between user and kernel stacks; keep architecture entry separate from native dispatch.

### 18. Static user ELF loader ✅
- Load real static user executables instead of generated machine-code pages.
- Map segments with correct permissions and construct a guarded user stack and entry context.

### 19. Native zOS user runtime ✅
- Add native startup/termination, basic syscall wrappers, and freestanding memory functions.
- Support C++ `main()`, static constructors/destructors, and normal exit with a retained status.

### 20. First City ✅
- Run three independently loaded, isolated native ELF64 processes.
- Verify distinct backing for identical user addresses; contain one fatal fault, record another program's normal exit, and prove a third continues under preemption.

## Era III — Native object and service model

### 21. Kernel object and handle manager ← NEXT
- Represent kernel-owned resources as typed objects with explicit identity and reference lifetime.
- Give user mode validated, per-process handles rather than kernel pointers; establish handle closure, rights, and stale-handle protection.

### 22. Waitable synchronization objects
- Add events, semaphores, mutexes, and timers.
- Allow threads to block and wake without spinning.

### 23. Shared sections and mapped memory
- Share memory safely between processes.
- Establish the base for mapped files, DLLs, IPC, and graphics buffers.

### 24. Native IPC
- Add message/port-style process communication.
- Support shared-memory and synchronization-backed IPC patterns.

### 25. System service model
- Start and supervise privileged user-mode services.
- Move policy out of the kernel where appropriate.

### 26. Process security foundation
- Introduce identities, security tokens, privileges, and access checks.
- Make the model usable by later files, devices, IPC, and services.

## Era IV — Modules, drivers, and hardware discovery

### 27. Native module loader
- Add imports, exports, relocations, dependency resolution, and version metadata.
- Establish reusable infrastructure for DLLs, drivers, and compatibility modules.

### 28. Kernel driver module format
- Load privileged driver modules independently from the kernel image.
- Add an explicit versioned driver ABI.

### 29. Boot-module package
- Let UEFI load the minimum storage-critical modules beside the kernel.
- Break the dependency cycle between storage drivers and the disk containing them.

### 30. Device and driver manager
- Enumerate hardware and match it to installed drivers.
- Resolve dependencies and load only required modules.

### 31. DMA and IOMMU infrastructure
- Add explicit DMA mappings and device-visible buffers.
- Use IOMMU isolation where available.

### 32. Standard device interfaces
- Publish capabilities such as `BlockDevice`, `NetworkAdapter`, and `GraphicsAdapter`.
- Keep vendor implementation details below those contracts.

## Era V — Storage and filesystems

### 33. Block-device abstraction
- Expose generic block I/O independent of the storage controller.

### 34. RAM-backed test block device
- Test storage/filesystem layers without hardware or DMA uncertainty.

### 35. First real block driver
- Implement VirtIO-block first.
- Follow with NVMe as the first major real-hardware storage target.

### 36. Partition and volume manager
- Parse GPT.
- Discover volumes and publish them through the native object model.

### 37. Filesystem interface and file objects
- Add native files, directories, streams, metadata, and mount operations.
- Avoid defining the generic model in Unix inode terms.

### 38. RAMFS
- Validate file handles, path traversal, directory semantics, and caching.

### 39. FAT32
- Implement read support and then read/write.
- Cover ESP and common removable-media use cases.

### 40. FAT12 and FAT16
- Extend the FAT family implementation rather than inventing separate architectures.

### 41. System-volume boot
- Mount the actual zOS system volume.
- Replace boot-package-only module access with the full on-disk driver repository.

### 42. exFAT
- Provide native read/write support for modern removable storage.

### 43. NTFS
- Begin with safe read support.
- Grow toward full native read/write and important NTFS features.

## Era VI — Networking

### 44. Network-adapter interface
- Standardize packet queues, link state, MTU, capabilities, and offloads.

### 45. VirtIO-net
- Prove the network-driver boundary in QEMU.

### 46. Ethernet, ARP, IPv4, and ICMP
- Bring up basic LAN communication.
- `ping` becomes the first visible milestone.

### 47. UDP, DHCP, and DNS
- Obtain network configuration automatically.
- Resolve hostnames and exchange datagrams.

### 48. TCP
- Add reliable connection-oriented streams.

### 49. Native userspace network API
- Expose sockets or an equivalent clean zOS-native networking interface.
- Keep it suitable for later WinSock/POSIX translation.

### 50. First physical Ethernet driver
- Target common, well-documented hardware.

### 51. Wi-Fi
- Add 802.11, authentication, WPA2/WPA3, regulatory behavior, roaming, and power handling.

## Era VII — Everyday hardware

### 52. xHCI USB
- Support the modern USB host-controller model.

### 53. USB HID and input
- Support keyboards, mice, gamepads, and generic HID input.

### 54. Power management
- Add clean shutdown/reboot, CPU idle, battery state, and device power states.
- Grow later into sleep/resume.

### 55. Audio
- Add a generic audio-device interface and software mixing.
- Begin with a virtual or well-documented target.

### 56. Bluetooth
- Add wireless peripheral and later audio support.

## Era VIII — Basic graphical desktop

### 57. Basic display driver
- Preserve/use a UEFI GOP framebuffer.
- Guarantee a functional display path without accelerated GPU support.

### 58. Software 2D graphics
- Add surfaces, fonts, images, clipping, and software composition.

### 59. Window server and compositor
- Manage windows, surfaces, input routing, focus, and presentation in user mode.

### 60. First usable desktop shell
- Add session startup, launcher/panel, basic file management, and settings.
- Remain usable with software rendering.

## Era IX — Accelerated graphics

### 61. zOS Graphics Kernel
- Add adapter objects, GPU contexts, queues, fences, buffers, and scheduling.

### 62. GPU virtual memory
- Give processes isolated GPU address spaces.
- Add VRAM/system-memory residency accounting.

### 63. VirtIO-GPU
- Validate the accelerated graphics architecture before real hardware.

### 64. User-mode GPU driver interface
- Define the stable boundary used by shader compilers and command-generation libraries.

### 65. Mesa integration
- Reuse portable open graphics infrastructure where practical.
- Keep the zOS system-driver boundary independent of Linux DRM.

### 66. First real accelerated GPU
- AMD is the preferred initial desktop candidate unless hardware/documentation realities change.

### 67. Intel graphics
- Add a second major vendor architecture.

### 68. NVIDIA graphics
- Use available open hardware/kernel/user-mode knowledge where practical.
- Expect firmware and ecosystem constraints.

### 69. GPU recovery and advanced display
- Add timeout/reset recovery, multi-monitor support, VRR/HDR, video engines, and multi-GPU policy.

## Era X — Native application platform

### 70. Native shared DLLs
- Add dynamic user libraries, TLS, initialization/destruction, and symbol versioning.

### 71. Application/package format
- Install applications as coherent units with manifests, resources, and dependency metadata.

### 72. Native SDK
- Publish headers, libraries, compiler support, debugger integration, and packaging tools.

### 73. Major language runtimes
- Keep C/C++ first-class.
- Add high-value runtimes such as .NET, Python, Java, and Rust support.

### 74. Package manager
- Install, remove, update, and resolve application dependencies coherently.

## Era XI — Native compatibility

### 75. Logical module contracts
- Separate foreign module identity from physical implementation.
- Allow virtual DLL/SONAME identities backed by native zOS modules.

### 76. PE/COFF userspace loader
- Load Windows executables and DLL-format images.

### 77. Windows ABI translation runtime
- Translate high-value Win32/NT API calls directly onto native zOS facilities.
- Centralize or generate Microsoft-x64 ABI thunks.

### 78. Windows virtual DLL namespace
- Resolve `kernel32`, `kernelbase`, `user32`, API sets, and related identities through logical contracts.

### 79. Linux ELF compatibility runtime
- Present expected libc/pthread/dl-style identities through a native zOS compatibility module.

### 80. Linux direct-syscall adapter
- Translate software that bypasses libc.
- Keep the adapter thin and native-API-backed.

### 81. Compatibility-aware language runtimes
- Prefer native zOS runtime implementations for portable application formats where possible.

### 82. Compatibility profiles
- Version foreign behavior without freezing or polluting the native zOS ABI.

## Era XII — Daily-driver completion

### 83. User accounts and login
- Add multiple users, profiles, credentials, and session identity.

### 84. Configuration system
- Provide coherent machine and per-user configuration.
- Avoid scattering critical state across arbitrary files.

### 85. Installer
- Partition, format, install boot components, install the system, and perform first hardware detection.

### 86. System updates
- Update the kernel, modules, drivers, system libraries, and packages coherently.
- Prefer rollback-capable or atomic behavior.

### 87. Recovery environment
- Repair boot, filesystem, package, driver, and update failures without another OS.

### 88. Crash dumps and diagnostics
- Add kernel/process dumps, structured logging, symbol support, and useful diagnostics.

### 89. Security hardening
- Add module/driver signing policy, ASLR, exploit mitigations, sandboxing, and stronger capability controls.

### 90. Daily-driver zOS
- Install on ordinary hardware.
- Boot, network, sleep, play audio, accept input, render accelerated graphics, update, and recover.
- Run useful native software and meaningful Windows/Linux software through native compatibility.

## Inter-era cleanup after Step 20 ✅

This work did not add a numbered feature milestone; it prepared the kernel for Era III:

- Separate initialization orchestration from regression test implementations and long-lived test fixtures.
- Refine the process launcher and executable-image view; record partial launch failures explicitly.
- Consolidate native user ELF construction and kernel image embedding.
- Centralize relevant AMD64 CPU/interrupt-state helpers and split interrupt entry and paging validation where appropriate.
- Add a noninteractive QEMU smoke test for native runtime, First City, and permanent runtime milestones.

The cleanup preserved externally observable bring-up behavior. Terminated process resource reclamation, SMP-safe synchronization, blocking/idle scheduling, and full UEFI loader restructuring are **not** considered completed by the cleanup.

## Future research — native zOS filesystem

A custom filesystem is deliberately not assigned a roadmap number yet. It should be designed only after zOS gains real-world experience with existing filesystems and discovers concrete requirements. Potential topics include copy-on-write, checksummed metadata and data, snapshots, compression, cloning, transactional updates, native security metadata, named streams, encryption, and recovery behavior.

A native filesystem should exist because zOS has a demonstrated need, not merely because building one sounds fun.
