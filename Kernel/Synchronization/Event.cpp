#include <Kernel/Synchronization/Event.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>

namespace Zos::Kernel::Synchronization {
    SynchronizationError Event::Initialize(EventResetMode mode, bool initially_signaled) noexcept {
        if (m_Initialized) return SynchronizationError::AlreadyInitialized;
        if (!InitializeObject(ObjectKind)) return SynchronizationError::InvalidState;

        m_Mode = mode;
        m_Signaled = initially_signaled;
        m_Initialized = true;
        return SynchronizationError::Success;
    }

    SynchronizationError Event::Wait(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        if (m_Signaled) {
            if (m_Mode == EventResetMode::Automatic) m_Signaled = false;
            return SynchronizationError::Success;
        }

        return m_Waiters.BlockCurrent(scheduler);
    }

    SynchronizationError Event::Signal(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        if (m_Mode == EventResetMode::Manual) {
            m_Signaled = true;

            Memory::Uint64 woken{};
            return m_Waiters.WakeAll(scheduler, woken);
        }

        Scheduling::Thread* woken{};
        const SynchronizationError wake_error = m_Waiters.WakeOne(scheduler, woken);
        if (wake_error != SynchronizationError::Success) return wake_error;

        m_Signaled = woken == nullptr;
        return SynchronizationError::Success;
    }

    SynchronizationError Event::Reset() noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;
        Architecture::AMD64::ScopedInterruptDisable guard{};
        m_Signaled = false;
        return SynchronizationError::Success;
    }

    bool Event::Validate() const noexcept {
        if (!m_Initialized || !IsObjectInitialized() || Type() != ObjectKind) return false;
        if (!m_Waiters.IsEmpty() && !m_Waiters.Validate()) return false;
        return true;
    }
}