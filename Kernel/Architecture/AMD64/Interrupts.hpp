#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/VirtualMemory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    inline constexpr Memory::Uint16 KernelCodeSelector{ 0x08 };
    inline constexpr Memory::Uint16 KernelDataSelector{ 0x10 };
    inline constexpr Memory::Uint16 TaskStateSelector{ 0x18 };

    inline constexpr Memory::Uint64 InterruptVectorCount{ 256 };

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
    };

    static_assert(__builtin_offsetof(InterruptContext, Vector) == 15 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(InterruptContext, ErrorCode) == 16 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(InterruptContext, Rip) == 17 * sizeof(Memory::Uint64));
    static_assert(sizeof(InterruptContext) == 20 * sizeof(Memory::Uint64));

    using InterruptHandler = void(*)(InterruptContext& context) noexcept;

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
        InterruptManager() noexcept = default;

        InterruptManager(const InterruptManager&) = delete;
        InterruptManager& operator=(const InterruptManager&) = delete;

        [[nodiscard]] InterruptInitializationError Initialize(Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& virtual_addresses, PageMap& page_map) noexcept;

        [[nodiscard]] bool RegisterHandler(Memory::Uint8 vector, InterruptHandler handler, const char* name = nullptr) noexcept;

        void UnregisterHandler(Memory::Uint8 vector) noexcept;

        void Dispatch(InterruptContext& context) noexcept;

        /*
         * Temporarily installs a handler for #BP, executes INT3,
         * confirms that control returns through IRETQ, and restores
         * the previous handler.
         */
        [[nodiscard]] bool RunBreakpointSelfTest() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::VirtualAddress TrampolineBase() const noexcept { return m_TrampolineVirtual.Base(); }

        [[nodiscard]] const char* HandlerName(Memory::Uint8 vector) const noexcept;

        [[nodiscard]] static const char* Describe(InterruptInitializationError error) noexcept;
    
    private:
        struct HandlerSlot final {
            InterruptHandler Handler{};
            const char* Name{};
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
        } __attribute__((packed));

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
        
        bool m_Initialized{};
    };

    [[noreturn]] void GenericInterruptHandler(InterruptContext& context) noexcept;
}