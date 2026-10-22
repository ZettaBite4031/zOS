#include <Kernel/Scheduling/Thread.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
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

        m_State = ThreadState::Ready;
        next.m_State = ThreadState::Running;
        processor->CurrentThread = &next;

        m_ContextReady = true;

        Architecture::AMD64::SwitchThreadContext(&m_Context, &next.m_Context);

        processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != this || m_State != ThreadState::Running)
            return ThreadSwitchError::ResumeStateMismatch;

        return ThreadSwitchError::Success;
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
        default: return "unknown cooperative thread switch error";
        }
    }
}
