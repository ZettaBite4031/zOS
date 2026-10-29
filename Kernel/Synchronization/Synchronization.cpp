#include <Kernel/Synchronization/Semaphore.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>

namespace Zos::Kernel::Synchronization {
    SynchronizationError Semaphore::Initialize(Memory::Uint64 initial_count, Memory::Uint64 maximum_count) noexcept {
        if (m_Initialized) return SynchronizationError::AlreadyInitialized;
        if (maximum_count == 0 || initial_count > maximum_count)
            return SynchronizationError::InvalidState;

        if (!InitializeObject(ObjectKind))
            return SynchronizationError::InvalidState;

        m_Count = initial_count;
        m_MaximumCount = maximum_count;
        m_Initialized = true;
        return SynchronizationError::Success;
    }

    SynchronizationError Semaphore::Wait(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        if (m_Count != 0) {
            --m_Count;
            return SynchronizationError::Success;
        }

        return m_Waiters.BlockCurrent(scheduler);
    }

    SynchronizationError Semaphore::Release(Scheduling::Scheduler& scheduler, Memory::Uint64 count) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;
        if (count == 0) return SynchronizationError::InvalidState;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        const Memory::Uint64 waiter_count = m_Waiters.Count();
        const Memory::Uint64 available_count = m_MaximumCount - m_Count;
        if (count > waiter_count && count - waiter_count > available_count)
            return SynchronizationError::LimitExceeded;

        Memory::Uint64 remaining = count;
        while (remaining != 0 && !m_Waiters.IsEmpty()) {
            Scheduling::Thread* woken{};

            const SynchronizationError error = m_Waiters.WakeOne(scheduler, woken);
            if (error != SynchronizationError::Success) return error;
            if (woken == nullptr) return SynchronizationError::QueueCorrupted;

            --remaining;
        }

        m_Count += remaining;
        return SynchronizationError::Success;
    }

    bool Semaphore::Validate() const noexcept {
        if (!m_Initialized || !IsObjectInitialized() || Type() != ObjectKind ||
            m_MaximumCount == 0 || m_Count > m_MaximumCount) return false;
        if (!m_Waiters.IsEmpty() && !m_Waiters.Validate()) return false;
        return true;
    }
}
