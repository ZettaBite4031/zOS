#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Scheduling {
    class Thread;
}

namespace Zos::Kernel::Processes {
    using ProcessId = Memory::Uint64;

    enum class ProcessState : Memory::Uint32 {
        Active,
        Terminating,
        Terminated,
    };

    enum class ProcessInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
    };

    class Process final {
    public:
        constexpr Process() noexcept = default;

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        /*
         * Creates the kernel process. Step 13 does not give processes an
         * address space yet; Step 14 will add that ownership boundary.
         */
        [[nodiscard]] ProcessInitializationError InitializeKernel(ProcessId id) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] ProcessId Id() const noexcept { return m_Id; }
        [[nodiscard]] ProcessState State() const noexcept { return m_State; }
        [[nodiscard]] Memory::Uint64 ThreadCount() const noexcept { return m_ThreadCount; }

        [[nodiscard]] bool OwnsThread(const Scheduling::Thread& thread) const noexcept;

        [[nodiscard]] static const char* StateName(ProcessState state) noexcept;

        [[nodiscard]] static const char* Describe(ProcessInitializationError error) noexcept;

    private:
        friend class ::Zos::Kernel::Scheduling::Thread;

        /*
         * Thread initialization is the only path that may attach a thread.
         * This keeps Process membership and Thread::OwningProcess in lockstep.
         */
        void AttachThread(Scheduling::Thread& thread) noexcept;

        void Print() const noexcept;

        ProcessId m_Id{};
        ProcessState m_State{ ProcessState::Terminated };

        Scheduling::Thread* m_ThreadHead{};
        Scheduling::Thread* m_ThreadTail{};
        Memory::Uint64 m_ThreadCount{};

        bool m_Initialized{};
    };
}