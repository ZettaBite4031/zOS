#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>

extern "C" void LoadDescriptorTables(const void* gdtr, const void* idtr) noexcept;

/*
 * Installs the zOS GDT, reloads the kernel segment selectors, loads TR with
 * the zOS TSS, and installs the IDT.
 *
 * RDI = GDTR*
 * RSI = IDTR*
 */
__asm__(
    ".pushsection .text.LoadDescriptorTables,\"ax\",@progbits\n"
    ".global LoadDescriptorTables\n"
    ".type LoadDescriptorTables,@function\n"

    "LoadDescriptorTables:\n"
    "   lgdt (%rdi)\n"

    "   movw $0x10, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   movw %ax, %ss\n"

    "   pushq $0x08\n"
    "   leaq 1f(%rip), %rax\n"
    "   pushq %rax\n"
    "   lretq\n"

    "1:\n"
    "   movw $0x18, %ax\n"
    "   ltr %ax\n"
    "   lidt (%rsi)\n"
    "   ret\n"

    ".size LoadDescriptorTables, .-LoadDescriptorTables\n"
    ".popsection\n"
);

namespace Zos::Kernel::Architecture::AMD64 {
    namespace {
        DescriptorTables* g_CurrentDescriptorTables{};
    }

    void DescriptorTables::InitializeGdt() noexcept {
        constexpr Memory::Uint64 KernelCodeDescriptor{ 0x00AF9A000000FFFFULL };
        constexpr Memory::Uint64 KernelDataDescriptor{ 0x00CF92000000FFFFULL };
        constexpr Memory::Uint64 UserDataDescriptor{ 0x00CFF2000000FFFFULL };
        constexpr Memory::Uint64 UserCodeDescriptor{ 0x00AFFA000000FFFFULL };

        m_Gdt = {};
        m_Tss = {};

        m_Gdt.KernelCode = KernelCodeDescriptor;
        m_Gdt.KernelData = KernelDataDescriptor;
        m_Gdt.UserData = UserDataDescriptor;
        m_Gdt.UserCode = UserCodeDescriptor;

        m_Tss.Ist1 = reinterpret_cast<Memory::Uint64>(m_DoubleFaultStack + sizeof(m_DoubleFaultStack));
        m_Tss.Rsp0 = 0;
        m_Tss.IoMapBase = static_cast<Memory::Uint16>(sizeof(TaskStateSegment));

        const Memory::Uint64 tss_base = reinterpret_cast<Memory::Uint64>(&m_Tss);
        constexpr Memory::Uint32 TssLimit = sizeof(TaskStateSegment) - 1;

        TssDescriptor& descriptor = m_Gdt.Tss;
        descriptor.LimitLow = static_cast<Memory::Uint16>(TssLimit);
        descriptor.BaseLow = static_cast<Memory::Uint16>(tss_base);
        descriptor.BaseMiddle = static_cast<Memory::Uint8>(tss_base >> 16);
        descriptor.Access = 0x89; // Present, DPL0, available 64-bit TSS.
        descriptor.LimitHighFlags = static_cast<Memory::Uint8>((TssLimit >> 16) & 0x0F);
        descriptor.BaseHigh = static_cast<Memory::Uint8>(tss_base >> 24);
        descriptor.BaseUpper = static_cast<Memory::Uint32>(tss_base >> 32);
        descriptor.Reserved = 0;
    }

    void DescriptorTables::SetIdtGate(Memory::Uint8 vector, Memory::VirtualAddress handler, Memory::Uint8 ist) noexcept {
        IdtEntry& entry = m_Idt[vector];
        entry = {};

        const Memory::Uint64 address = handler.Value();
        entry.OffsetLow = static_cast<Memory::Uint16>(address);
        entry.Selector = KernelCodeSelector;
        entry.Ist = ist & 0x07;
        entry.Attributes = 0x8E; // Present, DPL0, 64-bit interrupt gate.
        entry.OffsetMiddle = static_cast<Memory::Uint16>(address >> 16);
        entry.OffsetHigh = static_cast<Memory::Uint32>(address >> 32);
        entry.Reserved = 0;
    }

    void DescriptorTables::InitializeIdt(Memory::VirtualAddress trampoline_base, Memory::Uint64 stub_size) noexcept {
        for (Memory::Uint64 vector = 0; vector < InterruptDescriptorCount; vector++) {
            const Memory::VirtualAddress stub = trampoline_base + vector * stub_size;

            /*
             * #DF receives IST1. Other vectors initially use the current
             * kernel stack. NMI/#MC can receive dedicated IST stacks later.
             */
            const Memory::Uint8 ist = vector == 8 ? 1 : 0;
            SetIdtGate(static_cast<Memory::Uint8>(vector), stub, ist);
        }
    }

    bool DescriptorTables::Validate() const noexcept {
        DescriptorTablePointer loaded_gdt{};
        DescriptorTablePointer loaded_idt{};

        Memory::Uint16 task_register{};
        Memory::Uint16 code_segment{};
        Memory::Uint16 stack_segment{};

        __asm__ volatile("sgdt %0" : "=m"(loaded_gdt));
        __asm__ volatile("sidt %0" : "=m"(loaded_idt));
        __asm__ volatile("str %0" : "=r"(task_register));
        __asm__ volatile("mov %%cs, %0" : "=r"(code_segment));
        __asm__ volatile("mov %%ss, %0" : "=r"(stack_segment));

        return loaded_gdt.Base == reinterpret_cast<Memory::Uint64>(&m_Gdt)
            && loaded_gdt.Limit == sizeof(m_Gdt) - 1
            && loaded_idt.Base == reinterpret_cast<Memory::Uint64>(&m_Idt[0])
            && loaded_idt.Limit == sizeof(m_Idt) - 1
            && task_register == TaskStateSelector
            && code_segment == KernelCodeSelector
            && stack_segment == KernelDataSelector;
    }

    DescriptorTableInitializationError DescriptorTables::Initialize(
        Memory::VirtualAddress interrupt_trampoline_base,
        Memory::Uint64 interrupt_stub_size
    ) noexcept {
        if (m_Initialized || g_CurrentDescriptorTables != nullptr)
            return DescriptorTableInitializationError::AlreadyInitialized;

        if (interrupt_trampoline_base.IsNull() ||
            !PageMap::IsCanonical(interrupt_trampoline_base) ||
            interrupt_stub_size == 0)
            return DescriptorTableInitializationError::InvalidTrampoline;

        const Memory::Uint64 last_offset = (InterruptDescriptorCount - 1) * interrupt_stub_size;
        if (last_offset > Memory::MaximumValue - interrupt_trampoline_base.Value() ||
            !PageMap::IsCanonical(Memory::VirtualAddress(interrupt_trampoline_base.Value() + last_offset)))
            return DescriptorTableInitializationError::InvalidTrampoline;

        InitializeGdt();
        InitializeIdt(interrupt_trampoline_base, interrupt_stub_size);

        const DescriptorTablePointer gdtr{
            .Limit = static_cast<Memory::Uint16>(sizeof(m_Gdt) - 1),
            .Base = reinterpret_cast<Memory::Uint64>(&m_Gdt),
        };

        const DescriptorTablePointer idtr{
            .Limit = static_cast<Memory::Uint16>(sizeof(m_Idt) - 1),
            .Base = reinterpret_cast<Memory::Uint64>(&m_Idt[0]),
        };

        /*
         * Publish the active table owner before LIDT. Architectural events can
         * arrive independently of IF once these tables become active.
         */
        g_CurrentDescriptorTables = this;
        LoadDescriptorTables(&gdtr, &idtr);

        if (!Validate()) {
            g_CurrentDescriptorTables = nullptr;
            return DescriptorTableInitializationError::LoadFailed;
        }

        m_Initialized = true;
        return DescriptorTableInitializationError::Success;
    }

    bool DescriptorTables::SetKernelPrivilegeStack(Memory::VirtualAddress stack_top) noexcept {
        if (!m_Initialized || stack_top.IsNull() || !PageMap::IsCanonical(stack_top) ||
            (stack_top.Value() & 0xFULL) != 0)
            return false;

        m_Tss.Rsp0 = stack_top.Value();
        return m_Tss.Rsp0 == stack_top.Value();
    }

    const char* DescriptorTables::Describe(DescriptorTableInitializationError error) noexcept {
        switch (error) {
        case DescriptorTableInitializationError::Success: return "success";
        case DescriptorTableInitializationError::AlreadyInitialized: return "descriptor tables are already initialized";
        case DescriptorTableInitializationError::InvalidTrampoline: return "interrupt trampoline layout is invalid";
        case DescriptorTableInitializationError::LoadFailed: return "failed to activate or validate descriptor tables";
        default: return "unknown descriptor-table initialization error";
        }
    }

    bool SetCurrentKernelPrivilegeStack(Memory::VirtualAddress stack_top) noexcept {
        return g_CurrentDescriptorTables != nullptr &&
            g_CurrentDescriptorTables->SetKernelPrivilegeStack(stack_top);
    }

    Memory::VirtualAddress CurrentKernelPrivilegeStack() noexcept {
        if (g_CurrentDescriptorTables == nullptr)
            return {};
        return g_CurrentDescriptorTables->KernelPrivilegeStack();
    }
}
