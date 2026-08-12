#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>
#include <Kernel/Memory/VirtualMemory.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Architecture/AMD64/Interrupts.hpp>

namespace Zos::Kernel {
    struct BootContext final {
        /*
         * Permanent kernel image ownership.
         */
        Memory::PhysicalSpan KernelImage{};

        /*
         * Temporary bootstrap resources which remain reserved
         * until later roadmap milestones explicitly replace them.
         */
        Memory::PhysicalSpan BootstrapStack{};
        Memory::PhysicalSpan EnvironmentStorage{};
        Memory::PhysicalSpan MemoryMapStorage{};

        /*
         * Firmware-independent physical root for later ACPI
         * initialization
         * 
         * The underlying ACPI memory remains DeferredAcpi until
         * the ACPI subsystem has consumed it.
         */
        Memory::PhysicalAddress AcpiRsdp{};

        bool Initialized{};
    };

    enum class KernelPhase : Memory::Uint32 {
        Entry,
        BootEnvironmentValidated,
        PhysicalMemoryReady,
        VirtualMemoryReady,
        AddressSpaceActive,
        InterruptsReady,
        BootMemoryReclaimed,
        BootContextInternalized,
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
        BootContext Boot{};
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
