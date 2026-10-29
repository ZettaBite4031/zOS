#include <Kernel/Synchronization/WaitQueue.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Synchronization {
    bool WaitQueue::Enqueue(Scheduling::Thread& thread) noexcept {
        if (!thread.IsInitialized() || thread.m_State != Scheduling::ThreadState::Running || thread.m_ReadyQueued ||
            thread.m_ReadyNext != nullptr || thread.m_WaitQueue != nullptr || thread.m_WaitNext != nullptr) return false;

        thread.m_WaitQueue = this;
        thread.m_WaitNext = nullptr;

        if (m_Tail != nullptr) m_Tail->m_WaitNext = &thread;
        else m_Head = &thread;

        m_Tail = &thread;
        m_Count++;
        return true;
    }

    Scheduling::Thread* WaitQueue::Dequeue() noexcept {
        if (m_Head == nullptr || m_Tail == nullptr || m_Count == 0) return nullptr;

        Scheduling::Thread* thread = m_Head;
        m_Head = thread->m_WaitNext;

        if (m_Head == nullptr) m_Tail = nullptr;

        thread->m_WaitNext = nullptr;
        thread->m_WaitQueue = nullptr;
        m_Count--;

        return thread;
    }

    void WaitQueue::RestoreFront(Scheduling::Thread& thread) noexcept {
        thread.m_WaitQueue = this;
        thread.m_WaitNext = m_Head;
        m_Head = &thread;

        if (m_Tail == nullptr) m_Tail = &thread;

        m_Count++;
    }

    SynchronizationError WaitQueue::BlockCurrent(Scheduling::Scheduler& scheduler) noexcept {
        Architecture::AMD64::ScopedInterruptDisable guard{};

        Scheduling::Thread* current = scheduler.CurrentThread();
        if (current == nullptr || !Enqueue(*current)) 
            return SynchronizationError::InvalidState;

        const Scheduling::SchedulerBlockError block_error = scheduler.BlockCurrent();
        if (block_error != Scheduling::SchedulerBlockError::Success) {
            /*
             * A failed block never transferred execution, so the current
             * thread must still be at the tail of this queue. Remove it
             * without disturbing earlier waiters.
             */
            Scheduling::Thread* previous{};
            Scheduling::Thread* cursor = m_Head;
            while (cursor != nullptr && cursor != current) {
                previous = cursor;
                cursor = cursor->m_WaitNext;
            }

            if (cursor != current) 
                return SynchronizationError::QueueCorrupted;

            if (previous != nullptr) previous->m_WaitNext = current->m_WaitNext;
            else m_Head = current->m_WaitNext;

            if (m_Tail == current) m_Tail = previous;

            current->m_WaitNext = nullptr;
            current->m_WaitQueue = nullptr;

            if (m_Count == 0) 
                return SynchronizationError::QueueCorrupted;

            m_Count--;
            return SynchronizationError::SchedulerFailure;
        }

        /*
         * Reaching here means this thread was woken and later scheduled again.
         * The wake path must have detached it from this queue first.
         */
        if (current->m_WaitQueue != nullptr || current->m_WaitNext != nullptr ||
            current->State() != Scheduling::ThreadState::Running)
            return SynchronizationError::QueueCorrupted;

        return SynchronizationError::Success;
    }

    SynchronizationError WaitQueue::WakeOneLocked(Scheduling::Scheduler& scheduler, Scheduling::Thread*& woken) noexcept {
        woken = nullptr;
        if (m_Count == 0) {
            if (m_Head != nullptr || m_Tail != nullptr) 
                return SynchronizationError::QueueCorrupted;
            return SynchronizationError::Success;
        }
        
        Scheduling::Thread* thread = Dequeue();
        if (thread == nullptr) 
            return SynchronizationError::QueueCorrupted;

        const Scheduling::SchedulerWakeError wake_error = scheduler.Wake(*thread);
        if (wake_error != Scheduling::SchedulerWakeError::Success) {
            RestoreFront(*thread);
            return SynchronizationError::SchedulerFailure;
        }

        woken = thread;
        return SynchronizationError::Success;
    }

    SynchronizationError WaitQueue::WakeOne(Scheduling::Scheduler& scheduler, Scheduling::Thread*& woken) noexcept {
        Architecture::AMD64::ScopedInterruptDisable guard{};
        return WakeOneLocked(scheduler, woken);
    }

    SynchronizationError WaitQueue::WakeAll(Scheduling::Scheduler& scheduler, Memory::Uint64& woken_count) noexcept {
        Architecture::AMD64::ScopedInterruptDisable guard{};

        woken_count = 0;
        while (m_Count != 0) {
            Scheduling::Thread* thread{};
            const SynchronizationError error = WakeOneLocked(scheduler, thread);
            if (error != SynchronizationError::Success) return error;
            if (thread == nullptr) return SynchronizationError::QueueCorrupted;
            woken_count++;
        }

        return SynchronizationError::Success;
    }

    bool WaitQueue::Validate() const noexcept {
        if ((m_Head == nullptr) != (m_Tail == nullptr)) return false;
        if ((m_Head == nullptr) != (m_Count == 0)) return false;

        Memory::Uint64 count{};
        const Scheduling::Thread* current = m_Head;
        while (current != nullptr) {
            ++count;

            if (!current->IsInitialized() || current->State() != Scheduling::ThreadState::Blocked ||
                current->m_WaitQueue != this || current->m_ReadyQueued || current->m_ReadyNext != nullptr)
                return false;

            if (count > m_Count) return false;
            if (current->m_WaitNext == nullptr && current != m_Tail)
                return false;

            current = current->m_WaitNext;
        }

        return count == m_Count;
    }
}
