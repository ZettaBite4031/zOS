#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>

namespace Zos::Kernel {
    struct UserFault;
}

namespace Zos::Kernel::Architecture::AMD64 {
    class LocalApic;
    class DescriptorTables;

    inline constexpr Memory::Uint64 InterruptVectorCount{ 256 };

    inline constexpr Memory::Uint8 ExceptionVectorFirst{ 0x00 };
    inline constexpr Memory::Uint8 ExceptionVectorLast{ 0x1F };

    inline constexpr Memory::Uint8 DeviceVectorFirst{ 0x20 };
    inline constexpr Memory::Uint8 DeviceVectorLast{ 0xDF };

    inline constexpr Memory::Uint8 LocalApicVectorFirst{ 0xE0 };
    inline constexpr Memory::Uint8 LocalApicVectorLast{ 0xEF };

    inline constexpr Memory::Uint8 LocalApicTimerVector{ LocalApicVectorFirst };

    inline constexpr Memory::Uint8 IpiVectorFirst{ 0xF0 };
    inline constexpr Memory::Uint8 IpiVectorLast{ 0xFE };

    inline constexpr Memory::Uint8 SpuriousInterruptVector{ 0xFF };

    struct InterruptContext final {
        /* Order MUST exactly match InterruptCommonEntry. */
        Memory::Uint64 R15;
        Memory::Uint64 R14;
        Memory::Uint64 R13;
        Memory::Uint64 R12;
        Memory::Uint64 R11;
        Memory::Uint64 R10;
        Memory::Uint64 R9;
        Memory::Uint64 R8;

        Memory::Uint64 Rsi;
        Memory::Uint64 Rdi;
        Memory::Uint64 Rbp;
        Memory::Uint64 Rdx;
        Memory::Uint64 Rcx;
        Memory::Uint64 Rbx;
        Memory::Uint64 Rax;

        /*
         * Normalized by the runtime trampoline.
         * ErrorCode is real for exceptions that supply one and zero otherwise.
         */
        Memory::Uint64 Vector;
        Memory::Uint64 ErrorCode;

        /* Hardware interrupt frame. */
        Memory::Uint64 Rip;
        Memory::Uint64 Cs;
        Memory::Uint64 Rflags;

        [[nodiscard]] bool FromUserMode() const noexcept { return (Cs & 0x3ULL) == 0x3ULL; }
    };

    static_assert(__builtin_offsetof(InterruptContext, Vector) == 15 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(InterruptContext, ErrorCode) == 16 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(InterruptContext, Rip) == 17 * sizeof(Memory::Uint64));
    static_assert(sizeof(InterruptContext) == 20 * sizeof(Memory::Uint64));

    using InterruptHandler = void(*)(InterruptContext& context) noexcept;
    using UserFaultHandler = void(*)(const ::Zos::Kernel::UserFault& fault) noexcept;

    enum class InterruptVectorClass : Memory::Uint8 {
        Exception,
        Device,
        LocalApic,
        Ipi,
        Spurious,
    };

    enum class InterruptAcknowledgement : Memory::Uint8 {
        None,
        LocalApicEoi,
        LocalApicEoiBeforeHandler,
    };

    enum class InterruptInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        InterruptsEnabled,
        PhysicalAllocationFailed,
        VirtualAllocationFailed,
        MappingFailed,
        TrampolineGenerationFailed,
        ProtectionFailed,
        DescriptorLoadFailed,
    };

    class InterruptManager final {
    public:
        constexpr InterruptManager() noexcept = default;

        InterruptManager(const InterruptManager&) = delete;
        InterruptManager& operator=(const InterruptManager&) = delete;

        [[nodiscard]] InterruptInitializationError Initialize(
            Memory::PhysicalMemoryManager& physical_memory,
            Memory::VirtualAddressAllocator& virtual_addresses,
            PageMap& page_map,
            DescriptorTables& descriptor_tables
        ) noexcept;

        [[nodiscard]] bool BindLocalApic(LocalApic& local_apic) noexcept;
        [[nodiscard]] bool BindUserFaultHandler(UserFaultHandler handler) noexcept;

        [[nodiscard]] bool RegisterFixedHandler(
            Memory::Uint8 vector,
            InterruptHandler handler,
            const char* name = nullptr,
            InterruptAcknowledgement acknowledgement = InterruptAcknowledgement::None
        ) noexcept;

        [[nodiscard]] bool AllocateDeviceVector(
            Memory::Uint8& vector,
            InterruptHandler handler,
            const char* name = nullptr
        ) noexcept;

        [[nodiscard]] bool ReleaseDeviceVector(Memory::Uint8 vector) noexcept;

        void Dispatch(InterruptContext& context) noexcept;

        /*
         * Temporarily installs a handler for #BP, executes INT3, confirms that
         * control returns through IRETQ, and restores the previous handler.
         */
        [[nodiscard]] bool RunBreakpointSelfTest() noexcept;
        [[nodiscard]] bool RunVectorPolicySelfTest() noexcept;

        [[nodiscard]] static InterruptVectorClass VectorClass(Memory::Uint8 vector) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::VirtualAddress TrampolineBase() const noexcept { return m_TrampolineVirtual.Base(); }

        [[nodiscard]] const char* HandlerName(Memory::Uint8 vector) const noexcept;
        [[nodiscard]] static const char* Describe(InterruptInitializationError error) noexcept;

    private:
        struct HandlerSlot final {
            InterruptHandler Handler{};
            const char* Name{};
            InterruptAcknowledgement Acknowledgement{ InterruptAcknowledgement::None };
            bool Claimed{};
        };

        void InitializeHandlers() noexcept;
        void RegisterBuiltinHandlers() noexcept;

        [[nodiscard]] InterruptInitializationError InitializeTrampolines() noexcept;
        [[nodiscard]] bool BuildRuntimeTrampolines() noexcept;
        [[nodiscard]] bool SealRuntimeTrampolines() noexcept;

        HandlerSlot m_Handlers[InterruptVectorCount]{};

        Memory::PhysicalAllocation m_TrampolineBacking{};
        Memory::VirtualReservation m_TrampolineVirtual{};

        Memory::PhysicalMemoryManager* m_PhysicalMemory{};
        Memory::VirtualAddressAllocator* m_VirtualAddresses{};
        PageMap* m_PageMap{};
        LocalApic* m_LocalApic{};
        UserFaultHandler m_UserFaultHandler{};

        bool m_Initialized{};
    };

    [[noreturn]] void GenericInterruptHandler(InterruptContext& context) noexcept;
}
