#pragma once

#include <Kernel/Architecture/AMD64/APIC.hpp>

namespace Zos::Kernel::Scheduling {
    class Thread;
    class Scheduler;
}

namespace Zos::Kernel::Architecture::AMD64 {
    enum class ProcessorLocalInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        InvalidAddress,
        GsBaseWriteFailed,
        KernelGsBaseWriteFailed,
        CurrentProcessorMismatch,
    };

    struct ProcessorLocal final {
        ProcessorLocal* Self{};

        Memory::Uint32 ProcessorIndex{};
        Memory::Uint32 ApicId{};

        ::Zos::Kernel::Scheduling::Thread* CurrentThread{};
        ::Zos::Kernel::Scheduling::Scheduler* LocalScheduler{};

        /*
         * Assembly-visible kernel-entry state. Keep these offsets synchronized
         * with SystemCallEntryAssembly.
         */
        Memory::Uint64 KernelEntryStackTop{};
        Memory::Uint64 SystemCallUserStackPointer{};

        bool BootstrapProcessor{};
        bool Initialized{};

        constexpr ProcessorLocal() noexcept = default;

        ProcessorLocal(const ProcessorLocal&) = delete;
        ProcessorLocal& operator=(const ProcessorLocal&) = delete;

        [[nodiscard]] ProcessorLocalInitializationError Initialize(Memory::Uint32 processor_index, const LocalApic& apic, bool bootstrap_processor) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return Initialized; }
        [[nodiscard]] Memory::VirtualAddress KernelEntryStack() const noexcept { return Memory::VirtualAddress(KernelEntryStackTop); }
        [[nodiscard]] bool SetKernelEntryStack(Memory::VirtualAddress stack_top) noexcept;

        [[nodiscard]] static const char* Describe(ProcessorLocalInitializationError error) noexcept;

    private:
        void Print() const noexcept;
    };

    static_assert(__builtin_offsetof(ProcessorLocal, Self) == 0);
    static_assert(__builtin_offsetof(ProcessorLocal, KernelEntryStackTop) == 32);
    static_assert(__builtin_offsetof(ProcessorLocal, SystemCallUserStackPointer) == 40);

    /*
     * Valid after the current logical processor has installed its GS base.
     *
     * Offset zero is a permanent zOS contract:
     *  - GS:[0] -> current ProcessorLocal*
     */
    [[nodiscard]] ProcessorLocal* CurrentProcessor() noexcept;

    /*
     * Keeps the interrupt privilege stack (TSS RSP0) and the fast-syscall
     * kernel stack pointer synchronized for the current thread.
     */
    [[nodiscard]] bool SetCurrentKernelEntryStack(Memory::VirtualAddress stack_top) noexcept;
    [[nodiscard]] Memory::VirtualAddress CurrentKernelEntryStack() noexcept;
}
