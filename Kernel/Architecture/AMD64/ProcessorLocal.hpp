#pragma once

#include <Kernel/Architecture/AMD64/APIC.hpp>

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

        bool BootstrapProcessor{};
        bool Initialized{};

        constexpr ProcessorLocal() noexcept = default;

        ProcessorLocal(const ProcessorLocal&) = delete;
        ProcessorLocal& operator=(const ProcessorLocal&) = delete;

        [[nodiscard]] ProcessorLocalInitializationError Initialize(Memory::Uint32 processor_index, const LocalApic& apic, bool bootstrap_processor) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return Initialized; }

        [[nodiscard]] static const char* Describe(ProcessorLocalInitializationError error) noexcept;

    private:
        void Print() const noexcept;
    };

    static_assert(__builtin_offsetof(ProcessorLocal, Self) == 0);

    /*
     * Valid after the current logical processor has installed its GS base.
     *
     * Offset zero is a permanent zOS contract:
     *  - GS:[0] -> current ProcessorLocal*
     */
    [[nodiscard]] ProcessorLocal* CurrentProcessor() noexcept;
}
