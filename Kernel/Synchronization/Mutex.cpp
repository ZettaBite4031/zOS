#include <Kernel/Synchronization/Mutex.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Synchronization {
    SynchronizationError Mutex::Initialize() noexcept {
        if (m_Initialized) return SynchronizationError::AlreadyInitialized;
        if (!InitializeObject(ObjectKind)) return SynchronizationError::InvalidState;

        m_Owner = nullptr;
        m_Initialized = true;
        return SynchronizationError::Success;
    }

    SynchronizationError Mutex::Wait(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        Scheduling::Thread* current = scheduler.CurrentThread();
        if (current == nullptr || current->State() != Scheduling::ThreadState::Running)
            return SynchronizationError::InvalidState;

        if (m_Owner == nullptr) {
            m_Owner = current;
            return SynchronizationError::Success;
        }

        if (m_Owner == current)
            return SynchronizationError::AlreadyOwned;

        const SynchronizationError wait_error = m_Waiters.BlockCurrent(scheduler);
        if (wait_error != SynchronizationError::Success) return wait_error;

        return m_Owner == current ? SynchronizationError::Success : SynchronizationError::QueueCorrupted;
    }

    SynchronizationError Mutex::Release(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        Scheduling::Thread* current = scheduler.CurrentThread();
        if (current == nullptr || m_Owner != current)
            return SynchronizationError::NotOwner;

        if (m_Waiters.IsEmpty()) {
            m_Owner = nullptr;
            return SynchronizationError::Success;
        }

        Scheduling::Thread* woken{};
        const SynchronizationError wake_error = m_Waiters.WakeOne(scheduler, woken);
        if (wake_error != SynchronizationError::Success) return wake_error;
        if (woken == nullptr) return SynchronizationError::QueueCorrupted;

        m_Owner = woken;
        return SynchronizationError::Success;
    }

    bool Mutex::Validate() const noexcept {
        if (!m_Initialized || !IsObjectInitialized() || Type() != ObjectKind)
            return false;
        if (!m_Waiters.IsEmpty() && !m_Waiters.Validate())
            return false;
        if (m_Owner != nullptr && (!m_Owner->IsInitialized() || m_Owner->State() == Scheduling::ThreadState::Terminated))
            return false;
        return true;
    }
}
