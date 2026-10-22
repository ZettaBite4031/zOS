#include <Kernel/Scheduling/Scheduler.hpp>

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

    SchedulerInitializationError Scheduler::Initialize(Architecture::AMD64::ProcessorLocal& processor) noexcept {
        if (m_Initialized) return SchedulerInitializationError::AlreadyInitialized;
        if (!processor.IsInitialized()) return SchedulerInitializationError::InvalidDependency;
        if (processor.LocalScheduler != nullptr) return SchedulerInitializationError::ProcessorAlreadyBound;

        Thread* current = processor.CurrentThread;
        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running)
            return SchedulerInitializationError::CurrentThreadInvalid;

        m_Processor = &processor;
        processor.LocalScheduler = this;

        m_Initialized = true;
        return SchedulerInitializationError::Success;
    }

    SchedulerRegistrationError Scheduler::RegisterThread(Thread& thread) noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerRegistrationError::NotInitialized;
        if (!thread.IsInitialized() || thread.State() != ThreadState::Ready || !thread.HasContext())
            return SchedulerRegistrationError::InvalidThread;

        if (m_Processor->CurrentThread == &thread) return SchedulerRegistrationError::CurrentThread;
        if (thread.m_ReadyQueued || thread.m_ReadyNext != nullptr)
            return SchedulerRegistrationError::AlreadyQueued;

        if (!EnqueueReady(thread)) return SchedulerRegistrationError::AlreadyQueued;
        return SchedulerRegistrationError::Success;
    }

    bool Scheduler::EnqueueReady(Thread& thread) noexcept {
        if (thread.m_ReadyQueued || thread.m_ReadyNext != nullptr)
            return false;

        thread.m_ReadyNext = nullptr;
        thread.m_ReadyQueued = true;

        if (m_ReadyTail != nullptr) m_ReadyTail->m_ReadyNext = &thread;
        else m_ReadyHead = &thread;

        m_ReadyTail = &thread;
        m_ReadyCount++;

        return true;
    }

    Thread* Scheduler::DequeueReady() noexcept {
        Thread* thread = m_ReadyHead;
        if (thread == nullptr) return nullptr;

        m_ReadyHead = thread->m_ReadyNext;
        if (m_ReadyHead == nullptr) m_ReadyTail = nullptr;

        thread->m_ReadyNext = nullptr;
        thread->m_ReadyQueued = false;
        if (m_ReadyCount == 0) return nullptr;

        m_ReadyCount--;
        return thread;
    }

    SchedulerYieldError Scheduler::Yield() noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerYieldError::NotInitialized;
        if ((ReadRflags() & RflagsInterruptEnable) != 0) return SchedulerYieldError::InterruptsEnabled;

        Thread* current = m_Processor->CurrentThread;
        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr)
            return SchedulerYieldError::InvalidCurrentThread;

        /*
         * A single runnable thread has nothing to yield to.
         */
        if (m_ReadyHead == nullptr) {
            if (m_ReadyTail != nullptr || m_ReadyCount != 0)
                return SchedulerYieldError::ReadyQueueCorrupted;
            return SchedulerYieldError::Success;
        }

        if (m_ReadyTail == nullptr || m_ReadyCount == 0)
            return SchedulerYieldError::ReadyQueueCorrupted;

        Thread* next = m_ReadyHead;
        if (!next->IsInitialized() || next->State() != ThreadState::Ready ||
            !next->HasContext() || !next->m_ReadyQueued)
            return SchedulerYieldError::ReadyQueueCorrupted;

        next = DequeueReady();
        if (next == nullptr)
            return SchedulerYieldError::ReadyQueueCorrupted;

        /*
         * The current thread remains Running until Thread::SwitchTo commits
         * the ownership transition, but it must already be visible in the
         * ready queue so the incoming thread can select it immediately.
         *
         * IF is clear, so no asynchronous observer can see this transient
         * queue/state mismatch.
         */
        if (!EnqueueReady(*current))
            return SchedulerYieldError::ReadyQueueCorrupted;

        m_SwitchCount++;

        const ThreadSwitchError switch_error = current->SwitchTo(*next);
        if (switch_error != ThreadSwitchError::Success)
            return SchedulerYieldError::ContextSwitchFailed;

        /*
         * We only return here after another thread selected this thread from
         * the ready queue and restored the suspended Yield() call frame.
         */
        if (m_Processor->CurrentThread != current || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr)
            return SchedulerYieldError::ReadyQueueCorrupted;

        return SchedulerYieldError::Success;
    }

    const char* Scheduler::Describe(SchedulerInitializationError error) noexcept {
        switch (error) {
        case SchedulerInitializationError::Success: return "success";
        case SchedulerInitializationError::AlreadyInitialized: return "scheduler is already initialized";
        case SchedulerInitializationError::InvalidDependency: return "scheduler processor dependency is invalid";
        case SchedulerInitializationError::ProcessorAlreadyBound: return "processor already has a scheduler";
        case SchedulerInitializationError::CurrentThreadInvalid: return "processor does not have a valid running current thread";
        default: return "unknown scheduler initialization error";
        }
    }

    const char* Scheduler::Describe(SchedulerRegistrationError error) noexcept {
        switch (error) {
        case SchedulerRegistrationError::Success: return "success";
        case SchedulerRegistrationError::NotInitialized: return "scheduler is not initialized";
        case SchedulerRegistrationError::InvalidThread: return "thread is not a valid ready scheduling candidate";
        case SchedulerRegistrationError::CurrentThread: return "current running thread cannot be registered as ready";
        case SchedulerRegistrationError::AlreadyQueued: return "thread is already present in a scheduler ready queue";
        default: return "unknown scheduler registration error";
        }
    }

    const char* Scheduler::Describe(SchedulerYieldError error) noexcept {
        switch (error) {
        case SchedulerYieldError::Success: return "success";
        case SchedulerYieldError::NotInitialized: return "scheduler is not initialized";
        case SchedulerYieldError::InterruptsEnabled: return "cooperative scheduling currently requires maskable interrupts to be disabled";
        case SchedulerYieldError::InvalidCurrentThread: return "scheduler current thread state is invalid";
        case SchedulerYieldError::ReadyQueueCorrupted: return "scheduler ready queue invariants were violated";
        case SchedulerYieldError::ContextSwitchFailed: return "scheduler context switch failed";
        default: return "unknown scheduler yield error";
        }
    }
}
