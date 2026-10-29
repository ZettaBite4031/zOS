#pragma once

#include <Kernel/Objects/Object.hpp>
#include <Kernel/Synchronization/Rights.hpp>
#include <Kernel/Synchronization/WaitQueue.hpp>

namespace Zos::Kernel::Scheduling {
    class Scheduler;
    class Thread;
}

namespace Zos::Kernel::Synchronization {
    class Mutex final : public Objects::Object {
    public:
        inline static constexpr Objects::ObjectType ObjectKind{ Objects::ObjectType::Mutex };

        constexpr Mutex() noexcept = default;

        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;

        [[nodiscard]] SynchronizationError Initialize() noexcept;

        [[nodiscard]] SynchronizationError Wait(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] SynchronizationError Release(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] const Scheduling::Thread* Owner() const noexcept { return m_Owner; }
        [[nodiscard]] Memory::Uint64 WaiterCount() const noexcept { return m_Waiters.Count(); }

        [[nodiscard]] bool Validate() const noexcept;

    private:
        WaitQueue m_Waiters{};
        Scheduling::Thread* m_Owner{};
        bool m_Initialized{};
    };
}
