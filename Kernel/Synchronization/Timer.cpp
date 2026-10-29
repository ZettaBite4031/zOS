#include <Kernel/Synchronization/Timer.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Time/Clock.hpp>

namespace Zos::Kernel::Synchronization {
    Timer* Timer::s_ArmedHead{};

    SynchronizationError Timer::Initialize() noexcept {
        if (m_Initialized) return SynchronizationError::AlreadyInitialized;
        if (!InitializeObject(ObjectKind)) return SynchronizationError::InvalidState;

        m_Initialized = true;
        return SynchronizationError::Success;
    }

    SynchronizationError Timer::Wait(Scheduling::Scheduler& scheduler) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        if (m_Signaled) return SynchronizationError::Success;

        return m_Waiters.BlockCurrent(scheduler);
    }

    SynchronizationError Timer::Arm(const Time::MonotonicClock& clock, Memory::Uint64 duration_nanoseconds) noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;
        if (!clock.IsInitialized() || duration_nanoseconds == 0)
            return SynchronizationError::InvalidDependency;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        const Memory::Uint64 now = clock.Nanoseconds();
        if (duration_nanoseconds > Memory::MaximumValue - now)
            return SynchronizationError::LimitExceeded;

        if (m_Armed && !RemoveFromArmedList())
            return SynchronizationError::QueueCorrupted;

        m_DeadlineNanoseconds = now + duration_nanoseconds;

        m_Signaled = false;
        m_Armed = true;

        InsertIntoArmedList();
        return SynchronizationError::Success;
    }

    SynchronizationError Timer::Cancel() noexcept {
        if (!m_Initialized) return SynchronizationError::NotInitialized;

        Architecture::AMD64::ScopedInterruptDisable guard{};

        if (m_Armed && !RemoveFromArmedList())
            return SynchronizationError::QueueCorrupted;

        m_Armed = false;
        m_Signaled = false;
        m_DeadlineNanoseconds = 0;
        m_ArmedNext = nullptr;

        return SynchronizationError::Success;
    }

    bool Timer::RemoveFromArmedList() noexcept {
        Timer** link = &s_ArmedHead;

        while (*link != nullptr) {
            if (*link == this) {
                *link = m_ArmedNext;
                m_ArmedNext = nullptr;
                return true;
            }

            link = &(*link)->m_ArmedNext;
        }

        return false;
    }

    void Timer::InsertIntoArmedList() noexcept {
        m_ArmedNext = s_ArmedHead;
        s_ArmedHead = this;
    }

    void Timer::Poll(Memory::Uint64 now_nanoseconds, Scheduling::Scheduler& scheduler) noexcept {
        Architecture::AMD64::ScopedInterruptDisable guard{};

        Timer** link = &s_ArmedHead;

        while (*link != nullptr) {
            Timer* timer = *link;

            if (!timer->m_Initialized || !timer->m_Armed || timer->m_DeadlineNanoseconds == 0)
                Diagnostics::Fatal("Timer", "waitable timer armed-list invariant failed");

            if (timer->m_DeadlineNanoseconds > now_nanoseconds) {
                link = &timer->m_ArmedNext;
                continue;
            }

            *link = timer->m_ArmedNext;

            timer->m_ArmedNext = nullptr;
            timer->m_Armed = false;
            timer->m_Signaled = true;

            Memory::Uint64 woken{};

            const SynchronizationError wake_error = timer->m_Waiters.WakeAll(scheduler, woken);
            if (wake_error != SynchronizationError::Success)
                Diagnostics::Fatal("Timer", "waitable timer failed to wake its waiters");
        }
    }

    bool Timer::Validate() const noexcept {
        if (!m_Initialized || !IsObjectInitialized() || Type() != ObjectKind)
            return false;
        if (m_Armed && (m_DeadlineNanoseconds == 0 || m_Signaled))
            return false;
        if (!m_Armed && m_ArmedNext != nullptr)
            return false;
        if (!m_Waiters.IsEmpty() && !m_Waiters.Validate())
            return false;
        return true;
    }
}
