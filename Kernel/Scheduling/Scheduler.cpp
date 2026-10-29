#include <Kernel/Scheduling/Scheduler.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Scheduling {
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
        if (!thread.IsInitialized() || thread.State() != ThreadState::Ready || !thread.HasContext() ||
            thread.m_WaitQueue != nullptr || thread.m_WaitNext != nullptr)
            return SchedulerRegistrationError::InvalidThread;

        if (m_Processor->CurrentThread == &thread) return SchedulerRegistrationError::CurrentThread;
        if (thread.m_ReadyQueued || thread.m_ReadyNext != nullptr)
            return SchedulerRegistrationError::AlreadyQueued;

        if (!EnqueueReady(thread))
            return SchedulerRegistrationError::AlreadyQueued;

        return SchedulerRegistrationError::Success;
    }

    bool Scheduler::EnqueueReady(Thread& thread) noexcept {
        if (thread.m_ReadyQueued || thread.m_ReadyNext != nullptr || thread.m_WaitQueue != nullptr || thread.m_WaitNext != nullptr)
            return false;

        thread.m_ReadyNext = nullptr;
        thread.m_ReadyQueued = true;

        if (m_ReadyTail != nullptr) m_ReadyTail->m_ReadyNext = &thread;
        else m_ReadyHead = &thread;

        m_ReadyTail = &thread;
        ++m_ReadyCount;

        return true;
    }

    Thread* Scheduler::DequeueReady() noexcept {
        if (m_ReadyHead == nullptr || m_ReadyTail == nullptr || m_ReadyCount == 0)
            return nullptr;

        Thread* thread = m_ReadyHead;
        m_ReadyHead = thread->m_ReadyNext;
        if (m_ReadyHead == nullptr) m_ReadyTail = nullptr;

        thread->m_ReadyNext = nullptr;
        thread->m_ReadyQueued = false;
        --m_ReadyCount;

        return thread;
    }

    SchedulerYieldError Scheduler::Yield() noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerYieldError::NotInitialized;
        if (Architecture::AMD64::InterruptsEnabled()) return SchedulerYieldError::InterruptsEnabled;

        Thread* current = m_Processor->CurrentThread;

        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr ||
            current->m_WaitNext != nullptr)
            return SchedulerYieldError::InvalidCurrentThread;

        if (m_ReadyHead == nullptr) {
            if (m_ReadyTail != nullptr || m_ReadyCount != 0)
                return SchedulerYieldError::ReadyQueueCorrupted;
            return SchedulerYieldError::Success;
        }

        if (m_ReadyTail == nullptr || m_ReadyCount == 0)
            return SchedulerYieldError::ReadyQueueCorrupted;

        Thread* next = m_ReadyHead;
        if (!next->IsInitialized() || next->State() != ThreadState::Ready || !next->HasContext() ||
            !next->m_ReadyQueued || next->m_WaitQueue != nullptr || next->m_WaitNext != nullptr)
            return SchedulerYieldError::ReadyQueueCorrupted;

        next = DequeueReady();
        if (next == nullptr) return SchedulerYieldError::ReadyQueueCorrupted;
        if (!EnqueueReady(*current))
            return SchedulerYieldError::ReadyQueueCorrupted;

        ++m_SwitchCount;

        const ThreadSwitchError switch_error = current->SwitchTo(*next);
        if (switch_error != ThreadSwitchError::Success)
            return SchedulerYieldError::ContextSwitchFailed;

        if (m_Processor->CurrentThread != current || current->State() != ThreadState::Running || current->m_ReadyQueued ||
            current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr || current->m_WaitNext != nullptr)
            return SchedulerYieldError::ReadyQueueCorrupted;

        return SchedulerYieldError::Success;
    }

    bool Scheduler::EnablePreemption() noexcept {
        if (!m_Initialized || m_Processor == nullptr || m_PreemptionEnabled)
            return false;

        if (Architecture::AMD64::InterruptsEnabled())
            return false;

        Thread* current = m_Processor->CurrentThread;
        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr ||
            current->m_WaitNext != nullptr)
            return false;

        if ((m_ReadyHead == nullptr) != (m_ReadyTail == nullptr))
            return false;

        if ((m_ReadyHead == nullptr) != (m_ReadyCount == 0))
            return false;

        m_PreemptionEnabled = true;
        return true;
    }

    SchedulerPreemptionError Scheduler::Preempt() noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerPreemptionError::NotInitialized;
        if (!m_PreemptionEnabled) return SchedulerPreemptionError::Disabled;
        if (Architecture::AMD64::InterruptsEnabled())
            return SchedulerPreemptionError::InterruptsEnabled;

        Thread* current = m_Processor->CurrentThread;
        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr ||
            current->m_WaitNext != nullptr)
            return SchedulerPreemptionError::InvalidCurrentThread;

        if (m_ReadyHead == nullptr) {
            if (m_ReadyTail != nullptr || m_ReadyCount != 0)
                return SchedulerPreemptionError::ReadyQueueCorrupted;

            return SchedulerPreemptionError::Success;
        }

        if (m_ReadyTail == nullptr || m_ReadyCount == 0)
            return SchedulerPreemptionError::ReadyQueueCorrupted;

        Thread* next = m_ReadyHead;
        if (!next->IsInitialized() || next->State() != ThreadState::Ready || !next->HasContext() ||
            !next->m_ReadyQueued || next->m_WaitQueue != nullptr || next->m_WaitNext != nullptr)
            return SchedulerPreemptionError::ReadyQueueCorrupted;

        next = DequeueReady();
        if (next == nullptr) return SchedulerPreemptionError::ReadyQueueCorrupted;
        if (!EnqueueReady(*current))
            return SchedulerPreemptionError::ReadyQueueCorrupted;

        ++m_SwitchCount;
        ++m_PreemptionCount;

        const ThreadSwitchError switch_error = current->SwitchTo(*next);
        if (switch_error != ThreadSwitchError::Success)
            return SchedulerPreemptionError::ContextSwitchFailed;

        if (m_Processor->CurrentThread != current || current->State() != ThreadState::Running || current->m_ReadyQueued ||
            current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr || current->m_WaitNext != nullptr)
            return SchedulerPreemptionError::ReadyQueueCorrupted;

        return SchedulerPreemptionError::Success;
    }

    SchedulerBlockError Scheduler::BlockCurrent() noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerBlockError::NotInitialized;
        if (Architecture::AMD64::InterruptsEnabled()) return SchedulerBlockError::InterruptsEnabled;

        Thread* current = m_Processor->CurrentThread;

        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running ||
            current->m_ReadyQueued || current->m_ReadyNext != nullptr)
            return SchedulerBlockError::InvalidCurrentThread;

        if (current->m_WaitQueue == nullptr || current->m_WaitNext != nullptr)
            return SchedulerBlockError::InvalidWaitState;

        if (m_ReadyHead == nullptr) {
            if (m_ReadyTail != nullptr || m_ReadyCount != 0)
                return SchedulerBlockError::ReadyQueueCorrupted;
            return SchedulerBlockError::NoRunnableThread;
        }

        if (m_ReadyTail == nullptr || m_ReadyCount == 0)
            return SchedulerBlockError::ReadyQueueCorrupted;

        Thread* next = m_ReadyHead;
        if (!next->IsInitialized() || next->State() != ThreadState::Ready || !next->HasContext() ||
            !next->m_ReadyQueued || next->m_WaitQueue != nullptr || next->m_WaitNext != nullptr)
            return SchedulerBlockError::ReadyQueueCorrupted;

        next = DequeueReady();
        if (next == nullptr)
            return SchedulerBlockError::ReadyQueueCorrupted;

        ++m_BlockedCount;
        ++m_SwitchCount;

        const ThreadSwitchError switch_error = current->TransferTo(*next, Thread::TransferDisposition::Blocked);
        if (switch_error != ThreadSwitchError::Success) {
            /*
             * A failure before the architectural transfer leaves the current
             * thread Running and still attached to its WaitQueue. Restore the
             * scheduling accounting and make the target runnable again.
             */
            if (current->State() == ThreadState::Running && current->m_WaitQueue != nullptr) {
                if (m_BlockedCount == 0 || !EnqueueReady(*next))
                    return SchedulerBlockError::ReadyQueueCorrupted;

                --m_BlockedCount;
            }

            return SchedulerBlockError::ContextSwitchFailed;
        }

        if (m_Processor->CurrentThread != current || current->State() != ThreadState::Running || current->m_ReadyQueued ||
            current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr || current->m_WaitNext != nullptr)
            return SchedulerBlockError::ReadyQueueCorrupted;

        return SchedulerBlockError::Success;
    }

    SchedulerWakeError Scheduler::Wake(Thread& thread) noexcept {
        if (!m_Initialized || m_Processor == nullptr) return SchedulerWakeError::NotInitialized;
        if (Architecture::AMD64::InterruptsEnabled()) return SchedulerWakeError::InterruptsEnabled;

        if (!thread.IsInitialized() || thread.State() != ThreadState::Blocked || !thread.HasContext() ||
            thread.m_ReadyQueued || thread.m_ReadyNext != nullptr)
            return SchedulerWakeError::InvalidThread;

        if (thread.m_WaitQueue != nullptr || thread.m_WaitNext != nullptr)
            return SchedulerWakeError::InvalidWaitState;

        if (m_BlockedCount == 0)
            return SchedulerWakeError::ReadyQueueCorrupted;

        thread.m_State = ThreadState::Ready;
        if (!EnqueueReady(thread)) {
            thread.m_State = ThreadState::Blocked;
            return SchedulerWakeError::ReadyQueueCorrupted;
        }

        --m_BlockedCount;
        return SchedulerWakeError::Success;
    }

    [[noreturn]] void Scheduler::TerminateCurrent() noexcept {
        if (!m_Initialized || m_Processor == nullptr)
            Diagnostics::Fatal("Scheduler", "thread termination requested before scheduler initialization");

        if (Architecture::AMD64::InterruptsEnabled())
            Diagnostics::Fatal("Scheduler", "thread termination requires interrupts disabled");

        Thread* current = m_Processor->CurrentThread;
        if (current == nullptr || !current->IsInitialized() || current->State() != ThreadState::Running || current->m_ReadyQueued ||
            current->m_ReadyNext != nullptr || current->m_WaitQueue != nullptr || current->m_WaitNext != nullptr)
            Diagnostics::Fatal("Scheduler", "current thread is invalid for termination");

        if (m_ReadyHead == nullptr || m_ReadyTail == nullptr || m_ReadyCount == 0)
            Diagnostics::Fatal("Scheduler", "cannot terminate the final runnable thread");

        Thread* next = m_ReadyHead;
        if (!next->IsInitialized() || next->State() != ThreadState::Ready || !next->HasContext() || !next->m_ReadyQueued ||
            next->m_Stack == nullptr || next->m_WaitQueue != nullptr || next->m_WaitNext != nullptr)
            Diagnostics::Fatal("Scheduler", "termination target is not a valid ready thread");

        next = DequeueReady();
        if (next == nullptr)
            Diagnostics::Fatal("Scheduler", "failed to dequeue termination target");

        ++m_SwitchCount;
        ++m_TerminationCount;

        current->TerminateAndSwitchTo(*next);
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
        case SchedulerYieldError::InterruptsEnabled: return "cooperative scheduling requires maskable interrupts to be disabled";
        case SchedulerYieldError::InvalidCurrentThread: return "scheduler current thread state is invalid";
        case SchedulerYieldError::ReadyQueueCorrupted: return "scheduler ready queue invariants were violated";
        case SchedulerYieldError::ContextSwitchFailed: return "scheduler context switch failed";
        default: return "unknown scheduler yield error";
        }
    }

    const char* Scheduler::Describe(SchedulerPreemptionError error) noexcept {
        switch (error) {
        case SchedulerPreemptionError::Success: return "success";
        case SchedulerPreemptionError::NotInitialized: return "scheduler is not initialized";
        case SchedulerPreemptionError::Disabled: return "scheduler preemption is disabled";
        case SchedulerPreemptionError::InterruptsEnabled: return "preemption path entered with maskable interrupts enabled";
        case SchedulerPreemptionError::InvalidCurrentThread: return "preemption current thread state is invalid";
        case SchedulerPreemptionError::ReadyQueueCorrupted: return "scheduler ready queue invariants were violated during preemption";
        case SchedulerPreemptionError::ContextSwitchFailed: return "preemptive context switch failed";
        default: return "unknown scheduler preemption error";
        }
    }

    const char* Scheduler::Describe(SchedulerBlockError error) noexcept {
        switch (error) {
        case SchedulerBlockError::Success: return "success";
        case SchedulerBlockError::NotInitialized: return "scheduler is not initialized";
        case SchedulerBlockError::InterruptsEnabled: return "blocking requires maskable interrupts to be disabled";
        case SchedulerBlockError::InvalidCurrentThread: return "current thread is invalid for blocking";
        case SchedulerBlockError::InvalidWaitState: return "current thread is not attached to exactly one wait queue";
        case SchedulerBlockError::NoRunnableThread: return "cannot block the final runnable thread";
        case SchedulerBlockError::ReadyQueueCorrupted: return "scheduler ready/blocked accounting is inconsistent";
        case SchedulerBlockError::ContextSwitchFailed: return "blocking context switch failed";
        default: return "unknown scheduler block error";
        }
    }

    const char* Scheduler::Describe(SchedulerWakeError error) noexcept {
        switch (error) {
        case SchedulerWakeError::Success: return "success";
        case SchedulerWakeError::NotInitialized: return "scheduler is not initialized";
        case SchedulerWakeError::InterruptsEnabled: return "waking requires maskable interrupts to be disabled";
        case SchedulerWakeError::InvalidThread: return "thread is not a valid blocked scheduling candidate";
        case SchedulerWakeError::InvalidWaitState: return "thread is still attached to a wait queue";
        case SchedulerWakeError::ReadyQueueCorrupted: return "scheduler ready/blocked accounting is inconsistent";
        default: return "unknown scheduler wake error";
        }
    }
}
