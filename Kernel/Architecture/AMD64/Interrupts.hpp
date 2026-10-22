#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class LocalApic;

    inline constexpr Memory::Uint16 KernelCodeSelector{ 0x08 };
    inline constexpr Memory::Uint16 KernelDataSelector{ 0x10 };
    inline constexpr Memory::Uint16 TaskStateSelector{ 0x18 };

    inline constexpr Memory::Uint16 UserDataSelector{ 0x2B };
    inline constexpr Memory::Uint16 UserCodeSelector{ 0x33 };

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
        /*
         * Order MUST exactly match InterruptCommonEntry.
         */

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
         * 
         * ErrorCode is real for exceptions that supply
         * one, and zero for interrupts that do not.
         */
        Memory::Uint64 Vector;
        Memory::Uint64 ErrorCode;

        /* 
         * Hardware interrupt frame.
         */
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

        [[nodiscard]] InterruptInitializationError Initialize(Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& virtual_addresses, PageMap& page_map) noexcept;

        [[nodiscard]] bool BindLocalApic(LocalApic& local_apic) noexcept;

        [[nodiscard]] bool RegisterFixedHandler(Memory::Uint8 vector, InterruptHandler handler, const char* name = nullptr, InterruptAcknowledgement acknowledgement = InterruptAcknowledgement::None) noexcept;

        [[nodiscard]] bool AllocateDeviceVector(Memory::Uint8& vector, InterruptHandler handler, const char* name = nullptr) noexcept;

        [[nodiscard]] bool ReleaseDeviceVector(Memory::Uint8 vector) noexcept;

        void Dispatch(InterruptContext& context) noexcept;

        [[nodiscard]] bool SetKernelPrivilegeStack(Memory::VirtualAddress stack_top) noexcept;

        /*
         * Temporarily installs a handler for #BP, executes INT3,
         * confirms that control returns through IRETQ, and restores
         * the previous handler.
         */
        [[nodiscard]] bool RunBreakpointSelfTest() noexcept;
        
        [[nodiscard]] bool RunVectorPolicySelfTest() noexcept;

        [[nodiscard]] static InterruptVectorClass VectorClass(Memory::Uint8 vector) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::VirtualAddress TrampolineBase() const noexcept { return m_TrampolineVirtual.Base(); }
        [[nodiscard]] Memory::VirtualAddress KernelPrivilegeStack() const noexcept { return Memory::VirtualAddress(m_Tss.Rsp0); }

        [[nodiscard]] const char* HandlerName(Memory::Uint8 vector) const noexcept;

        [[nodiscard]] static const char* Describe(InterruptInitializationError error) noexcept;
    
    private:
        struct HandlerSlot final {
            InterruptHandler Handler{};
            const char* Name{};
            InterruptAcknowledgement Acknowledgement{ InterruptAcknowledgement::None };
            bool Claimed{};
        };

        struct IdtEntry final {
            Memory::Uint16 OffsetLow{};
            Memory::Uint16 Selector{};
            Memory::Uint8 Ist{};
            Memory::Uint8 Attributes{};
            Memory::Uint16 OffsetMiddle{};
            Memory::Uint32 OffsetHigh{};
            Memory::Uint32 Reserved{};
        } __attribute__((packed));

        static_assert(sizeof(IdtEntry) == 16);

        struct DescriptorTablePointer final {
            Memory::Uint16 Limit{};
            Memory::Uint64 Base{};
        } __attribute__((packed));

        static_assert(sizeof(DescriptorTablePointer) == 10);

        struct TaskStateSegment final {
            Memory::Uint32 Reserved0{};

            Memory::Uint64 Rsp0{};
            Memory::Uint64 Rsp1{};
            Memory::Uint64 Rsp2{};

            Memory::Uint64 Reserved1{};

            Memory::Uint64 Ist1{};
            Memory::Uint64 Ist2{};
            Memory::Uint64 Ist3{};
            Memory::Uint64 Ist4{};
            Memory::Uint64 Ist5{};
            Memory::Uint64 Ist6{};
            Memory::Uint64 Ist7{};

            Memory::Uint64 Reserved2{};
            Memory::Uint16 Reserved3{};
            Memory::Uint16 IoMapBase{};
        } __attribute__((packed));

        static_assert(sizeof(TaskStateSegment) == 104);

        struct TssDescriptor final {
            Memory::Uint16 LimitLow{};
            Memory::Uint16 BaseLow{};
            Memory::Uint8 BaseMiddle{};
            Memory::Uint8 Access{};
            Memory::Uint8 LimitHighFlags{};
            Memory::Uint8 BaseHigh{};
            Memory::Uint32 BaseUpper{};
            Memory::Uint32 Reserved{};
        } __attribute__((packed));

        static_assert(sizeof(TssDescriptor) == 16);

        struct GlobalDescriptorTable final {
            Memory::Uint64 Null{};
            Memory::Uint64 KernelCode{};
            Memory::Uint64 KernelData{};
            TssDescriptor Tss{};
            Memory::Uint64 UserData{};
            Memory::Uint64 UserCode{};
        } __attribute__((packed));

        static_assert(__builtin_offsetof(GlobalDescriptorTable, Tss) == TaskStateSelector);
        static_assert(__builtin_offsetof(GlobalDescriptorTable, UserData) == (UserDataSelector & ~Memory::Uint16{3}));
        static_assert(__builtin_offsetof(GlobalDescriptorTable, UserCode) ==(UserCodeSelector & ~Memory::Uint16{3}));
        
        void InitializeHandlers() noexcept;
        void RegisterBuiltinHandlers() noexcept;

        void InitializeGdt() noexcept;
        void InitializeIdt() noexcept;

        [[nodiscard]] InterruptInitializationError InitializeTrampolines() noexcept;
        [[nodiscard]] bool BuildRuntimeTrampolines() noexcept;
        [[nodiscard]] bool SealRuntimeTrampolines() noexcept;

        void SetIdtGate(Memory::Uint8 vector, Memory::VirtualAddress handler, Memory::Uint8 ist) noexcept;

        [[nodiscard]] bool ValidateDescriptorTables() const noexcept;

        HandlerSlot m_Handlers[InterruptVectorCount]{};

        alignas(16)
        IdtEntry m_Idt[InterruptVectorCount]{};

        alignas(16)
        GlobalDescriptorTable m_Gdt{};

        alignas(16)
        TaskStateSegment m_Tss{};

        /*
         * Temporary early-kernel #DF stack.
         *
         * This should later become a VMM-owned stack with guard pages.
         */
        alignas(16)
        Memory::Uint8 m_DoubleFaultStack[16 * 1024]{};

        Memory::PhysicalAllocation m_TrampolineBacking{};
        Memory::VirtualReservation m_TrampolineVirtual{};

        Memory::PhysicalMemoryManager* m_PhysicalMemory{};
        Memory::VirtualAddressAllocator* m_VirtualAddresses{};
        PageMap* m_PageMap{};
        LocalApic* m_LocalApic{};
        
        bool m_Initialized{};
    };

    [[noreturn]] void GenericInterruptHandler(InterruptContext& context) noexcept;

    [[nodiscard]] bool SetCurrentKernelPrivilegeStack(Memory::VirtualAddress stack_topc ) noexcept;
    [[nodiscard]] Memory::VirtualAddress CurrentKernelPrivilegeStack() noexcept;
}