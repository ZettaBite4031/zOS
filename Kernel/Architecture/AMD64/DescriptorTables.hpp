#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    inline constexpr Memory::Uint16 KernelCodeSelector{ 0x08 };
    inline constexpr Memory::Uint16 KernelDataSelector{ 0x10 };
    inline constexpr Memory::Uint16 TaskStateSelector{ 0x18 };
    inline constexpr Memory::Uint16 UserDataSelector{ 0x2B };
    inline constexpr Memory::Uint16 UserCodeSelector{ 0x33 };

    inline constexpr Memory::Uint64 InterruptDescriptorCount{ 256 };

    enum class DescriptorTableInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidTrampoline,
        LoadFailed,
    };

    class DescriptorTables final {
    public:
        constexpr DescriptorTables() noexcept = default;

        DescriptorTables(const DescriptorTables&) = delete;
        DescriptorTables& operator=(const DescriptorTables&) = delete;

        [[nodiscard]] DescriptorTableInitializationError Initialize(
            Memory::VirtualAddress interrupt_trampoline_base,
            Memory::Uint64 interrupt_stub_size
        ) noexcept;

        [[nodiscard]] bool SetKernelPrivilegeStack(Memory::VirtualAddress stack_top) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::VirtualAddress KernelPrivilegeStack() const noexcept {
            return Memory::VirtualAddress(m_Tss.Rsp0);
        }

        [[nodiscard]] static const char* Describe(DescriptorTableInitializationError error) noexcept;

    private:
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
        static_assert(__builtin_offsetof(GlobalDescriptorTable, UserData) == (UserDataSelector & ~Memory::Uint16{ 3 }));
        static_assert(__builtin_offsetof(GlobalDescriptorTable, UserCode) == (UserCodeSelector & ~Memory::Uint16{ 3 }));

        void InitializeGdt() noexcept;
        void InitializeIdt(Memory::VirtualAddress trampoline_base, Memory::Uint64 stub_size) noexcept;
        void SetIdtGate(Memory::Uint8 vector, Memory::VirtualAddress handler, Memory::Uint8 ist) noexcept;

        [[nodiscard]] bool Validate() const noexcept;

        alignas(16)
        IdtEntry m_Idt[InterruptDescriptorCount]{};

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

        bool m_Initialized{};
    };

    [[nodiscard]] bool SetCurrentKernelPrivilegeStack(Memory::VirtualAddress stack_top) noexcept;
    [[nodiscard]] Memory::VirtualAddress CurrentKernelPrivilegeStack() noexcept;
}
