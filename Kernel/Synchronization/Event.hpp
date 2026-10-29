#pragma once

#include <Kernel/Objects/Object.hpp>
#include <Kernel/Synchronization/Rights.hpp>
#include <Kernel/Synchronization/WaitQueue.hpp>

namespace Zos::Kernel::Scheduling {
    class Scheduler;
}

namespace Zos::Kernel::Synchronization {
    enum class EventResetMode : Memory::Uint32 {
        Manual,
        Automatic,
    };

    class Event final : public Objects::Object {
    public:
        inline static constexpr Objects::ObjectType ObjectKind{ Objects::ObjectType::Event };

        constexpr Event() noexcept = default;

        Event(const Event&) = delete;
        Event& operator=(const Event&) = delete;

        [[nodiscard]] SynchronizationError Initialize(EventResetMode mode, bool initially_signaled = false) noexcept;

        [[nodiscard]] SynchronizationError Wait(Scheduling::Scheduler& scheduler) noexcept;
        [[nodiscard]] SynchronizationError Signal(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] SynchronizationError Reset() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] bool IsSignaled() const noexcept { return m_Signaled; }
        [[nodiscard]] EventResetMode ResetMode() const noexcept { return m_Mode; }
        [[nodiscard]] Memory::Uint64 WaiterCount() const noexcept { return m_Waiters.Count(); }

        [[nodiscard]] bool Validate() const noexcept;

    private:
        WaitQueue m_Waiters{};
        EventResetMode m_Mode{ EventResetMode::Manual };
        bool m_Signaled{};
        bool m_Initialized{};
    };
}
