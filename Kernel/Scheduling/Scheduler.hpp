#pragma once

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Scheduling {
    enum class SchedulerInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        ProcessorAlreadyBound,
        CurrentThreadInvalid,
    };

    enum class SchedulerRegistrationError : Memory::Uint32 {
        Success,
        NotInitialized,
        InvalidThread,
        CurrentThread,
        AlreadyQueued,
    };

    enum class SchedulerYieldError : Memory::Uint32 {
        Success,
        NotInitialized,
        InterruptsEnabled,
        InvalidCurrentThread,
        ReadyQueueCorrupted,
        ContextSwitchFailed,
    };

    class Scheduler final {
    public:
        constexpr Scheduler() noexcept = default;

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        /*
         * Binds this scheduler to one processor. The processor must already
         * have a valid Running CurrentThread.
         */
        [[nodiscard]] SchedulerInitializationError Initialize(Architecture::AMD64::ProcessorLocal& processor) noexcept;

        /*
         * Adds an initialized Ready thread to the FIFO run queue.
         */
        [[nodiscard]] SchedulerRegistrationError RegisterThread(Thread& thread) noexcept;

        /*
         * Cooperatively rotate to the next Ready thread.
         *
         * Step 11 deliberately retains Step 10's IF=0 requirement. Step 12
         * will integrate scheduling with interrupt/preemption state.
         *
         * Yield with no alternate Ready thread is a valid no-op.
         */
        [[nodiscard]] SchedulerYieldError Yield() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint64 ReadyCount() const noexcept { return m_ReadyCount; }
        [[nodiscard]] Memory::Uint64 SwitchCount() const noexcept { return m_SwitchCount; }

        [[nodiscard]] static const char* Describe(SchedulerInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerRegistrationError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerYieldError error) noexcept;

    private:
        [[nodiscard]] bool EnqueueReady(Thread& thread) noexcept;
        [[nodiscard]] Thread* DequeueReady() noexcept;

        Architecture::AMD64::ProcessorLocal* m_Processor{};

        Thread* m_ReadyHead{};
        Thread* m_ReadyTail{};

        Memory::Uint64 m_ReadyCount{};
        Memory::Uint64 m_SwitchCount{};

        bool m_Initialized{};
    };
}