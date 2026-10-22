#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>
#include <Kernel/Memory/KernelAddressSpace.hpp>
#include <Kernel/Memory/KernelHeap.hpp>
#include <Kernel/Memory/KernelStack.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/Interrupts.hpp>
#include <Kernel/Architecture/AMD64/CPU.hpp>
#include <Kernel/Architecture/AMD64/APIC.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Platform/ACPI/ACPI.hpp>
#include <Kernel/Platform/ACPI/MADT.hpp>

#include <Kernel/Platform/HPET.hpp>

#include <Kernel/Time/Clock.hpp>
#include <Kernel/Time/LocalApicTimer.hpp>

#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>

#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel {
    struct BootContext final {
        /*
         * Permanent kernel image ownership.
         */
        Memory::PhysicalSpan KernelImage{};

        /*
         * Loader-provided bootstrap resources.
         *
         * These are copied here only so the permanent-stack transition
         * can retire them without consulting BootEnvironment again.
         * They are cleared after release.
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

        PhysicalMemoryMetadataPromoted,

        InterruptsReady,
        BootMemoryReclaimed,
        BootContextInternalized,

        PermanentStackPrepared,
        PermanentStackActive,
        BootstrapResourcesReleased,
        KernelHeapReady,
        CxxAllocationReady,
        VirtualAddressMetadataPromoted,
        PageMapMetadataPromoted,
        BootstrapMetadataRetired,

        BootstrapComplete,

        AcpiReady,
        InterruptTopologyReady,
        CpuCapabilitiesReady,
        LegacyInterruptsQuiesced,
        LocalApicReady,
        InterruptPolicyReady,
        MonotonicClockReady,
        ProcessorLocalReady,
        KernelProcessReady,
        KernelThreadsReady,
        CooperativeContextSwitchReady,
        SchedulerReady,
        LocalApicTimerReady,
        PreemptionReady,
        ProcessAddressSpacesReady,
        Ring3Ready,
        UserFaultIsolationReady,

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
        Memory::VirtualAddressAllocator KernelMmioAddresses{};
        Architecture::AMD64::PageMap KernelPageMap{};
        Architecture::AMD64::DescriptorTables Descriptors{};
        Architecture::AMD64::InterruptManager Interrupts{};
        Architecture::AMD64::CpuInfo Cpu{};
        Architecture::AMD64::LocalApic Apic{};
        Architecture::AMD64::ProcessorLocal BootstrapProcessor{};
        Memory::KernelHeap Heap{};

        Platform::ACPI::TableDirectory Acpi{};
        Platform::ACPI::Madt Madt{};
        Platform::Hpet Hpet{};

        Time::MonotonicClock Clock{};
        Time::LocalApicTimer Timer{};

        Memory::PhysicalAllocation FirstUserCodePage{};
        Memory::PhysicalAllocation FirstUserDataPage{};
        Memory::PhysicalAllocation FirstUserStackBacking{};

        /*
         * Permanent bootstrap/runtime stack until kernel threads
         * introduce per-thread kernel stacks.
         */
        Memory::KernelStack PrimaryStack{};

        Processes::Process KernelProcess{};
        Processes::Process FirstUserProcess{};

        Scheduling::Thread BootstrapThread{};
        Scheduling::Thread FirstWorkerThread{};
        Scheduling::Thread FirstUserThread{};

        Scheduling::Scheduler BootstrapScheduler{};

        KernelPhase Phase{ KernelPhase::Entry };
    };

    /*
     * Intended for startup ownership/orchestration only.
     *
     * Do not use this as a global dependency accessor throughout
     * normal kernel code.
     */
    [[nodiscard]] KernelRuntime& GetKernelRuntime() noexcept;
}
