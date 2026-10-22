# Driver, Storage, Networking, and Graphics Direction

This document records long-term driver and I/O decisions. Most of this infrastructure does not exist yet.

## Driver philosophy

Drivers should be loadable modules selected from hardware and dependency information.

Higher layers should consume standardized device capabilities rather than know specific vendor implementations.

Examples:

```text
Intel Ethernet driver → NetworkAdapter
NVMe driver           → BlockDevice
AMD GPU driver        → GraphicsAdapter
USB HID driver        → InputDevice
audio driver          → AudioDevice
```

A networking stack should not need an Intel-specific branch. A compositor should not need to know whether an adapter is AMD, Intel, NVIDIA, VirtIO, or something obscure.

## Driver modules

Kernel drivers should use a distinct privileged module type even if they share lower-level loader machinery with native DLLs.

A future driver manifest may contain:

```text
Name
Version
Driver ABI version
Dependencies
Hardware match rules
Provided interfaces
Required privileges/capabilities
```

The exact on-disk extension and manifest schema are not fixed yet.

The important architectural requirement is that drivers are not permanently compiled into the kernel merely because early bring-up made that convenient.

## Boot-critical modules

Dynamic storage drivers create a chicken-and-egg problem:

> The driver needed to read the system disk cannot be loaded from the system disk.

The planned solution is a small boot-module package loaded by UEFI alongside the kernel.

Conceptually:

```text
BOOTX64.EFI
Kernel.elf
BootModules package
```

The package contains only enough machinery to reach the system volume, such as the required storage-controller, bus, and filesystem modules.

After the system volume is mounted, zOS switches to the complete on-disk module repository and resolves the remaining hardware.

## Pre-user hardware resolution

Before user setup/login, the system should:

```text
discover machine capabilities
        ↓
enumerate installed hardware
        ↓
identify boot-critical devices
        ↓
load boot-module dependency graph
        ↓
mount system volume
        ↓
open full driver repository
        ↓
resolve remaining required drivers
        ↓
start system services
        ↓
start user session
```

The goal is to load the minimum useful module set, not every driver installed on disk.

Drivers may be demand-loaded later when hardware or a subsystem actually needs them.

## Standard device interfaces

Interfaces should be explicit and versioned.

A consumer asks for a capability such as:

```text
BlockDevice v2
NetworkAdapter v1
GraphicsAdapter v3
```

rather than downcasting a generic object to a vendor-specific driver class.

Driver and kernel interface versions should be negotiated explicitly so kernel internals can evolve without silently breaking every driver.

## DMA and IOMMU

DMA must become a first-class I/O concern before real high-throughput devices are trusted.

The long-term design should include:

- explicit DMA buffer ownership
- addressability constraints
- scatter/gather descriptions
- cache/coherency rules
- device-visible mappings
- IOMMU isolation where supported

Drivers should not receive unrestricted physical-memory access merely because that is easy during bring-up.

## Storage

Storage should be layered:

```text
filesystem
    ↓
volume
    ↓
partition
    ↓
BlockDevice
    ↓
NVMe / VirtIO / SATA / USB implementation
```

The first filesystem work should use a RAM-backed test block device so filesystem bugs can be separated from DMA/controller bugs.

Likely progression:

```text
RAM block device
VirtIO-block
NVMe
```

NVMe is the preferred first major real-hardware storage target.

## Filesystems

The generic filesystem layer should expose native zOS concepts rather than Unix VFS terminology.

The intended progression is:

```text
RAMFS
FAT32
FAT12 / FAT16
system-volume integration
exFAT
NTFS
```

FAT variants should share an implementation where sensible.

NTFS is a major later milestone because full support requires substantially more than parsing directory entries.

A custom zOS filesystem is intentionally postponed until the operating system has real experience with caching, writeback, application workloads, package management, permissions, recovery, and compatibility workloads.

## Networking

Networking comes after the system can load normal drivers from its mounted system volume.

The first adapter should be virtual and predictable:

```text
VirtIO-net
```

Then build upward:

```text
Ethernet
ARP
IPv4
ICMP
UDP
DHCP
DNS
TCP
native socket/network API
```

`ping` is a useful early victory condition. Downloading data by hostname over TCP is the more meaningful one.

A first physical Ethernet driver should target common, well-documented hardware.

Wi-Fi is a separate major milestone because it adds 802.11 state, authentication, encryption, firmware, regulatory behavior, roaming, and power management.

## USB, input, audio, and power

After the general driver model, storage, and networking are healthy, everyday hardware support can expand into:

- xHCI USB
- USB HID
- keyboard/mouse/gamepad input
- ACPI power management
- battery and device power states
- audio
- Bluetooth

These are not secondary polish. They are part of the path from "kernel" to "computer someone can live in."

## Basic display

Graphics must have a safe fallback.

The first display path should preserve or establish a simple framebuffer using UEFI GOP information.

Without an accelerated driver, zOS should still be able to offer:

```text
basic display
software rendering
desktop compositor
```

A missing vendor GPU driver should degrade performance and capabilities, not produce a black screen.

## Accelerated graphics split

Modern graphics should use a split architecture.

```text
Application
    ↓
Vulkan / OpenGL / native graphics API
    ↓
user-mode GPU implementation
    ↓
zOS GPU interface
    ↓
hardware-facing graphics system driver
    ↓
GPU
```

The user-mode implementation is extremely important.

It may own:

- shader compilation
- pipeline creation
- descriptors
- resource layouts
- command-buffer construction
- GPU-specific packet generation
- API semantics
- performance policy

The system/kernel-facing driver owns privileged hardware concerns such as:

- PCI/MMIO ownership
- firmware
- interrupts
- DMA
- GPU virtual-memory machinery
- queues/engines
- fences
- reset/recovery
- display hardware
- power transitions

The kernel should not need to understand vendor command-buffer contents merely to schedule them.

## Graphics policy belongs to zOS

The generic zOS graphics layer should own policy where practical:

- process/context ownership
- GPU scheduling
- memory accounting
- residency
- synchronization
- isolation
- presentation
- timeout detection and recovery

Vendor drivers describe what hardware can do and implement how to do it.

zOS decides how resources are shared.

## GPU virtual memory

Processes should eventually have isolated GPU address spaces just as they have isolated CPU address spaces.

The generic system should deal in logical GPU mappings; the vendor system driver translates those operations into the GPU's actual page-table format.

## Graphics bring-up order

Preferred progression:

```text
UEFI GOP framebuffer
        ↓
software graphics
        ↓
window server/compositor
        ↓
generic zOS graphics kernel/interface
        ↓
VirtIO-GPU
        ↓
user-mode driver interface
        ↓
Mesa integration where practical
        ↓
real AMD / Intel / NVIDIA drivers
```

VirtIO-GPU should validate the architecture before real GPU complexity is introduced.

## Open-source references

When real GPU work begins, existing implementations should be studied for architecture and hardware behavior.

Useful families include:

- Windows WDDM documentation
- Linux DRM
- Mesa user-mode drivers
- AMDGPU / RADV
- Intel xe / ANV
- Nouveau / NVK
- NVIDIA open kernel modules
- Fuchsia Magma
- smaller open stacks such as V3D/V3DV, Freedreno/Turnip, and Panfrost/PanVK

zOS should learn from them without allowing another operating system's driver framework to become the zOS driver framework by default.

## Recovery

Driver interfaces should be designed with failure in mind.

For graphics in particular, the desired long-term behavior is:

```text
GPU hang
   ↓
detect timeout
   ↓
stop submissions
   ↓
reset engine or adapter
   ↓
invalidate/rebuild affected contexts
   ↓
terminate offending process if necessary
   ↓
desktop continues
```

The same philosophy applies more broadly: a device failure should be contained at the narrowest practical boundary.
