#include <Kernel/Architecture/AMD64/SystemCalls.hpp>

#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Architecture/AMD64/Paging.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>

extern "C" void SystemCallEntryAssembly() noexcept;
extern "C" void Amd64SystemCallDispatch(Zos::Kernel::Architecture::AMD64::SystemCallContext* context) noexcept;

namespace {
    using namespace Zos::Kernel;
    using namespace Zos::Kernel::Memory;
    using namespace Zos::Kernel::Architecture::AMD64;

    inline constexpr Uint32 EferMsr{ 0xC0000080u };
    inline constexpr Uint32 StarMsr{ 0xC0000081u };
    inline constexpr Uint32 LstarMsr{ 0xC0000082u };
    inline constexpr Uint32 FmaskMsr{ 0xC0000084u };

    inline constexpr Uint64 EferSystemCallEnable{ 1ULL << 0 };

    inline constexpr Uint64 RflagsTrap{ 1ULL << 8 };
    inline constexpr Uint64 RflagsInterruptEnable{ 1ULL << 9 };
    inline constexpr Uint64 RflagsDirection{ 1ULL << 10 };
    inline constexpr Uint64 RflagsIoPrivilege{ 3ULL << 12 };
    inline constexpr Uint64 RflagsNestedTask{ 1ULL << 14 };
    inline constexpr Uint64 RflagsResume{ 1ULL << 16 };
    inline constexpr Uint64 RflagsAlignmentCheck{ 1ULL << 18 };

    inline constexpr Uint64 SystemCallFlagMask{
        RflagsTrap |
        RflagsInterruptEnable |
        RflagsDirection |
        RflagsIoPrivilege |
        RflagsNestedTask |
        RflagsResume |
        RflagsAlignmentCheck
    };

    /*
     * For 64-bit SYSRET, CS = STAR[63:48] + 16 and SS = STAR[63:48] + 8,
     * with RPL forced to 3. The base field therefore sits one descriptor
     * before the user data selector.
     */
    inline constexpr Uint16 SysretSelectorBase{ static_cast<Uint16>(UserDataSelector - 8) };

    static_assert(static_cast<Uint16>(SysretSelectorBase + 8) == UserDataSelector);
    static_assert(static_cast<Uint16>(SysretSelectorBase + 16) == UserCodeSelector);

    inline constexpr Uint64 StarValue{
        (static_cast<Uint64>(SysretSelectorBase) << 48) |
        (static_cast<Uint64>(KernelCodeSelector) << 32)
    };

    SystemCallEntry* g_SystemCallEntry{};

    [[nodiscard]] bool IsUserAddress(VirtualAddress address) noexcept {
        return !address.IsNull() &&
            PageMap::IsCanonical(address) &&
            address.Value() < 0x0000800000000000ULL;
    }
}

/*
 * SYSCALL does not change RSP. Never touch the user stack.
 *
 * ProcessorLocal has two fields with fixed assembly-visible offsets:
 *   GS:[32] -> current kernel entry stack top
 *   GS:[40] -> temporary user RSP scratch
 *
 * The static_asserts below make a ProcessorLocal layout change fail the build
 * instead of silently corrupting syscall entry.
 */
static_assert(__builtin_offsetof(Zos::Kernel::Architecture::AMD64::ProcessorLocal, KernelEntryStackTop) == 32);
static_assert(__builtin_offsetof(Zos::Kernel::Architecture::AMD64::ProcessorLocal, SystemCallUserStackPointer) == 40);

__asm__(
    ".pushsection .text.SystemCallEntry,\"ax\",@progbits\n"
    ".global SystemCallEntryAssembly\n"
    ".type SystemCallEntryAssembly,@function\n"

    "SystemCallEntryAssembly:\n"
    "    swapgs\n"

    /* Save user RSP before abandoning the user stack. */
    "    movq %rsp, %gs:40\n"
    "    movq %gs:32, %rsp\n"
    "    testq %rsp, %rsp\n"
    "    jz .Lsyscall_emergency_halt\n"

    /* SystemCallContext is exactly 128 bytes, preserving 16-byte alignment. */
    "    subq $128, %rsp\n"
    "    movq %r15,   0(%rsp)\n"
    "    movq %r14,   8(%rsp)\n"
    "    movq %r13,  16(%rsp)\n"
    "    movq %r12,  24(%rsp)\n"
    "    movq %r10,  32(%rsp)\n"
    "    movq %r9,   40(%rsp)\n"
    "    movq %r8,   48(%rsp)\n"
    "    movq %rsi,  56(%rsp)\n"
    "    movq %rdi,  64(%rsp)\n"
    "    movq %rbp,  72(%rsp)\n"
    "    movq %rdx,  80(%rsp)\n"
    "    movq %rbx,  88(%rsp)\n"
    "    movq %rax,  96(%rsp)\n"
    "    movq %gs:40, %rax\n"
    "    movq %rax, 104(%rsp)\n"
    "    movq %rcx, 112(%rsp)\n"
    "    movq %r11, 120(%rsp)\n"
    "    movq $0, %gs:40\n"

    /* The SysV C++ entry contract requires DF clear. */
    "    cld\n"

    /* Keep ordinary kernel C++ execution on the kernel data selectors. */
    "    movw $0x10, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"

    "    movq %rsp, %rdi\n"
    "    call Amd64SystemCallDispatch\n"

    /* Restore all preserved registers. RAX carries the native syscall result. */
    "    movq   0(%rsp), %r15\n"
    "    movq   8(%rsp), %r14\n"
    "    movq  16(%rsp), %r13\n"
    "    movq  24(%rsp), %r12\n"
    "    movq  32(%rsp), %r10\n"
    "    movq  40(%rsp), %r9\n"
    "    movq  48(%rsp), %r8\n"
    "    movq  56(%rsp), %rsi\n"
    "    movq  64(%rsp), %rdi\n"
    "    movq  72(%rsp), %rbp\n"
    "    movq  80(%rsp), %rdx\n"
    "    movq  88(%rsp), %rbx\n"
    "    movq  96(%rsp), %rax\n"
    "    movq 112(%rsp), %rcx\n"
    "    movq 120(%rsp), %r11\n"

    /* Restore userspace data selectors before loading the user stack. */
    "    movw $0x2B, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movq 96(%rsp), %rax\n"

    /*
     * This is the final memory access through the kernel stack. IF is still
     * clear because IA32_FMASK masked it on entry.
     */
    "    movq 104(%rsp), %rsp\n"
    "    swapgs\n"
    "    sysretq\n"

    ".Lsyscall_emergency_halt:\n"
    "    cli\n"
    ".Lsyscall_emergency_halt_loop:\n"
    "    hlt\n"
    "    jmp .Lsyscall_emergency_halt_loop\n"

    ".size SystemCallEntryAssembly, .-SystemCallEntryAssembly\n"
    ".popsection\n"
);

extern "C" void Amd64SystemCallDispatch(Zos::Kernel::Architecture::AMD64::SystemCallContext* context) noexcept {
    using namespace Zos::Kernel::Diagnostics;

    if (context == nullptr || g_SystemCallEntry == nullptr) {
        Fatal("Syscall", "AMD64 syscall dispatcher entered without initialized state");
    }

    g_SystemCallEntry->Dispatch(*context);
}

namespace Zos::Kernel::Architecture::AMD64 {
    SystemCallInitializationError SystemCallEntry::Initialize(const CpuInfo& cpu, const DescriptorTables& descriptor_tables) noexcept {
        if (m_Initialized || g_SystemCallEntry != nullptr)
            return SystemCallInitializationError::AlreadyInitialized;

        ProcessorLocal* processor = CurrentProcessor();
        if (!cpu.IsInitialized() || !descriptor_tables.IsInitialized() ||
            processor == nullptr || !processor->IsInitialized() ||
            CurrentKernelEntryStack().IsNull())
            return SystemCallInitializationError::InvalidDependency;

        if (!cpu.Features().Syscall)
            return SystemCallInitializationError::Unsupported;

        const Memory::VirtualAddress entry(reinterpret_cast<Memory::Uint64>(&SystemCallEntryAssembly));
        if (!PageMap::IsCanonical(entry))
            return SystemCallInitializationError::InvalidEntryPoint;

        const Memory::Uint64 previous_efer = ReadMsr(EferMsr);
        const Memory::Uint64 desired_efer = previous_efer | EferSystemCallEnable;
        WriteMsr(EferMsr, desired_efer);
        if (ReadMsr(EferMsr) != desired_efer)
            return SystemCallInitializationError::EferWriteFailed;

        WriteMsr(StarMsr, StarValue);
        if (ReadMsr(StarMsr) != StarValue)
            return SystemCallInitializationError::StarWriteFailed;

        WriteMsr(LstarMsr, entry.Value());
        if (ReadMsr(LstarMsr) != entry.Value())
            return SystemCallInitializationError::LstarWriteFailed;

        WriteMsr(FmaskMsr, SystemCallFlagMask);
        if (ReadMsr(FmaskMsr) != SystemCallFlagMask)
            return SystemCallInitializationError::FmaskWriteFailed;

        g_SystemCallEntry = this;
        m_Initialized = true;

        Diagnostics::Write("[zOS/Syscall] Native AMD64 syscall entry initialized.\n");
        Diagnostics::Write("[zOS/Syscall] SYSCALL/SYSRET enabled.\n");

        return SystemCallInitializationError::Success;
    }

    bool SystemCallEntry::BindUserFaultHandler(UserFaultHandler handler) noexcept {
        if (!m_Initialized || handler == nullptr || m_UserFaultHandler != nullptr)
            return false;

        m_UserFaultHandler = handler;
        return true;
    }

    bool SystemCallEntry::ReturnStateValid(const SystemCallContext& context, Memory::VirtualAddress& invalid_address) const noexcept {
        const Memory::VirtualAddress user_rip(context.UserRip);
        const Memory::VirtualAddress user_rsp(context.UserRsp);

        if (!IsUserAddress(user_rip)) {
            invalid_address = user_rip;
            return false;
        }

        if (!IsUserAddress(user_rsp)) {
            invalid_address = user_rsp;
            return false;
        }

        return true;
    }

    [[noreturn]] void SystemCallEntry::RejectReturnState(const SystemCallContext& context, Memory::VirtualAddress address) noexcept {
        if (m_UserFaultHandler == nullptr)
            Diagnostics::Fatal("Syscall", "invalid user return state arrived before fault policy binding");

        UserFault fault{};
        fault.Kind = UserFaultKind::InvalidSystemCallState;
        fault.Address = address;
        fault.AddressValid = true;
        fault.InstructionPointer = Memory::VirtualAddress(context.UserRip);

        m_UserFaultHandler(fault);
        Diagnostics::Fatal("Syscall", "user fault policy returned from invalid syscall state");
    }

    void SystemCallEntry::Dispatch(SystemCallContext& context) noexcept {
        if (!m_Initialized)
            Diagnostics::Fatal("Syscall", "syscall dispatched before initialization");

        ProcessorLocal* processor = CurrentProcessor();
        if (processor == nullptr || !processor->IsInitialized() || processor->CurrentThread == nullptr)
            Diagnostics::Fatal("Syscall", "syscall arrived without current-thread ownership");

        Scheduling::Thread* thread = processor->CurrentThread;
        Processes::Process* process = thread->OwningProcess();
        if (process == nullptr || !process->IsInitialized() || !process->OwnsAddressSpace() ||
            process->State() != Processes::ProcessState::Active || process->AddressSpace() == nullptr ||
            !process->AddressSpace()->IsActive())
            Diagnostics::Fatal("Syscall", "syscall arrived from an invalid process context");

        Memory::Uint64 kernel_rsp{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(kernel_rsp));

        if (processor->KernelEntryStack().IsNull() || thread->Stack() == nullptr ||
            processor->KernelEntryStack() != thread->Stack()->Top() ||
            CurrentKernelPrivilegeStack() != thread->Stack()->Top() ||
            !thread->Stack()->Contains(Memory::VirtualAddress(kernel_rsp)))
            Diagnostics::Fatal("Syscall", "syscall did not enter on the current thread kernel stack");

        Memory::VirtualAddress invalid_address{};
        if (!ReturnStateValid(context, invalid_address))
            RejectReturnState(context, invalid_address);

        /*
         * SYSRET should never restore elevated privilege or legacy-mode state
         * from userspace RFLAGS. Ordinary arithmetic/status flags and IF are
         * otherwise preserved from the architectural R11 image.
         */
        constexpr Memory::Uint64 ForbiddenReturnFlags{
            RflagsIoPrivilege | RflagsNestedTask | (1ULL << 17)
        };
        context.UserRflags &= ~ForbiddenReturnFlags;
        context.UserRflags |= 1ULL << 1;

        const SystemCalls::Request request{
            .CallNumber = context.Rax,
            .Arguments = {
                context.Rdi,
                context.Rsi,
                context.Rdx,
                context.R10,
                context.R8,
                context.R9,
            },
        };

        SystemCalls::Context native_context{
            .CurrentProcess = *process,
            .CurrentThread = *thread,
        };

        m_DispatchCount++;
        context.Rax = SystemCalls::DispatchNative(request, native_context);
    }

    const char* SystemCallEntry::Describe(SystemCallInitializationError error) noexcept {
        switch (error) {
        case SystemCallInitializationError::Success: return "success";
        case SystemCallInitializationError::AlreadyInitialized: return "syscall entry is already initialized";
        case SystemCallInitializationError::InvalidDependency: return "syscall entry dependency is invalid";
        case SystemCallInitializationError::Unsupported: return "processor does not support SYSCALL/SYSRET";
        case SystemCallInitializationError::InvalidEntryPoint: return "syscall entry point is not canonical";
        case SystemCallInitializationError::EferWriteFailed: return "failed to enable EFER.SCE";
        case SystemCallInitializationError::StarWriteFailed: return "failed to program IA32_STAR";
        case SystemCallInitializationError::LstarWriteFailed: return "failed to program IA32_LSTAR";
        case SystemCallInitializationError::FmaskWriteFailed: return "failed to program IA32_FMASK";
        default: return "unknown syscall initialization error";
        }
    }
}
