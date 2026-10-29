#pragma once

#include <Kernel/Objects/Object.hpp>
#include <Kernel/Synchronization/Rights.hpp>
#include <Kernel/Synchronization/WaitQueue.hpp>

namespace Zos::Kernel::Scheduling {
    class Scheduler;
}

namespace Zos::Kernel::Synchronization {
    class Semaphore final : public Objects::Object {
    public:
        inline static constexpr Objects::ObjectType ObjectKind{ Objects::ObjectType::Semaphore };

        constexpr Semaphore() noexcept = default;

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        [[nodiscard]] SynchronizationError Initialize(Memory::Uint64 initial_count, Memory::Uint64 maximum_count) noexcept;

        [[nodiscard]] SynchronizationError Wait(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] SynchronizationError Release(Scheduling::Scheduler& scheduler, Memory::Uint64 count = 1) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint64 Count() const noexcept { return m_Count; }
        [[nodiscard]] Memory::Uint64 MaximumCount() const noexcept { return m_MaximumCount; }
        [[nodiscard]] Memory::Uint64 WaiterCount() const noexcept { return m_Waiters.Count(); }

        [[nodiscard]] bool Validate() const noexcept;

    private:
        WaitQueue m_Waiters{};
        Memory::Uint64 m_Count{};
        Memory::Uint64 m_MaximumCount{};
        bool m_Initialized{};
    };
}
