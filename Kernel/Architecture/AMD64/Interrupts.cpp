#include <Kernel/Architecture/AMD64/Interrupts.hpp>

extern "C" void InterruptCommonEntry() noexcept;
extern "C" void LoadDescriptorTables(const void* gdtr, const void* idtr) noexcept;
extern "C" void InterruptDispatch(Zos::Kernel::Architecture::AMD64::InterruptContext* context) noexcept;

/*
 * All 256 runtime-generated vector trampolines converge here.
 * 
 * On entry the stack has already been normalized to:
 * 
 *      Vector
 *      ErrorCode
 *      RIP
 *      CS
 *      RFLAGS
 *      [old RSP]
 *      [old SS]
 * 
 * The final two fields only exist when the processor performed
 * a stack switch
 */
__asm__(
    ".pushsection .text.InterruptCommonEntry,\"ax\",@progbits\n"
    ".global InterruptCommonEntry\n"
    ".type InterruptCommonEntry,@function\n"

    "InterruptCommonEntry:\n"

    /*
     * The SysV ABI requires DF to be clear.
     *
     * IRETQ restores the interrupted RFLAGS, including DF.
     */
    "   cld\n"

    /*
     * Keep this order synchronized with InterruptContext !!!
     */
    "   pushq %rax\n"
    "   pushq %rbx\n"
    "   pushq %rcx\n"
    "   pushq %rdx\n"
    "   pushq %rbp\n"
    "   pushq %rdi\n"
    "   pushq %rsi\n"

    "   pushq %r8\n"
    "   pushq %r9\n"
    "   pushq %r10\n"
    "   pushq %r11\n"
    "   pushq %r12\n"
    "   pushq %r13\n"
    "   pushq %r14\n"
    "   pushq %r15\n"

    /*
     * R12 now contains the exact InterruptContext pointer.
     *
     * The interrupted R12 value is already safely stored in
     * the register frame above.
     * 
     * R12 is callee-saved under the SysV ABI, which allows 
     * recovery of the original interrupt-frame stack pointer
     * even after aligning RSP for the C++ call.
     */
    "   movq %rsp, %r12\n"

    /*
     * First SysV argument:
     *      InterruptContext* context
     */
    "   movq %rsp, %rdi\n"

    /*
     * Hardware interrupt entry does not give us a SysV call-site
     * alignment guarantee, so alignment is done explicitly.
     */
    "   andq $-16, %rsp\n"
    "   call InterruptDispatch\n"

    /*
     * Return to the exact register image.
     */
    "   movq %r12, %rsp\n"

    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"

    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbp\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    popq %rbx\n"
    "    popq %rax\n"

    /*
     * Discard our normalized fields:
     *
     *      Vector
     *      ErrorCode
     * IRETQ consumes the processor frame.
     */
    "   addq $16, %rsp\n"
    "iretq\n"

    ".size InterruptCommonEntry, .-InterruptCommonEntry\n"
    ".popsection\n"
);

/*
 * Installs the zOS GDT, reloads the kernel segment selectors
 * loads TR with the zOS TSS, and isntalls the IDT.
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

    /*
     * Reload data/stack selectors
     */
    "   movw $0x10, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   movw %ax, %ss\n"

    /*
     * Reload CS using a far return
     *
     * 0x08 = KernelCodeSelector
     */
    "   pushq $0x08\n"
    "   leaq 1f(%rip), %rax\n"
    "   pushq %rax\n"
    "   lretq\n"

    "1:\n"

    /*
     * 0x18 = TaskStateSelector
     */
    "   movw $0x18, %ax\n"
    "   ltr %ax\n"

    "   lidt (%rsi)\n"

    "   ret\n"

    ".size LoadDescriptorTables, .-LoadDescriptorTables\n"
    ".popsection\n"
);

namespace {
    using namespace Zos::Kernel;
    using namespace Zos::Kernel::Memory;
    using namespace Zos::Kernel::Architecture::AMD64;

    inline constexpr Uint16 DebugPort{ 0xE9 };

    inline constexpr Uint64 RflagsInterruptEnable{ 1ULL << 9 };

    inline constexpr Uint64 RuntimeTrampolinePageCount{ 2 };
    inline constexpr Uint64 RuntimeStubSize{ 16 };
    inline constexpr Uint64 CommonTargetOffset{ PageSize };

    static_assert(RuntimeStubSize * InterruptVectorCount == PageSize);

    /*
     * One InterruptManager exists during the current single-CPU bring-up.
     *
     * This becomes per-CPU or references permanent kernel state
     * once SMP is introduced.
     */
    InterruptManager* g_InterruptManager{};

    volatile bool g_BreakpointObserved{};

    void WriteDebugChar(char value) noexcept {
        __asm__ volatile(
            "outb %0, %1"
            :
            : "a"(value),
              "Nd"(DebugPort)
        );
    }

    void WriteDebug(const char* message) noexcept {
        if (message == nullptr)
            return;

        while (*message != '\0') {
            WriteDebugChar(*message);
            ++message;
        }
    }

    void WriteHex(Uint64 value) noexcept {
        static constexpr char Digits[]{ "0123456789ABCDEF" };

        WriteDebug("0x");

        for (int shift = 60; shift >= 0; shift -= 4) {
            const Uint8 digit = static_cast<Uint8>((value >> shift) & 0x0F);
            WriteDebugChar(Digits[digit]);
        }
    }

    void WriteDecimal(Uint64 value) noexcept {
        char buffer[21]{};
        Uint64 position = sizeof(buffer);

        do {
            const Uint8 digit = static_cast<Uint8>(value % 10);
            buffer[--position] = static_cast<char>('0' + digit);
            value /= 10;
        } while (value != 0);

        while (position < sizeof(buffer)) {
            WriteDebugChar(buffer[position]);
            ++position;
        }
    }

    void WriteRegister(const char* name, Uint64 value) noexcept {
        WriteDebug(name);
        WriteDebug("=");
        WriteHex(value);
    }

    [[noreturn]] void HaltForever() noexcept {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    Uint64 ReadRflags() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "pushfq\n\t"
            "popq %0"
            : "=r"(value)
        );
        return value;
    }

    Uint64 ReadCr2() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "mov %%cr2, %0"
            : "=r"(value)
        );
        return value;
    }

    /*
     * Exceptions for which the processor itself places an error
     * code between the hardware state and our runtime trampoline.
     * 
     * Extended architectural exceptions are included even though
     * zOS does not currently enable the corresponding facilities.
     */
    bool HasHardwareErrorCode(Uint8 vector) noexcept {
        switch (vector) {
        case 8:  // #DF
        case 10: // #TS
        case 11: // #NP
        case 12: // #SS
        case 13: // #GP
        case 14: // #PF
        case 17: // #AC
        case 21: // #CP
        case 29: // #VC
        case 30: // #SX
            return true;
        default:
            return false;
        }
    }

    const char* DefaultVectorName(Uint8 vector) noexcept {
        switch (vector) {
        case 0:  return "#DE Divide Error";
        case 1:  return "#DB Debug";
        case 2:  return "NMI";
        case 3:  return "#BP Breakpoint";
        case 4:  return "#OF Overflow";
        case 5:  return "#BR Bound Range Exceeded";
        case 6:  return "#UD Invalid Opcode";
        case 7:  return "#NM Device Not Available";
        case 8:  return "#DF Double Fault";
        case 9:  return "Reserved Exception 9";
        case 10: return "#TS Invalid TSS";
        case 11: return "#NP Segment Not Present";
        case 12: return "#SS Stack-Segment Fault";
        case 13: return "#GP General Protection";
        case 14: return "#PF Page Fault";
        case 15: return "Reserved Exception 15";
        case 16: return "#MF x87 Floating-Point";
        case 17: return "#AC Alignment Check";
        case 18: return "#MC Machine Check";
        case 19: return "#XM SIMD Floating-Point";
        case 20: return "#VE Virtualization";
        case 21: return "#CP Control Protection";
        case 29: return "#VC VMM Communication";
        case 30: return "#SX Security Exception";
        default:
            return vector < 32 ? "Reserved CPU Exception" : "Unhandled Interrupt";
        }
    }

    void DumpContext(const InterruptContext& context) noexcept {
        WriteDebug("Vector: ");
        WriteDecimal(context.Vector);

        WriteDebug("\nError:  ");
        WriteHex(context.ErrorCode);

        WriteDebug("\nRIP:    ");
        WriteHex(context.Rip);

        WriteDebug("\nCS:     ");
        WriteHex(context.Cs);

        WriteDebug("\nRFLAGS: ");
        WriteHex(context.Rflags);

        WriteDebug("\n\n");

        WriteRegister("RAX", context.Rax);
        WriteDebug("  ");
        WriteRegister("RBX", context.Rbx);
        WriteDebug("\n");

        WriteRegister("RCX", context.Rcx);
        WriteDebug("  ");
        WriteRegister("RDX", context.Rdx);
        WriteDebug("\n");

        WriteRegister("RSI", context.Rsi);
        WriteDebug("  ");
        WriteRegister("RDI", context.Rdi);
        WriteDebug("\n");

        WriteRegister("RBP", context.Rbp);
        WriteDebug("\n");

        WriteRegister("R8 ", context.R8);
        WriteDebug("  ");
        WriteRegister("R9 ", context.R9);
        WriteDebug("\n");

        WriteRegister("R10", context.R10);
        WriteDebug("  ");
        WriteRegister("R11", context.R11);
        WriteDebug("\n");

        WriteRegister("R12", context.R12);
        WriteDebug("  ");
        WriteRegister("R13", context.R13);
        WriteDebug("\n");

        WriteRegister("R14", context.R14);
        WriteDebug("  ");
        WriteRegister("R15", context.R15);
        WriteDebug("\n");
    }

    [[noreturn]] void DumpAndHalt(const InterruptContext& context) noexcept {
        DumpContext(context);
        WriteDebug("\n");
        HaltForever();
    }

    void WriteUint32(Uint8* dst, Uint32 value) {
        dst[0] = static_cast<Uint8>(value);
        dst[1] = static_cast<Uint8>(value >> 8);
        dst[2] = static_cast<Uint8>(value >> 16);
        dst[3] = static_cast<Uint8>(value >> 24);
    }

    void WriteUint64(Uint8* dst, Uint64 value) noexcept {
        for (Uint64 i = 0; i < sizeof(Uint64); ++i)
            dst[i] = static_cast<Uint8>(value >> (i * 8));
    }

    bool EmitVectorStub(Uint8 vector, Uint8* stub, VirtualAddress stub_address, VirtualAddress common_pointer) noexcept {
        Uint64 position = 0;
        if (!HasHardwareErrorCode(vector)) {
            /*
             * push 0
             * Synthetic error code.
             * 6A 00
             */
            stub[position++] = 0x6A;
            stub[position++] = 0x00;
        }

        /*
         * push imm32
         * 
         * Always use imm32 rather than imm8 because PUSH imm8
         * sign-extends vectors 128-255.
         * 
         * 68 XX XX XX XX
         */
        stub[position++] = 0x68;
        WriteUint32(stub + position, static_cast<Uint32>(vector));
        position += sizeof(Uint32);

        /*
         * jmp qword ptr [rip + disp32]
         *
         * This reaches a nearby slot containing the absolute address
         * of InterruptCommonEntry without destroying a GPR.
         * 
         * FF 25 XX XX XX XX
         */
        stub[position++] = 0xFF;
        stub[position++] = 0x25;

        const Uint64 rip_after_instruction = stub_address.Value() + position + sizeof(Uint32);
        const long long displacement = static_cast<long long>(common_pointer.Value()) - static_cast<long long>(rip_after_instruction);
        if (displacement < -0x80000000LL || displacement > 0x7FFFFFFFLL) return false;
        WriteUint32(stub + position, static_cast<Uint32>(displacement));
        position += sizeof(Uint32);

        if (position > RuntimeStubSize) return false;
        while (position < RuntimeStubSize) stub[position++] = 0x90; // nop
        return true;
    }

    /*
     * Changes the permissions of an existing 4 KiB mapping using
     * the current public PageMap interface.
     * 
     * The previous mapping is restored if the new mapping fails
     */
    bool RemapPage(PageMap& page_map, VirtualAddress virt_addr, PhysicalAddress phys_addr, MappingOptions options) noexcept {
        const TranslationResult previous = page_map.Translate(virt_addr);
        if (!previous.Mapped || previous.Physical != phys_addr) return false;
        if (previous.Options.Access == options.Access && previous.Options.Cache == options.Cache) return true;
        if (page_map.UnmapPage(virt_addr) != MappingError::Success) return false;
        if (page_map.MapPage(virt_addr, phys_addr, options) == MappingError::Success) return true;

        /*
         * Best effort rollback
         */
        (void)page_map.MapPage(virt_addr, phys_addr, previous.Options);
        return false;
    }

    /*
     * Serialize the instruction stream after emitting runtime code.
     */
    void SerializeInstructionStream() noexcept {
        Uint32 eax{};
        Uint32 ebx{};
        Uint32 ecx{};
        Uint32 edx{};

        __asm__ volatile(
            "cpuid"
            : "+a"(eax),
              "=b"(ebx),
              "+c"(ecx),
              "=d"(edx)
            :
            : "memory"
        );
    }

    void HandlePageFault(InterruptContext& context) noexcept {
        const Uint64 fault_address = ReadCr2();
        const Uint64 error = context.ErrorCode;

        WriteDebug("\n[zOS/Exception] PAGE FAULT\n");

        WriteDebug("CR2:    ");
        WriteHex(fault_address);
        WriteDebug("\nCause:  ");

        if ((error & (1ULL << 0)) != 0)
            WriteDebug("protection violation");
        else WriteDebug("non-present page");

        WriteDebug("\nAccess: ");
        if ((error & (1ULL << 1)) != 0)
            WriteDebug("write");
        else WriteDebug("read");

        if ((error & (1ULL << 4)) != 0)
            WriteDebug(" / instruction fetch");

        WriteDebug("\nMode:   ");
        if ((error & (1ULL << 2)) != 0)
            WriteDebug("user");
        else WriteDebug("supervisor");

        if ((error & (1ULL << 3)) != 0) 
            WriteDebug("\nReserved-bit violation: yes");
        
        WriteDebug("\n\n");

        DumpAndHalt(context);
    }

    void HandleGeneralProtection(InterruptContext& context) noexcept {
        const Uint64 error = context.ErrorCode;

        WriteDebug("\n[zOS/Exception] GENERAL PROTECTION FAULT\n");

        WriteDebug("External: ");
        WriteDebug((error & 0x01) != 0 ? "yes" : "no");
        
        WriteDebug("\nTable:    ");
         if ((error & 0x2) != 0) 
            WriteDebug("IDT");
        else if ((error & 0x4) != 0) 
            WriteDebug("LDT");
        else WriteDebug("GDT");
        

        WriteDebug("\nSelector index: ");
        WriteDecimal(error >> 3);
        WriteDebug("\n\n");

        DumpAndHalt(context);
    }

    void HandleDoubleFault(InterruptContext& context) noexcept {
        WriteDebug("\n[zOS/Exception] DOUBLE FAULT\nRunning on the dedicated IST1 stack.\n\n");
        DumpAndHalt(context);
    }

    void HandleInvalidOpcode(InterruptContext& context) noexcept {
        WriteDebug("\n[zOS/Exception] INVALID OPCODE\n\n");
        DumpAndHalt(context);
    }

    void BreakpointSelfTestHandler(InterruptContext&) noexcept {
        g_BreakpointObserved = true;
    }   
}

extern "C" void InterruptDispatch(Zos::Kernel::Architecture::AMD64::InterruptContext* context) noexcept {
    if (context == nullptr || g_InterruptManager == nullptr) {
        WriteDebug("\n[zOS/Interrupt] Emergency dispatcher failure.\n");
        HaltForever();
    }
    g_InterruptManager->Dispatch(*context);
}

namespace Zos::Kernel::Architecture::AMD64 {
    using namespace Memory;

    void InterruptManager::InitializeHandlers() noexcept {
        for (Uint64 vector = 0; vector < InterruptVectorCount; vector++) {
            m_Handlers[vector] = {
                .Handler = &GenericInterruptHandler,
                .Name = nullptr,
            };
        }
    }

    void InterruptManager::RegisterBuiltinHandlers() noexcept {
        m_Handlers[6] = {
            .Handler = &HandleInvalidOpcode,
            .Name = "#UD Invalid Opcode",
        };
        m_Handlers[8] = {
            .Handler = &HandleDoubleFault,
            .Name = "#DF Double Fault",
        };
        m_Handlers[13] = {
            .Handler = &HandleGeneralProtection,
            .Name = "#GP General Protection",
        };
        m_Handlers[14] = {
            .Handler = &HandlePageFault,
            .Name = "#PF Page Fault",
        };
    }

    bool InterruptManager::RegisterHandler(Uint8 vector, InterruptHandler handler, const char* name) noexcept {
        if (!m_Initialized || handler == nullptr) return false;
        m_Handlers[vector] = { 
            .Handler = handler,
            .Name = name,
        };
        return true;
    }

    void InterruptManager::UnregisterHandler(Uint8 vector) noexcept {
        if (!m_Initialized) return;
        m_Handlers[vector] = {
            .Handler = &GenericInterruptHandler,
            .Name = nullptr,
        };
    }

    const char* InterruptManager::HandlerName(Uint8 vector) const noexcept {
        const char* registered_name = m_Handlers[vector].Name;
        if (registered_name != nullptr) return registered_name;
        return DefaultVectorName(vector);
    }

    void InterruptManager::Dispatch(InterruptContext& context) noexcept {
        if (context.Vector >= InterruptVectorCount) GenericInterruptHandler(context);
        HandlerSlot& slot = m_Handlers[context.Vector];
        if (slot.Handler == nullptr) GenericInterruptHandler(context);
        slot.Handler(context);
    }

    void InterruptManager::InitializeGdt() noexcept {
        /*
         * 64-bit kernel code:
         *
         * Present
         * DPL 0
         * Executable
         * Readable
         * Long mode
         * 4 KiB granularity
         */
        constexpr Uint64 KernelCodeDescriptor{ 0x00AF9A000000FFFFULL };

        /*
         * Kernel data segment
         *
         * Most segmentation semantics are ignored in long mode,
         * but a valid data/stack selector remains useful.
         */
        constexpr Uint64 KernelDataDescriptor{ 0x00CF92000000FFFFULL };

        m_Gdt = {};
        m_Tss = {};

        m_Gdt.KernelCode = KernelCodeDescriptor;
        m_Gdt.KernelData = KernelDataDescriptor;

        /*
         * Stack grows downward, so IST1 points immediately after
         * the final byte of the backing array
         */
        m_Tss.Ist1 = reinterpret_cast<Uint64>(m_DoubleFaultStack + sizeof(m_DoubleFaultStack));

        /*
         * RSP0 becomes relevant when zOS begins transitioning
         * from CPL3 to CPL0.
         */
        m_Tss.Rsp0 = 0;

        /*
         * Place the I/O permission bitmap outside the TSS limit,
         * effectively indicating that no bitmap is present.
         */
        m_Tss.IoMapBase = static_cast<Uint16>(sizeof(TaskStateSegment));

        const Uint64 tss_base = reinterpret_cast<Uint64>(&m_Tss);
        constexpr Uint32 tss_limit = sizeof(TaskStateSegment) - 1;
        TssDescriptor& descriptor = m_Gdt.Tss;
        descriptor.LimitLow = static_cast<Uint16>(tss_limit);
        descriptor.BaseLow = static_cast<Uint16>(tss_base);
        descriptor.BaseMiddle = static_cast<Uint8>(tss_base >> 16);

        /*
         * Present, DPL 0, available 64-bit TSS.
         */
        descriptor.Access = 0x89;

        descriptor.LimitHighFlags = static_cast<Uint8>((tss_limit >> 16) & 0x0F);
        descriptor.BaseHigh = static_cast<Uint8>(tss_base >> 24);
        descriptor.BaseUpper = static_cast<Uint32>(tss_base >> 32);
        descriptor.Reserved = 0;
    }

    void InterruptManager::SetIdtGate(Uint8 vector, VirtualAddress handler, Uint8 ist) noexcept {
        IdtEntry& entry = m_Idt[vector];
        entry = {};

        const Uint64 address = handler.Value();
        entry.OffsetLow = static_cast<Uint16>(address);
        entry.Selector = KernelCodeSelector;
        entry.Ist = ist & 0x07;

        /*
         * 0x8E:
         * 
         * Present  = 1
         * DPL      = 0
         * Type     = 64-bit interrupt gate
         */
        entry.Attributes = 0x8E;
        entry.OffsetMiddle = static_cast<Uint16>(address >> 16);
        entry.OffsetHigh = static_cast<Uint32>(address >> 32);
        entry.Reserved = 0;
    }

    void InterruptManager::InitializeIdt() noexcept {
        for (Uint64 vector = 0; vector < InterruptVectorCount; vector++) {
            const VirtualAddress stub = m_TrampolineVirtual.Base() + vector * RuntimeStubSize;

            /*
             * #DF receives IST1.
             *
             * Other vectors initially remain on the current kernel
             * stack. NMI/#MC can receive dedicated IST stacks later.
             */
            const Uint8 ist = vector == 8 ? 1 : 0;
            SetIdtGate(static_cast<Uint8>(vector), stub, ist);
        }
    }

    bool InterruptManager::BuildRuntimeTrampolines() noexcept {
        if (!m_TrampolineVirtual.IsValid() || !m_TrampolineBacking.IsValid()) return false;

        auto* bytes = reinterpret_cast<Uint8*>(m_TrampolineVirtual.Base().Value());

        /*
         * The first page contains exactly 256 16-byte stubs.
         * 
         * The second page begins with one absolute pointer to
         * InterruptCommonEntry.
         */
        const VirtualAddress common_pointer = m_TrampolineVirtual.Base() + CommonTargetOffset;
        WriteUint64(bytes + CommonTargetOffset, reinterpret_cast<Uint64>(&InterruptCommonEntry));

        for (Uint64 vector = 0; vector < InterruptVectorCount; vector++) {
            const Uint64 offset = vector * RuntimeStubSize;
            if (!EmitVectorStub(static_cast<Uint8>(vector), bytes + offset, m_TrampolineVirtual.Base() + offset, common_pointer)) return false;
        }
        return true;
    }

    bool InterruptManager::SealRuntimeTrampolines() noexcept {
        if (m_PageMap == nullptr) return false;

        const PhysicalAddress code_physical = m_TrampolineBacking.Base();
        const PhysicalAddress data_physical = m_TrampolineBacking.Base() + PageSize;

        const VirtualAddress code_virtual = m_TrampolineVirtual.Base();
        const VirtualAddress data_virtual = m_TrampolineVirtual.Base() + PageSize;

        const VirtualAddress code_direct = Layout::DirectMapAddress(code_physical);
        const VirtualAddress data_direct = Layout::DirectMapAddress(data_physical);

        if (code_direct.IsNull() || data_direct.IsNull()) return false;

        const MappingOptions read_only{
            .Access = PageAccess::Read | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions executable{
            .Access = PageAccess::Read | PageAccess::Execute | PageAccess::Global,
            .Cache = CachePolicy::WriteBack
        };

        /*
         * Remove writable direct-map aliases BEFORE making the
         * runtime-code alias executable.
         * 
         * This guarantees that at no point does the trampoline
         * physical page have simulataneous writable and executable
         * aliases
         */
        if (!RemapPage(*m_PageMap, code_direct, code_physical, read_only)) return false;
        if (!RemapPage(*m_PageMap, data_direct, data_physical, read_only)) return false;

        /*
         * The pointer page is permanently read-only and NX.
         */
        if (!RemapPage(*m_PageMap, data_virtual, data_physical, read_only)) return false;

        /*
         * Make the emitted stub page executable last.
         */
        if (!RemapPage(*m_PageMap, code_virtual, code_physical, executable)) return false;

        SerializeInstructionStream();

        return true;
    }

    InterruptInitializationError InterruptManager::InitializeTrampolines() noexcept {
        const PhysicalAllocationError physical_error = m_PhysicalMemory->AllocateContiguous(RuntimeTrampolinePageCount, m_TrampolineBacking);
        if (physical_error != PhysicalAllocationError::Success) return InterruptInitializationError::PhysicalAllocationFailed;

        const VirtualAllocationError virtual_error = m_VirtualAddresses->Reserve(RuntimeTrampolinePageCount, m_TrampolineVirtual);
        if (virtual_error != VirtualAllocationError::Success) {
            (void)m_PhysicalMemory->Release(m_TrampolineBacking);
            return InterruptInitializationError::VirtualAllocationFailed;
        }

        const MappingOptions writable{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingError mapping_error = m_PageMap->MapRange(m_TrampolineVirtual.Base(), m_TrampolineBacking.Base(), RuntimeTrampolinePageCount, writable);
        if (mapping_error != MappingError::Success) {
            (void)m_VirtualAddresses->Release(m_TrampolineVirtual);
            (void)m_PhysicalMemory->Release(m_TrampolineBacking);
            return InterruptInitializationError::MappingFailed;
        }

        if (!BuildRuntimeTrampolines()) {
            for (Uint64 page = 0; page < RuntimeTrampolinePageCount; page++) 
                (void)m_PageMap->UnmapPage(m_TrampolineVirtual.Base() + page * PageSize);
            (void)m_VirtualAddresses->Release(m_TrampolineVirtual);
            (void)m_PhysicalMemory->Release(m_TrampolineBacking);
            return InterruptInitializationError::TrampolineGenerationFailed;
        }

        /*
         * A sealing failure is considered fatal during early boot.
         * 
         * Keep ownership of the pages rather than releasing physical
         * pages whose direct-map permissions may already have changed.
         */
        if (!SealRuntimeTrampolines()) 
            return InterruptInitializationError::ProtectionFailed;
        return InterruptInitializationError::Success;
    }

    bool InterruptManager::ValidateDescriptorTables() const noexcept {
        DescriptorTablePointer loaded_gdt{};
        DescriptorTablePointer loaded_idt{};

        Uint16 task_register{};
        Uint16 code_segment{};
        Uint16 stack_segment{};

        __asm__ volatile(
            "sgdt %0"
            : "=m"(loaded_gdt)
        );
        __asm__ volatile(
            "sidt %0"
            : "=m"(loaded_idt)
        );
        __asm__ volatile(
            "str %0"
            : "=r"(task_register)
        );
        __asm__ volatile(
            "mov %%cs, %0"
            : "=r"(code_segment)
        );
        __asm__ volatile(
            "mov %%ss, %0"
            : "=r"(stack_segment)
        );

        const Uint64 expected_gdt = reinterpret_cast<Uint64>(&m_Gdt);
        const Uint64 expected_idt = reinterpret_cast<Uint64>(&m_Idt[0]);

        return loaded_gdt.Base == expected_gdt && loaded_gdt.Limit == sizeof(m_Gdt) - 1
            && loaded_idt.Base == expected_idt && loaded_idt.Limit == sizeof(m_Idt) - 1
            && task_register == TaskStateSelector
            && code_segment == KernelCodeSelector
            && stack_segment == KernelDataSelector;   
    }

    InterruptInitializationError InterruptManager::Initialize(PhysicalMemoryManager& physical_memory, VirtualAddressAllocator& virtual_addresses, PageMap& page_map) noexcept {
        if (m_Initialized) return InterruptInitializationError::AlreadyInitialized;
        if (!physical_memory.IsInitialized() || !virtual_addresses.IsInitialized() || !page_map.IsInitialized() || !page_map.IsActive()) 
            return InterruptInitializationError::InvalidDependency;
        
        /*
         * zOS does not yet have an interrupt controller or external
         * interrupt routing. The descriptor-table transition must
         * therefore occur with maskable interrupts disabled.
         */
        if ((ReadRflags() & RflagsInterruptEnable) != 0) 
            return InterruptInitializationError::InterruptsEnabled;

        m_PhysicalMemory = &physical_memory;
        m_VirtualAddresses = &virtual_addresses;
        m_PageMap = &page_map;

        InitializeHandlers();
        RegisterBuiltinHandlers();

        const InterruptInitializationError trampoline_error = InitializeTrampolines();
        if (trampoline_error != InterruptInitializationError::Success) return trampoline_error;

        InitializeGdt();
        InitializeIdt();

        const DescriptorTablePointer gdtr{ 
            .Limit = static_cast<Uint16>(sizeof(m_Gdt) - 1),
            .Base = reinterpret_cast<Uint64>(&m_Gdt),
        };

        const DescriptorTablePointer idtr{
            .Limit = static_cast<Uint16>(sizeof(m_Idt) - 1),
            .Base = reinterpret_cast<Uint64>(&m_Idt),
        };

        /*
         * The dispatcher must already know its manager before LIDT.
         * An NMI or architectural exception can occur independently
         * of IF.
         */
        g_InterruptManager = this;

        LoadDescriptorTables(&gdtr, &idtr);

        if (!ValidateDescriptorTables()) 
            return InterruptInitializationError::DescriptorLoadFailed;

        m_Initialized = true;

        return InterruptInitializationError::Success;
    }

    bool InterruptManager::RunBreakpointSelfTest() noexcept {
        if (!m_Initialized) return false;

        HandlerSlot previous = m_Handlers[3];

        g_BreakpointObserved = false;

        m_Handlers[3] = {
            .Handler = &BreakpointSelfTestHandler,
            .Name = "#BP Interrupt Self-Test",
        };

        __asm__ volatile(
            "int3"
            :
            :
            : "memory"
        );

        const bool observed = g_BreakpointObserved;
        m_Handlers[3] = previous;

        return observed;
    }

     const char* InterruptManager::Describe(InterruptInitializationError error) noexcept {
        switch (error) {
        case InterruptInitializationError::Success: return "success";
        case InterruptInitializationError::AlreadyInitialized: return "interrupt infrastructure is already initialized";
        case InterruptInitializationError::InvalidDependency: return "interrupt infrastructure dependency is invalid";
        case InterruptInitializationError::InterruptsEnabled: return "interrupt initialization requires maskable interrupts to be disabled";
        case InterruptInitializationError::PhysicalAllocationFailed: return "failed to allocate physical trampoline storage";
        case InterruptInitializationError::VirtualAllocationFailed: return "failed to reserve trampoline virtual address space";
        case InterruptInitializationError::MappingFailed: return "failed to map runtime interrupt trampolines";
        case InterruptInitializationError::TrampolineGenerationFailed: return "failed to generate runtime interrupt trampolines";
        case InterruptInitializationError::ProtectionFailed: return "failed to seal runtime interrupt trampolines";
        case InterruptInitializationError::DescriptorLoadFailed: return "failed to activate or validate zOS descriptor tables";
        }
        return "unknown interrupt initialization error";
    }

    [[noreturn]] void GenericInterruptHandler(InterruptContext& context) noexcept {
        __asm__ volatile("cli");

        WriteDebug("\n[zOS/Interrupt] UNHANDLED INTERRUPT\n");
        WriteDebug("Reason: ");

        if (context.Vector < InterruptVectorCount) {
            if (g_InterruptManager != nullptr) 
                WriteDebug(g_InterruptManager->HandlerName(static_cast<Uint8>(context.Vector)));
            else WriteDebug(DefaultVectorName(static_cast<Uint8>(context.Vector)));
        } else WriteDebug("invalid interrupt vector");

        WriteDebug("\n\n");

        DumpAndHalt(context);
    }
}