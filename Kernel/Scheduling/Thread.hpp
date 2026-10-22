#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Architecture/AMD64/ContextSwitching.hpp>
#include <Kernel/Memory/KernelStack.hpp>

namespace Zos::Kernel::Scheduling {
    using ThreadId = Memory::Uint64;
    using ThreadEntry = void(*)(void* context) noexcept;

    enum class ThreadState : Memory::Uint32 {
        Ready,
        Running,
        Terminated,
    };

    enum class ThreadInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        InvalidEntry,
        StackInitializationFailed,
        ContextInitializationFailed,
    };

    enum class ThreadSwitchError : Memory::Uint32 {
        Success,
        InvalidThread,
        InterruptsEnabled,
        NotCurrentThread,
        CurrentStateInvalid,
        TargetNotReady,
        ResumeStateMismatch,
    };

    class Thread final {
    public:
        constexpr Thread() noexcept = default;

        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;

        /*
         * Represents execution that is already running on an existing
         * kernel-owned stack. The thread borrows the stack; it does not
         * acquire a second ownership claim over it.
         */
        [[nodiscard]] ThreadInitializationError InitializeBootstrap(ThreadId id, Memory::KernelStack& current_stack) noexcept;

        /*
         * Creates a dormant kernel thread with its own guarded stack and
         * prepares the initial architectural context used on first dispatch.
         */
        [[nodiscard]] ThreadInitializationError InitializeKernel(ThreadId id, ThreadEntry entry, void* entry_context, Memory::PhysicalMemoryManager& physical_memory, 
            Memory::VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map, Memory::Uint64 stack_page_count) noexcept;

        /*
         * Low-level cooperative transfer from this Running thread to a
         * Ready thread. IF must already be clear.
         *
         * Once a Scheduler is active, normal scheduling goes through
         * Scheduler::Yield() rather than selecting threads directly.
         */
        [[nodiscard]] ThreadSwitchError SwitchTo(Thread& next) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] ThreadId Id() const noexcept { return m_Id; }
        [[nodiscard]] ThreadState State() const noexcept { return m_State; }

        [[nodiscard]] ThreadEntry Entry() const noexcept { return m_Entry; }
        [[nodiscard]] void* EntryContext() const noexcept { return m_EntryContext; }

        [[nodiscard]] Memory::KernelStack* Stack() noexcept { return m_Stack; }
        [[nodiscard]] const Memory::KernelStack* Stack() const noexcept { return m_Stack; }
        [[nodiscard]] bool OwnsStack() const noexcept { return m_OwnsStack; }
        [[nodiscard]] bool HasContext() const noexcept { return m_ContextReady; }


        [[nodiscard]] static const char* StateName(ThreadState state) noexcept;
        [[nodiscard]] static const char* Describe(ThreadInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(ThreadSwitchError error) noexcept;

    private:
        friend class Scheduler;

        static void StartContext(void* argument) noexcept;
        void Print() const noexcept;

        ThreadId m_Id{};
        ThreadState m_State{ ThreadState::Ready };

        ThreadEntry m_Entry{};
        void* m_EntryContext{};

        Memory::KernelStack m_OwnedStack{};
        Memory::KernelStack* m_Stack{};

        Architecture::AMD64::ThreadContext m_Context{};
        Thread* m_ReadyNext{};

        bool m_OwnsStack{};
        bool m_ContextReady{};
        bool m_ReadyQueued{};
        bool m_Initialized{};
    };
}