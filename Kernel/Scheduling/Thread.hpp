#pragma once

#include <Kernel/Architecture/AMD64/ContextSwitching.hpp>
#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/KernelStack.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Objects/Object.hpp>

namespace Zos::Kernel::Processes {
    class Process;
}

namespace Zos::Kernel::Scheduling {
    using ThreadId = Memory::Uint64;
    using ThreadEntry = void(*)(void* context) noexcept;

    inline constexpr Objects::AccessMask ThreadQuery{ 1ULL << 0 };

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
        ObjectInitializationFailed,
    };

    enum class ThreadSwitchError : Memory::Uint32 {
        Success,
        InvalidThread,
        InterruptsEnabled,
        NotCurrentThread,
        CurrentStateInvalid,
        TargetNotReady,
        ResumeStateMismatch,
        AddressSpaceSwitchFailed,
        ResumeAddressSpaceMismatch,
        KernelPrivilegeStackUpdateFailed,
        ResumeKernelPrivilegeStackMismatch,
    };

    enum class ThreadUserModeError : Memory::Uint32 {
        Success,
        NotInitialized,
        AlreadyConfigured,
        InvalidState,
        InvalidProcess,
        InvalidEntryMapping,
        InvalidStackMapping,
    };

    class Thread final : public Objects::Object {
    public:
        inline static constexpr Objects::ObjectType ObjectKind{ Objects::ObjectType::Thread };

        constexpr Thread() noexcept = default;

        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;

        [[nodiscard]] ThreadInitializationError InitializeBootstrap(ThreadId id, Processes::Process& owning_process, Memory::KernelStack& current_stack) noexcept;

        [[nodiscard]] ThreadInitializationError InitializeKernel(ThreadId id, Processes::Process& owning_process, ThreadEntry entry, void* entry_context, Memory::PhysicalMemoryManager& physical_memory,
            Memory::VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map, Memory::Uint64 stack_page_count) noexcept;

        [[nodiscard]] ThreadSwitchError SwitchTo(Thread& next) noexcept;

        [[nodiscard]] ThreadUserModeError ConfigureUserMode(Memory::VirtualAddress entry, Memory::VirtualAddress stack_top) noexcept;

        [[noreturn]] void EnterUserMode() noexcept;

        [[nodiscard]] bool UserModeConfigured() const noexcept { return m_UserModeConfigured; }

        [[nodiscard]] Memory::VirtualAddress UserEntry() const noexcept { return m_UserEntry; }
        [[nodiscard]] Memory::VirtualAddress UserStackTop() const noexcept { return m_UserStackTop; }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] ThreadId Id() const noexcept { return m_Id; }
        [[nodiscard]] ThreadState State() const noexcept { return m_State; }

        [[nodiscard]] ThreadEntry Entry() const noexcept { return m_Entry; }
        [[nodiscard]] void* EntryContext() const noexcept { return m_EntryContext; }

        [[nodiscard]] Processes::Process* OwningProcess() noexcept { return m_OwningProcess; }
        [[nodiscard]] const Processes::Process* OwningProcess() const noexcept { return m_OwningProcess; }

        [[nodiscard]] Memory::KernelStack* Stack() noexcept { return m_Stack; }
        [[nodiscard]] const Memory::KernelStack* Stack() const noexcept { return m_Stack; }

        [[nodiscard]] bool OwnsStack() const noexcept { return m_OwnsStack; }
        [[nodiscard]] bool HasContext() const noexcept { return m_ContextReady; }

        [[nodiscard]] static const char* StateName(ThreadState state) noexcept;
        [[nodiscard]] static const char* Describe(ThreadInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(ThreadSwitchError error) noexcept;
        [[nodiscard]] static const char* Describe(ThreadUserModeError error) noexcept;

    private:
        friend class Scheduler;
        friend class ::Zos::Kernel::Processes::Process;

        enum class TransferDisposition : Memory::Uint8 {
            Ready,
            Terminated,
        };

        [[nodiscard]] ThreadSwitchError TransferTo(Thread& next, TransferDisposition disposition) noexcept;

        [[noreturn]] void TerminateAndSwitchTo(Thread& next) noexcept;
        void MarkTerminated() noexcept;
        static void StartContext(void* argument) noexcept;
        void Print() const noexcept;

        ThreadId m_Id{};
        ThreadState m_State{ ThreadState::Ready };

        Processes::Process* m_OwningProcess{};
        Thread* m_ProcessNext{};

        ThreadEntry m_Entry{};
        void* m_EntryContext{};

        Memory::KernelStack m_OwnedStack{};
        Memory::KernelStack* m_Stack{};

        Architecture::AMD64::ThreadContext m_Context{};
        Thread* m_ReadyNext{};

        Memory::VirtualAddress m_UserEntry{};
        Memory::VirtualAddress m_UserStackTop{};

        bool m_OwnsStack{};
        bool m_ContextReady{};
        bool m_ReadyQueued{};
        bool m_Initialized{};
        bool m_UserModeConfigured{};
    };
}
