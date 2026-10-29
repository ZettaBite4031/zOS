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

    enum class SchedulerPreemptionError : Memory::Uint32 {
        Success,
        NotInitialized,
        Disabled,
        InterruptsEnabled,
        InvalidCurrentThread,
        ReadyQueueCorrupted,
        ContextSwitchFailed,
    };

    enum class SchedulerBlockError : Memory::Uint32 {
        Success,
        NotInitialized,
        InterruptsEnabled,
        InvalidCurrentThread,
        InvalidWaitState,
        NoRunnableThread,
        ReadyQueueCorrupted,
        ContextSwitchFailed,
    };

    enum class SchedulerWakeError : Memory::Uint32 {
        Success,
        NotInitialized,
        InterruptsEnabled,
        InvalidThread,
        InvalidWaitState,
        ReadyQueueCorrupted,
    };

    class Scheduler final {
    public:
        constexpr Scheduler() noexcept = default;

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        [[nodiscard]] SchedulerInitializationError Initialize(Architecture::AMD64::ProcessorLocal& processor) noexcept;

        [[nodiscard]] SchedulerRegistrationError RegisterThread(Thread& thread) noexcept;

        /*
         * Cooperative scheduling path. IF must already be clear.
         */
        [[nodiscard]] SchedulerYieldError Yield() noexcept;

        /*
         * Enables timer-driven scheduling on this processor.
         *
         * The transition itself must occur with IF clear so the timer cannot
         * observe partially published scheduler state.
         */
        [[nodiscard]] bool EnablePreemption() noexcept;

        /*
         * Timer-interrupt scheduling path. The interrupt gate has already
         * cleared IF and the interrupt manager has already acknowledged the
         * Local APIC timer before this can switch stacks.
         */
        [[nodiscard]] SchedulerPreemptionError Preempt() noexcept;

        /*
         * Removes the current Running thread from the runnable set until a
         * WaitQueue wakes it. IF must be clear and another Ready thread must
         * exist because zOS does not yet have a dedicated idle thread.
         */
        [[nodiscard]] SchedulerBlockError BlockCurrent() noexcept;

        /*
         * Moves one Blocked thread back to the Ready queue. WaitQueue removes
         * the thread from its wait list before calling this method.
         */
        [[nodiscard]] SchedulerWakeError Wake(Thread& thread) noexcept;


        /*
         * Permanently removes the current Running thread and transfers execution
         * to the next Ready thread. This is the non-returning path used for fatal
         * user exceptions and later for ordinary thread exit.
         */
        [[noreturn]] void TerminateCurrent() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] bool PreemptionEnabled() const noexcept { return m_PreemptionEnabled; }

        [[nodiscard]] Thread* CurrentThread() noexcept {
            return m_Processor != nullptr ? m_Processor->CurrentThread : nullptr;
        }

        [[nodiscard]] const Thread* CurrentThread() const noexcept {
            return m_Processor != nullptr ? m_Processor->CurrentThread : nullptr;
        }

        [[nodiscard]] Memory::Uint64 ReadyCount() const noexcept { return m_ReadyCount; }
        [[nodiscard]] Memory::Uint64 BlockedCount() const noexcept { return m_BlockedCount; }
        [[nodiscard]] Memory::Uint64 SwitchCount() const noexcept { return m_SwitchCount; }
        [[nodiscard]] Memory::Uint64 PreemptionCount() const noexcept { return m_PreemptionCount; }
        [[nodiscard]] Memory::Uint64 TerminationCount() const noexcept { return m_TerminationCount; }

        [[nodiscard]] static const char* Describe(SchedulerInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerRegistrationError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerYieldError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerPreemptionError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerBlockError error) noexcept;
        [[nodiscard]] static const char* Describe(SchedulerWakeError error) noexcept;

    private:
        [[nodiscard]] bool EnqueueReady(Thread& thread) noexcept;
        [[nodiscard]] Thread* DequeueReady() noexcept;

        Architecture::AMD64::ProcessorLocal* m_Processor{};

        Thread* m_ReadyHead{};
        Thread* m_ReadyTail{};

        Memory::Uint64 m_ReadyCount{};
        Memory::Uint64 m_BlockedCount{};
        Memory::Uint64 m_SwitchCount{};
        Memory::Uint64 m_PreemptionCount{};
        Memory::Uint64 m_TerminationCount{};

        bool m_PreemptionEnabled{};
        bool m_Initialized{};
    };
}