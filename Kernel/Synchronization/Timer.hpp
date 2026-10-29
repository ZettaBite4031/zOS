#pragma once

#include <Kernel/Objects/Object.hpp>
#include <Kernel/Synchronization/Rights.hpp>
#include <Kernel/Synchronization/WaitQueue.hpp>

namespace Zos::Kernel::Scheduling {
    class Scheduler;
}

namespace Zos::Kernel::Time {
    class MonotonicClock;
}

namespace Zos::Kernel::Synchronization {
    /*
     * One-shot, manual-reset waitable timer.
     *
     * Arm() clears the signaled state and publishes a deadline. Expiration
     * leaves the object signaled until it is armed again or cancelled.
     */
    class Timer final : public Objects::Object {
    public:
        inline static constexpr Objects::ObjectType ObjectKind{ Objects::ObjectType::Timer };

        constexpr Timer() noexcept = default;

        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

        [[nodiscard]] SynchronizationError Initialize() noexcept;

        [[nodiscard]] SynchronizationError Wait(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] SynchronizationError Arm(const Time::MonotonicClock& clock, Memory::Uint64 duration_nanoseconds) noexcept;

        [[nodiscard]] SynchronizationError Cancel() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] bool IsArmed() const noexcept { return m_Armed; }
        [[nodiscard]] bool IsSignaled() const noexcept { return m_Signaled; }
        [[nodiscard]] Memory::Uint64 DeadlineNanoseconds() const noexcept { return m_DeadlineNanoseconds; }
        [[nodiscard]] Memory::Uint64 WaiterCount() const noexcept { return m_Waiters.Count(); }

        [[nodiscard]] bool Validate() const noexcept;

        static void Poll(Memory::Uint64 now_nanoseconds, Scheduling::Scheduler& scheduler) noexcept;

    private:
        [[nodiscard]] bool RemoveFromArmedList() noexcept;
        void InsertIntoArmedList() noexcept;

        WaitQueue m_Waiters{};

        Memory::Uint64 m_DeadlineNanoseconds{};
        Timer* m_ArmedNext{};

        bool m_Armed{};
        bool m_Signaled{};
        bool m_Initialized{};

        static Timer* s_ArmedHead;
    };
}
