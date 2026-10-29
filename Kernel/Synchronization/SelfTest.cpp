#include <Kernel/Synchronization/SelfTest.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Kernel.hpp>
#include <Kernel/Synchronization/Event.hpp>
#include <Kernel/Synchronization/Mutex.hpp>
#include <Kernel/Synchronization/Semaphore.hpp>
#include <Kernel/Synchronization/Timer.hpp>

namespace Zos::Kernel::Synchronization::SelfTest {
    void VerifyFoundation(KernelRuntime& runtime) noexcept {
        Event manual_event{};
        Event automatic_event{};
        Semaphore semaphore{};
        Mutex mutex{};
        Timer timer{};

        if (manual_event.Initialize(EventResetMode::Manual) != SynchronizationError::Success ||
            automatic_event.Initialize(EventResetMode::Automatic) != SynchronizationError::Success ||
            semaphore.Initialize(2, 3) != SynchronizationError::Success ||
            mutex.Initialize() != SynchronizationError::Success ||
            timer.Initialize() != SynchronizationError::Success)
            Diagnostics::Fatal("Sync", "synchronization object initialization failed");

        if (manual_event.Signal(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success ||
            !manual_event.IsSignaled() || manual_event.Reset() != SynchronizationError::Success ||
            manual_event.IsSignaled())
            Diagnostics::Fatal("Event", "manual-reset event semantics failed");

        if (automatic_event.Signal(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success || !automatic_event.IsSignaled() || 
            automatic_event.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success || automatic_event.IsSignaled())
            Diagnostics::Fatal("Event", "automatic-reset event semantics failed");

        if (semaphore.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success ||
            semaphore.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success ||
            semaphore.Count() != 0 ||
            semaphore.Release(runtime.Scheduling.BootstrapScheduler, 2) != SynchronizationError::Success ||
            semaphore.Count() != 2)
            Diagnostics::Fatal("Semaphore", "semaphore count semantics failed");

        if (mutex.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success ||
            mutex.Owner() != &runtime.Scheduling.BootstrapThread ||
            mutex.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::AlreadyOwned ||
            mutex.Release(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success ||
            mutex.Owner() != nullptr)
            Diagnostics::Fatal("Mutex", "mutex ownership semantics failed");

        if (timer.Arm(runtime.Time.Clock, 1'000'000ULL) != SynchronizationError::Success ||
            !timer.IsArmed() || timer.IsSignaled())
            Diagnostics::Fatal("Timer", "waitable timer arm semantics failed");

        {
            Architecture::AMD64::ScopedInterruptDisable guard{};
            Timer::Poll(timer.DeadlineNanoseconds(), runtime.Scheduling.BootstrapScheduler);
        }

        if (timer.IsArmed() || !timer.IsSignaled() ||
            timer.Wait(runtime.Scheduling.BootstrapScheduler) != SynchronizationError::Success)
            Diagnostics::Fatal("Timer", "waitable timer expiration semantics failed");

        if (!manual_event.Validate() || !automatic_event.Validate() || !semaphore.Validate() ||
            !mutex.Validate() || !timer.Validate())
            Diagnostics::Fatal("Sync", "synchronization object validation failed");

        Diagnostics::Write("[zOS/Event] Signal and reset semantics verified.\n");
        Diagnostics::Write("[zOS/Semaphore] Count semantics verified.\n");
        Diagnostics::Write("[zOS/Mutex] Ownership semantics verified.\n");
        Diagnostics::Write("[zOS/Timer] Waitable timer state verified.\n");
        Diagnostics::Write("[zOS/Sync] Waitable object foundation verified.\n");
    }
}
