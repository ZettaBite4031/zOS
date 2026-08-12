#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>
#include <Kernel/Memory/VirtualMemory.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Architecture/AMD64/Interrupts.hpp>

namespace Zos::Kernel {
    enum class KernelPhase : Memory::Uint32 {
        Entry,
        BootEnvironmentValidated,
        PhysicalMemoryReady,
        VirtualMemoryReady,
        AddressSpaceActive,
        InterruptsReady,
        BootMemoryReclaimed,
        BootstrapComplete,
        Runtime,
    };

    /*
     * Permanent ownership root for kernel-lifetime infrastructure.
     * 
     * This is intentionally NOT a general service locator. Runtime
     * subsystems should still receive explicit dependencies rather
     * than calling GetKernelRuntime() from arbitrary code.
     * 
     * Its primary responsibilities are:
     *  - permanent object lifetime,
     *  - explicit initialization ordering,
     *  - removing long-lived objects from the bootstrap stack.
     */
    struct KernelRuntime final {
        Memory::PhysicalMemoryManager PhysicalMemory{};
        Memory::BootstrapMetadataArena BootstrapMetadata{};
        Memory::VirtualAddressAllocator KernelAddresses{};
        Architecture::AMD64::PageMap KernelPageMap{};
        Architecture::AMD64::InterruptManager Interrupts{};

        KernelPhase Phase{ KernelPhase::Entry };
    };

    /*
     * Intended for startup ownership/orchestration only.
     * 
     * Do not use this as a globale dependency accessor throughout
     * normal kernel code.
     */
    [[nodiscard]] KernelRuntime& GetKernelRuntime() noexcept;
}
