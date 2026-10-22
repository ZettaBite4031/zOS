#include <Kernel/Scheduling/Thread.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Architecture/AMD64/UserMode.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Scheduling/Scheduler.hpp>

#include <Kernel/Processes/Process.hpp>


namespace Zos::Kernel::Scheduling {
    namespace {
        [[nodiscard]] Memory::Uint64 ReadRflags() noexcept {
            Memory::Uint64 flags{};

            __asm__ volatile(
                "pushfq\n\t"
                "popq %0"
                : "=r"(flags)
                :
                : "memory"
            );

            return flags;
        }

        inline constexpr Memory::Uint64 RflagsInterruptEnable{ 1ULL << 9 };
    }
    
    ThreadInitializationError Thread::InitializeBootstrap(ThreadId id, Processes::Process& owning_process, Memory::KernelStack& current_stack) noexcept {
        if (m_Initialized || m_OwningProcess != nullptr || m_ProcessNext != nullptr || m_Stack != nullptr || m_OwnedStack.IsInitialized())
            return ThreadInitializationError::AlreadyInitialized;

        if (!owning_process.IsInitialized() || owning_process.State() != Processes::ProcessState::Active || !current_stack.IsInitialized())
            return ThreadInitializationError::InvalidDependency;

        m_Id = id;
        m_State = ThreadState::Running;
        m_Entry = nullptr;
        m_EntryContext = nullptr;

        m_Stack = &current_stack;
        m_OwnsStack = false;

        /*
         * There is no saved bootstrap context yet because this thread is
         * already executing. Its first SwitchTo() captures that context.
         */
        m_Context = {};
        m_ContextReady = false;
        m_Initialized = true;

        owning_process.AttachThread(*this);

        Print();
        return ThreadInitializationError::Success;
    }

    ThreadInitializationError Thread::InitializeKernel(ThreadId id, Processes::Process& owning_process, ThreadEntry entry, void* entry_context,
        Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map, Memory::Uint64 stack_page_count) noexcept {
        if (m_Initialized || m_OwningProcess != nullptr || m_ProcessNext != nullptr || m_Stack != nullptr || m_OwnedStack.IsInitialized())
            return ThreadInitializationError::AlreadyInitialized;

        if (!owning_process.IsInitialized() || owning_process.State() != Processes::ProcessState::Active || !physical_memory.IsInitialized() ||
            !virtual_addresses.IsInitialized() || !page_map.IsInitialized() || !page_map.IsActive())
            return ThreadInitializationError::InvalidDependency;

        if (entry == nullptr) return ThreadInitializationError::InvalidEntry;
        const auto stack_error = m_OwnedStack.Initialize(physical_memory, virtual_addresses, page_map, stack_page_count);
        if (stack_error != Memory::KernelStackInitializationError::Success)
            return ThreadInitializationError::StackInitializationFailed;

        if (!Architecture::AMD64::PrepareThreadContext(m_Context, m_OwnedStack, &StartContext, this))
            return ThreadInitializationError::ContextInitializationFailed;

        m_Id = id;
        m_State = ThreadState::Ready;
        m_Entry = entry;
        m_EntryContext = entry_context;

        m_Stack = &m_OwnedStack;
        m_OwnsStack = true;
        m_ContextReady = true;
        m_Initialized = true;

        owning_process.AttachThread(*this);

        Print();
        return ThreadInitializationError::Success;
    }

    ThreadSwitchError Thread::SwitchTo(Thread& next) noexcept {
        return TransferTo(next, TransferDisposition::Ready);
    }

    ThreadSwitchError Thread::TransferTo(Thread& next, TransferDisposition disposition) noexcept {
        if (!m_Initialized || !next.m_Initialized || &next == this)
            return ThreadSwitchError::InvalidThread;

        if ((ReadRflags() & RflagsInterruptEnable) != 0)
            return ThreadSwitchError::InterruptsEnabled;

        Architecture::AMD64::ProcessorLocal* processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != this)
            return ThreadSwitchError::NotCurrentThread;

        if (m_State != ThreadState::Running)
            return ThreadSwitchError::CurrentStateInvalid;

        if (next.m_State != ThreadState::Ready || !next.m_ContextReady)
            return ThreadSwitchError::TargetNotReady;

        if (m_Stack == nullptr || next.m_Stack == nullptr ||
            !Architecture::AMD64::SetCurrentKernelEntryStack(next.m_Stack->Top()))
            return ThreadSwitchError::KernelPrivilegeStackUpdateFailed;

        Architecture::AMD64::PageMap* current_space =
            m_OwningProcess != nullptr ? m_OwningProcess->AddressSpace() : nullptr;
        Architecture::AMD64::PageMap* next_space =
            next.m_OwningProcess != nullptr ? next.m_OwningProcess->AddressSpace() : nullptr;

        if (current_space == nullptr || next_space == nullptr || !current_space->IsActive()) {
            (void)Architecture::AMD64::SetCurrentKernelEntryStack(m_Stack->Top());
            return ThreadSwitchError::AddressSpaceSwitchFailed;
        }

        if (current_space != next_space) {
            const auto address_space_error = next_space->SwitchTo();
            if (address_space_error != Architecture::AMD64::PageMapSwitchError::Success) {
                (void)Architecture::AMD64::SetCurrentKernelEntryStack(m_Stack->Top());
                return ThreadSwitchError::AddressSpaceSwitchFailed;
            }
        }

        if (disposition == TransferDisposition::Terminated)
            MarkTerminated();
        else {
            m_State = ThreadState::Ready;
            m_ContextReady = true;
        }

        next.m_State = ThreadState::Running;
        processor->CurrentThread = &next;

        Architecture::AMD64::SwitchThreadContext(&m_Context, &next.m_Context);

        if (disposition == TransferDisposition::Terminated)
            Diagnostics::Fatal("Thread", "terminated thread unexpectedly resumed");

        if (m_Stack == nullptr || Architecture::AMD64::CurrentKernelEntryStack() != m_Stack->Top())
            return ThreadSwitchError::ResumeKernelPrivilegeStackMismatch;

        processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != this || m_State != ThreadState::Running)
            return ThreadSwitchError::ResumeStateMismatch;

        if (m_OwningProcess == nullptr || m_OwningProcess->AddressSpace() == nullptr ||
            !m_OwningProcess->AddressSpace()->IsActive())
            return ThreadSwitchError::ResumeAddressSpaceMismatch;

        return ThreadSwitchError::Success;
    }

    [[noreturn]] void Thread::TerminateAndSwitchTo(Thread& next) noexcept {
        const ThreadSwitchError error = TransferTo(next, TransferDisposition::Terminated);
        Diagnostics::Fatal("Thread", Describe(error));
    }

    void Thread::MarkTerminated() noexcept {
        if (!m_Initialized || m_State != ThreadState::Running || m_OwningProcess == nullptr)
            Diagnostics::Fatal("Thread", "invalid thread termination transition");

        m_State = ThreadState::Terminated;

        /*
        * The low-level switch will still write the abandoned CPU context into
        * m_Context, but ContextReady stays false so the scheduler can never
        * legally dispatch this thread again.
        */
        m_ContextReady = false;
        m_ReadyNext = nullptr;
        m_ReadyQueued = false;

        m_OwningProcess->NotifyThreadTerminated(*this);
    }

    void Thread::StartContext(void* argument) noexcept {
        Thread* thread = static_cast<Thread*>(argument);

        if (thread == nullptr || !thread->m_Initialized || thread->m_OwningProcess == nullptr ||
            thread->m_State != ThreadState::Running || thread->m_Entry == nullptr)
            Diagnostics::Fatal("Thread", "fresh kernel thread entered with invalid state");

        Architecture::AMD64::ProcessorLocal* processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != thread)
            Diagnostics::Fatal("Thread", "fresh kernel thread does not own the current processor");

        /*
         * Fresh threads dispatched by the preemptive scheduler begin life
         * from an interrupt-disabled switching context. Once scheduler
         * preemption is active, normal kernel-thread execution runs with
         * maskable interrupts enabled.
         */
        if (processor->LocalScheduler != nullptr && processor->LocalScheduler->PreemptionEnabled())
            __asm__ volatile("sti" ::: "memory");

        thread->m_Entry(thread->m_EntryContext);
        thread->m_State = ThreadState::Terminated;
        Diagnostics::Fatal("Thread", "kernel thread entry returned before termination scheduling exists");
    }

    ThreadUserModeError Thread::ConfigureUserMode(Memory::VirtualAddress entry, Memory::VirtualAddress stack_top) noexcept {
        if (!m_Initialized) return ThreadUserModeError::NotInitialized;
        if (m_UserModeConfigured) return ThreadUserModeError::AlreadyConfigured;
        if (m_State != ThreadState::Ready) return ThreadUserModeError::InvalidState;
        if (m_OwningProcess == nullptr || m_OwningProcess->AddressSpace() == nullptr)
            return ThreadUserModeError::InvalidProcess;

        if (entry.IsNull() || stack_top.IsNull() || !Architecture::AMD64::PageMap::IsCanonical(entry) || !Architecture::AMD64::PageMap::IsCanonical(stack_top) ||
            entry.Value() >= 0x0000800000000000ULL || stack_top.Value() >= 0x0000800000000000ULL || (stack_top.Value() & 0xFULL) != 0 || stack_top.Value() < sizeof(Memory::Uint64))
            return ThreadUserModeError::InvalidState;

        const Architecture::AMD64::PageMap* address_space = m_OwningProcess->AddressSpace();
        const auto code = address_space->Translate(entry);
        if (!code.Mapped ||
            !Memory::HasAccess(code.Options.Access, Memory::PageAccess::Read) ||
            !Memory::HasAccess(code.Options.Access, Memory::PageAccess::Execute) ||
            !Memory::HasAccess(code.Options.Access, Memory::PageAccess::User) ||
            Memory::HasAccess(code.Options.Access, Memory::PageAccess::Write) ||
            Memory::HasAccess(code.Options.Access, Memory::PageAccess::Global))
            return ThreadUserModeError::InvalidEntryMapping;

        const auto stack = address_space->Translate(stack_top - sizeof(Memory::Uint64));
        if (!stack.Mapped ||
            !Memory::HasAccess(stack.Options.Access, Memory::PageAccess::Read) ||
            !Memory::HasAccess(stack.Options.Access, Memory::PageAccess::Write) ||
            !Memory::HasAccess(stack.Options.Access, Memory::PageAccess::User) ||
            Memory::HasAccess(stack.Options.Access, Memory::PageAccess::Execute) ||
            Memory::HasAccess(stack.Options.Access, Memory::PageAccess::Global))
            return ThreadUserModeError::InvalidStackMapping;

        m_UserEntry = entry;
        m_UserStackTop = stack_top;
        m_UserModeConfigured = true;

        return ThreadUserModeError::Success;
    }

    [[noreturn]] void Thread::EnterUserMode() noexcept {
        if (!m_Initialized || !m_UserModeConfigured || m_State != ThreadState::Running ||
            m_OwningProcess == nullptr || m_OwningProcess->AddressSpace() == nullptr ||
            !m_OwningProcess->AddressSpace()->IsActive() || m_Stack == nullptr)
            Diagnostics::Fatal("Thread", "user-mode transition requested from invalid thread state");

        Architecture::AMD64::ProcessorLocal* processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != this)
            Diagnostics::Fatal("Thread", "user-mode transition thread does not own the processor");

        if (Architecture::AMD64::CurrentKernelEntryStack() != m_Stack->Top())
            Diagnostics::Fatal("Thread", "kernel entry stack does not match the user thread kernel stack");

        Architecture::AMD64::EnterUserMode(m_UserEntry, m_UserStackTop);
    }

    void Thread::Print() const noexcept {
        Diagnostics::Write(m_OwnsStack ? "[zOS/Thread] Kernel thread created.\n" : "[zOS/Thread] Bootstrap thread initialized.\n");

        Diagnostics::Write("[zOS/Thread] Thread ID: ");
        Diagnostics::WriteDecimal(m_Id);
        Diagnostics::Write("\n");

        if (m_OwningProcess != nullptr) {
            Diagnostics::Write("[zOS/Thread] Process ID: ");
            Diagnostics::WriteDecimal(m_OwningProcess->Id());
            Diagnostics::Write("\n");
        }

        Diagnostics::Write("[zOS/Thread] State: ");
        Diagnostics::Write(StateName(m_State));
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Thread] Stack ownership: ");
        Diagnostics::Write(m_OwnsStack ? "owned\n" : "borrowed\n");

        if (m_Stack != nullptr) {
            Diagnostics::Write("[zOS/Thread] Stack bottom: ");
            Diagnostics::WriteHex(m_Stack->Bottom().Value());
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Thread] Stack top: ");
            Diagnostics::WriteHex(m_Stack->Top().Value());
            Diagnostics::Write("\n");
        }
    }

    const char* Thread::StateName(ThreadState state) noexcept {
        switch (state) {
        case ThreadState::Ready: return "ready";
        case ThreadState::Running: return "running";
        case ThreadState::Terminated: return "terminated";
        default: return "unknown";
        }
    }

    const char* Thread::Describe(ThreadInitializationError error) noexcept {
        switch (error) {
        case ThreadInitializationError::Success: return "success";
        case ThreadInitializationError::AlreadyInitialized: return "thread is already initialized";
        case ThreadInitializationError::InvalidDependency: return "thread initialization dependency is invalid";
        case ThreadInitializationError::InvalidEntry: return "kernel thread entry point is invalid";
        case ThreadInitializationError::StackInitializationFailed: return "failed to initialize the kernel thread stack";
        case ThreadInitializationError::ContextInitializationFailed: return "failed to prepare the initial kernel thread context";
        default: return "unknown thread initialization error";
        }
    }

    const char* Thread::Describe(ThreadSwitchError error) noexcept {
        switch (error) {
        case ThreadSwitchError::Success: return "success";
        case ThreadSwitchError::InvalidThread: return "cooperative switch thread is invalid";
        case ThreadSwitchError::InterruptsEnabled: return "cooperative context switching requires maskable interrupts to be disabled";
        case ThreadSwitchError::NotCurrentThread: return "outgoing thread is not the current processor thread";
        case ThreadSwitchError::CurrentStateInvalid: return "outgoing thread is not running";
        case ThreadSwitchError::TargetNotReady: return "target thread is not ready or has no resumable context";
        case ThreadSwitchError::ResumeStateMismatch: return "resumed thread state does not match processor ownership";
        case ThreadSwitchError::AddressSpaceSwitchFailed: return "failed to activate the target thread process address space";
        case ThreadSwitchError::ResumeAddressSpaceMismatch: return "resumed thread address space does not match process ownership";
        case ThreadSwitchError::KernelPrivilegeStackUpdateFailed: return "failed to update the target thread kernel-entry stack";
        case ThreadSwitchError::ResumeKernelPrivilegeStackMismatch: return "resumed thread does not own the active kernel-entry stack";
        default: return "unknown cooperative thread switch error";
        }
    }

    const char* Thread::Describe(ThreadUserModeError error) noexcept {
        switch (error) {
        case ThreadUserModeError::Success: return "success";
        case ThreadUserModeError::NotInitialized: return "thread is not initialized";
        case ThreadUserModeError::AlreadyConfigured: return "thread already has user-mode execution state";
        case ThreadUserModeError::InvalidState: return "thread state is invalid for user-mode configuration";
        case ThreadUserModeError::InvalidProcess: return "thread does not have a valid owning process address space";
        case ThreadUserModeError::InvalidEntryMapping: return "user entry point is not mapped executable user memory";
        case ThreadUserModeError::InvalidStackMapping: return "user stack is not mapped writable user memory";
        default: return "unknown thread user-mode configuration error";
        }
    }
}
