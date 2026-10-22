#include <Kernel/Scheduling/Thread.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Scheduling {
    ThreadInitializationError Thread::InitializeBootstrap(ThreadId id, Memory::KernelStack& current_stack) noexcept {
        if (m_Initialized || m_Stack != nullptr || m_OwnedStack.IsInitialized()) return ThreadInitializationError::AlreadyInitialized;
        if (!current_stack.IsInitialized()) return ThreadInitializationError::InvalidDependency;

        m_Id = id;
        m_State = ThreadState::Running;
        m_Entry = nullptr;
        m_EntryContext = nullptr;

        m_Stack = &current_stack;
        m_OwnsStack = false;
        m_Initialized = true;

        Print();
        return ThreadInitializationError::Success;
    }

    ThreadInitializationError Thread::InitializeKernel(ThreadId id, ThreadEntry entry, void* entry_context, Memory::PhysicalMemoryManager& physical_memory,
        Memory::VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map, Memory::Uint64 stack_page_count) noexcept {
            
        if (m_Initialized || m_Stack != nullptr || m_OwnedStack.IsInitialized())
            return ThreadInitializationError::AlreadyInitialized;

        if (!physical_memory.IsInitialized() || !virtual_addresses.IsInitialized() ||
            !page_map.IsInitialized() || !page_map.IsActive())
            return ThreadInitializationError::InvalidDependency;

        if (entry == nullptr)
            return ThreadInitializationError::InvalidEntry;

        const auto stack_error = m_OwnedStack.Initialize(physical_memory, virtual_addresses, page_map, stack_page_count);
        if (stack_error != Memory::KernelStackInitializationError::Success)
            return ThreadInitializationError::StackInitializationFailed;

        m_Id = id;
        m_State = ThreadState::Ready;
        m_Entry = entry;
        m_EntryContext = entry_context;

        m_Stack = &m_OwnedStack;
        m_OwnsStack = true;
        m_Initialized = true;

        Print();
        return ThreadInitializationError::Success;
    }

    void Thread::Print() const noexcept {
        Diagnostics::Write(m_OwnsStack ? "[zOS/Thread] Kernel thread created.\n" : "[zOS/Thread] Bootstrap thread initialized.\n");

        Diagnostics::Write("[zOS/Thread] Thread ID: ");
        Diagnostics::WriteDecimal(m_Id);
        Diagnostics::Write("\n");

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
        default: return "unknown thread initialization error";
        }
    }
}
