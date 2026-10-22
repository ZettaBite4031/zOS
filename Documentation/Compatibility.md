# Native Compatibility Direction

Compatibility is a long-term zOS feature, not a current capability.

It is also intended to become one of the operating system's strongest differentiators.

The goal is not to host miniature copies of Windows and Linux above zOS. The goal is to make foreign software call native zOS facilities through narrow translation layers.

## Core rule

> Compatibility ends at the native zOS API boundary.

Preferred:

```text
Windows / Linux API or ABI
          ↓
      translation
          ↓
      zOS Native API
          ↓
     kernel / services
```

Avoid:

```text
foreign application
      ↓
reimplemented foreign userspace
      ↓
reimplemented foreign kernel
      ↓
zOS
```

Some foreign behavior will necessarily be reproduced because real software depends on it, but compatibility work should not redefine zOS internals.

## Logical modules

The future module loader should distinguish:

```text
Module Identity
```

from:

```text
Module Implementation
```

A process may request a foreign module identity while zOS resolves it to a native implementation.

Example:

```text
kernel32.dll!CreateFileW
            ↓
logical module resolver
            ↓
native compatibility implementation
            ↓
zOS file/object API
```

There does not need to be a physical forwarding DLL named `kernel32.dll` if the loader can represent the identity virtually.

The same idea applies to Linux shared-object identities:

```text
libc.so.6!open
      ↓
Linux compatibility contract
      ↓
native zOS compatibility module
      ↓
zOS file API
```

This keeps compatibility compact and avoids forests of tiny forwarding libraries.

## Native module system

The native module infrastructure should eventually support:

- imports and exports
- symbol lookup
- relocations
- versioning
- initialization and shutdown
- dependency metadata
- logical aliases/contracts
- delayed resolution where useful
- physical and virtual modules
- trust/type metadata

User DLLs, driver modules, and compatibility implementations may share parts of this machinery while keeping separate privilege and load policies.

## Windows compatibility

Windows binaries are normally more dependent on DLL APIs than on issuing raw NT syscalls directly.

That makes import-level translation attractive.

A PE executable may ask for identities such as:

```text
kernel32.dll
kernelbase.dll
user32.dll
gdi32.dll
advapi32.dll
ntdll.dll
api-ms-win-*.dll
```

zOS should be able to present these as logical module contracts backed by a smaller number of real compatibility modules.

Example:

```text
CreateFileW
    ↓
Windows ABI entry stub
    ↓
argument / flag / path translation
    ↓
native zOS file operation
```

The Windows x64 calling convention differs from the SysV AMD64 convention currently used by ordinary zOS C++ code, so ABI thunks or functions built with the appropriate ABI will still be needed.

Those thunks should be generated or centralized rather than represented as one physical DLL per foreign identity.

Long-term compatibility work may include:

- PE/COFF user executable loading
- Windows x64 ABI entry points
- logical DLL and API-set contracts
- Win32 file/process/thread APIs
- synchronization
- registry/configuration translation
- windowing and graphics APIs
- COM and other higher-level facilities where real software requires them

The first goal should be ordinary applications, not kernel drivers, invasive anti-cheat, or every undocumented Windows behavior.

## Linux compatibility

Dynamically linked Linux software often reaches the operating system through libc and related runtimes.

The preferred path is:

```text
Linux application
      ↓
libc / pthread / dl logical contracts
      ↓
zOS Linux compatibility runtime
      ↓
zOS Native API
```

A single substantial compatibility module may present multiple expected Linux SONAMEs such as:

```text
libc.so.6
libpthread.so.0
libdl.so.2
librt.so.1
```

These identities do not have to correspond one-to-one with physical files.

## Direct Linux syscalls

Linux programs may execute `syscall` directly, especially static programs and language runtimes.

Therefore zOS will eventually need a Linux syscall ABI adapter.

It should remain an adapter:

```text
Linux syscall number + Linux argument rules
               ↓
        Linux ABI decoder
               ↓
         native zOS operation
               ↓
     Linux result / errno translation
```

It should not become a second scheduler, VFS, process model, or virtual Linux kernel unless a specific compatibility requirement forces localized emulation.

## Language runtimes

Portable runtimes can reduce compatibility work dramatically.

A foreign application may initially run through a foreign runtime:

```text
Linux application
      ↓
Linux runtime
      ↓
compatibility layer
      ↓
zOS
```

but mature support should prefer a native zOS runtime when practical:

```text
portable application / bytecode
      ↓
native zOS runtime
      ↓
zOS
```

High-value long-term runtime targets include C/C++, .NET, Python, Java, and Rust support.

## Filesystem and namespace views

Foreign path syntax should be a view over native zOS objects rather than a reason to redesign the kernel namespace.

Conceptually:

```text
native zOS namespace
       ↑
       ├── Windows path view
       └── Linux path view
```

Windows drive-letter semantics, Linux root semantics, case rules, symbolic links, and similar behavior can be translated at the compatibility boundary.

## Process ABI identity

Future processes may carry an execution ABI identity such as:

```text
Native zOS
Windows x64
Linux x86-64
```

This identity can guide executable loading, module lookup, syscall interpretation, exception/runtime conventions, path views, and compatibility versioning.

Do not let this become three separate process implementations. The native process, thread, scheduler, memory manager, and object system remain shared.

## What not to do

Do not:

- put Windows syscall numbers directly into AMD64 syscall entry code
- expose Linux file descriptors as the native zOS object model
- duplicate foreign kernels wholesale
- require one physical forwarding module per DLL or SONAME
- make compatibility code privileged merely because the original OS did
- sacrifice native API coherence to make one foreign program easier to port

Compatibility should be broad, aggressive, and practical, but always layered over zOS rather than replacing it.
