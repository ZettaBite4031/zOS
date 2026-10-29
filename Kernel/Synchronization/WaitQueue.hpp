#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Scheduling {
    class Scheduler;
    class Thread;
}

namespace Zos::Kernel::Synchronization {
    enum class SynchronizationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        NotInitialized,
        InvalidDependency,
        InvalidState,
        AlreadyOwned,
        NotOwner,
        LimitExceeded,
        SchedulerFailure,
        QueueCorrupted,
    };

    /*
     * Intrusive FIFO of blocked threads.
     *
     * A Thread can belong to exactly one WaitQueue and cannot simultaneously
     * reside in the scheduler Ready queue. All queue mutations are performed 
     * with local interrupts masked; this is not an SMP lock.
     */
    class WaitQueue final {
    public:
        constexpr WaitQueue() noexcept = default;

        WaitQueue(const WaitQueue&) = delete;
        WaitQueue& operator=(const WaitQueue&) = delete;

        [[nodiscard]] SynchronizationError BlockCurrent(Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] SynchronizationError WakeOne(Scheduling::Scheduler& scheduler, Scheduling::Thread*& woken) noexcept;

        [[nodiscard]] SynchronizationError WakeAll(Scheduling::Scheduler& scheduler, Memory::Uint64& woken_count) noexcept;

        [[nodiscard]] bool IsEmpty() const noexcept { return m_Count == 0; }
        [[nodiscard]] Memory::Uint64 Count() const noexcept { return m_Count; }
        
        [[nodiscard]] bool Validate() const noexcept;

    private:
        [[nodiscard]] bool Enqueue(Scheduling::Thread& thread) noexcept;

        [[nodiscard]] Scheduling::Thread* Dequeue() noexcept;

        void RestoreFront(Scheduling::Thread& thread) noexcept;

        [[nodiscard]] SynchronizationError WakeOneLocked(Scheduling::Scheduler& scheduler, Scheduling::Thread*& woken) noexcept;

        Scheduling::Thread* m_Head{};
        Scheduling::Thread* m_Tail{};
        Memory::Uint64 m_Count{};
    };
}