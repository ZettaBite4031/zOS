#include <Kernel/Tests/Integration/WaitableObjects.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Kernel.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Processes/UserProcessLauncher.hpp>
#include <Kernel/Synchronization/Event.hpp>
#include <Kernel/Synchronization/Mutex.hpp>
#include <Kernel/Synchronization/Semaphore.hpp>
#include <Kernel/Synchronization/Timer.hpp>

#include <User/WaitTest/Contract.hpp>

namespace Zos::Kernel::Tests::Integration::WaitableObjects {
    namespace {
        using namespace Memory;
        namespace Contract = ::Zos::User::WaitTest;

        inline constexpr Uint64 KernelStackPages{ 8 };
        inline constexpr Uint64 TimeoutNanoseconds{
            2'000'000'000ULL
        };

        inline constexpr Processes::ProcessId EventWaiterPid{ 7 };
        inline constexpr Processes::ProcessId EventSignalerPid{ 8 };
        inline constexpr Processes::ProcessId SemaphoreWaiterPid{ 9 };
        inline constexpr Processes::ProcessId SemaphoreReleaserPid{ 10 };
        inline constexpr Processes::ProcessId MutexOwnerPid{ 11 };
        inline constexpr Processes::ProcessId MutexWaiterPid{ 12 };
        inline constexpr Processes::ProcessId TimerWaiterPid{ 13 };

        inline constexpr Scheduling::ThreadId EventWaiterTid{ 8 };
        inline constexpr Scheduling::ThreadId EventSignalerTid{ 9 };
        inline constexpr Scheduling::ThreadId SemaphoreWaiterTid{ 10 };
        inline constexpr Scheduling::ThreadId SemaphoreReleaserTid{ 11 };
        inline constexpr Scheduling::ThreadId MutexOwnerTid{ 12 };
        inline constexpr Scheduling::ThreadId MutexWaiterTid{ 13 };
        inline constexpr Scheduling::ThreadId TimerWaiterTid{ 14 };

        struct Fixture final {
            Processes::UserProcessInstance EventWaiter{};
            Processes::UserProcessInstance EventSignaler{};
            Processes::UserProcessInstance SemaphoreWaiter{};
            Processes::UserProcessInstance SemaphoreReleaser{};
            Processes::UserProcessInstance MutexOwner{};
            Processes::UserProcessInstance MutexWaiter{};
            Processes::UserProcessInstance TimerWaiter{};

            Synchronization::Event Event{};
            Synchronization::Semaphore Semaphore{};
            Synchronization::Mutex Mutex{};
            Synchronization::Event MutexGate{};
            Synchronization::Timer Timer{};
        };

        constinit Fixture g_Fixture{};

        [[nodiscard]] volatile Contract::State* TestState(Processes::UserProcessInstance& instance) noexcept {
            const auto* space = instance.ProcessObject().AddressSpace();
            if (space == nullptr || !instance.Image().IsLoaded())
                Diagnostics::Fatal("Sync", "wait-test process is not prepared");

            const auto translation = space->Translate(VirtualAddress(Contract::StateAddress));
            if (!translation.Mapped ||
                !HasAccess(translation.Options.Access, PageAccess::Read) ||
                !HasAccess(translation.Options.Access, PageAccess::Write) ||
                !HasAccess(translation.Options.Access, PageAccess::User) ||
                HasAccess(translation.Options.Access, PageAccess::Execute))
                Diagnostics::Fatal("Sync", "wait-test state mapping is invalid");

            const VirtualAddress direct = Layout::DirectMapAddress(translation.Physical);
            if (direct.IsNull())
                Diagnostics::Fatal("Sync", "wait-test state has no direct-map alias");

            return reinterpret_cast<volatile Contract::State*>(direct.Value());
        }

        void PrepareProcess(KernelRuntime& runtime, Processes::UserProcessInstance& instance, Processes::ProcessId pid, Scheduling::ThreadId tid, Contract::Role role,
            Objects::Object& primary_object, Objects::AccessMask primary_rights, Objects::Object* secondary_object = nullptr, Objects::AccessMask secondary_rights = Objects::NoAccess) noexcept {
            const Executables::UserExecutableLoadOptions options{
                .StackBase = VirtualAddress(Contract::StackBaseAddress),
                .StackPageCount = Contract::StackPageCount,
            };

            const auto prepare_error = instance.Prepare(pid, tid, Executables::WaitTestImage(), options, KernelStackPages, runtime.Memory.Physical, runtime.Memory.KernelAddresses, runtime.Memory.KernelPageMap);
            if (prepare_error != Processes::UserProcessLaunchError::Success)
                Diagnostics::Fatal("Sync", Processes::UserProcessInstance::Describe(prepare_error));

            Objects::Handle primary_handle{};
            if (instance.ProcessObject().Handles().Open(primary_object, primary_rights,primary_handle) != Objects::HandleError::Success)
                Diagnostics::Fatal("Sync", "failed to grant primary synchronization handle");

            Objects::Handle secondary_handle{};
            if (secondary_object != nullptr) {
                if (secondary_rights == Objects::NoAccess ||
                    instance.ProcessObject().Handles().Open(*secondary_object, secondary_rights, secondary_handle) != Objects::HandleError::Success)
                    Diagnostics::Fatal("Sync", "failed to grant secondary synchronization handle");
            }

            volatile Contract::State* state = TestState(instance);
            state->RoleId = static_cast<Contract::Uint64>(role);
            state->PrimaryHandle = primary_handle.Value();
            state->SecondaryHandle = secondary_handle.Value();
        }

        void StartProcess(KernelRuntime& runtime, Processes::UserProcessInstance& instance) noexcept {
            const auto start_error = instance.Start(runtime.Scheduling.BootstrapScheduler);
            if (start_error != Processes::UserProcessLaunchError::Success)
                Diagnostics::Fatal("Sync", Processes::UserProcessInstance::Describe(start_error));
        }

        void WaitUntilBlocked(KernelRuntime& runtime, Processes::UserProcessInstance& instance, volatile Contract::State& state) noexcept {
            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (state.Entered != 1 || instance.MainThread().State() != Scheduling::ThreadState::Blocked) {
                if (runtime.Time.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                    Diagnostics::Fatal("Sync", "wait-test process did not enter the blocked state");

                if (state.Completed != 0)
                    Diagnostics::Fatal("Sync", "wait-test process completed without blocking");

                __asm__ volatile("pause");
            }
        }

        void VerifyBlockedForTicks(KernelRuntime& runtime, Processes::UserProcessInstance& instance, volatile Contract::State& state, Uint64 tick_count = 5) noexcept {
            const Uint64 initial_ticks = runtime.Time.Timer.InterruptCount();
            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (runtime.Time.Timer.InterruptCount() < initial_ticks + tick_count) {
                if (runtime.Time.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                    Diagnostics::Fatal("Sync", "timer ticks stopped while a thread was blocked");

                if (instance.MainThread().State() != Scheduling::ThreadState::Blocked || state.Completed != 0)
                    Diagnostics::Fatal("Sync", "blocked thread made progress without a wake");

                __asm__ volatile("pause");
            }
        }

        void WaitForTermination(KernelRuntime& runtime, Processes::UserProcessInstance& first, Processes::UserProcessInstance* second = nullptr) noexcept {
            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            for (;;) {
                const bool first_done = first.ProcessObject().State() == Processes::ProcessState::Terminated;
                const bool second_done = second == nullptr || second->ProcessObject().State() == Processes::ProcessState::Terminated;
                if (first_done && second_done) return;

                if (runtime.Time.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                    Diagnostics::Fatal("Sync", "wait-test process termination timed out");

                __asm__ volatile("pause");
            }
        }

        void VerifyExited(Processes::UserProcessInstance& instance, volatile Contract::State& state, Processes::ProcessId expected_pid, bool acquired, bool released) noexcept {
            const auto& exit = instance.ProcessObject().ExitRecord();
            if (state.ProcessId != expected_pid || state.Completed != 1 ||
                state.Marker != Contract::CompletionMarker ||
                state.Acquired != static_cast<Contract::Uint64>(acquired) ||
                state.Released != static_cast<Contract::Uint64>(released) ||
                !exit.Valid || exit.Status != static_cast<Memory::Uint32>(Contract::TestExitStatus) ||
                instance.ProcessObject().FaultRecord().Valid ||
                instance.MainThread().State() != Scheduling::ThreadState::Terminated ||
                !instance.ProcessObject().Handles().Validate() ||
                instance.ProcessObject().Handles().ActiveCount() != 0)
                Diagnostics::Fatal("Sync", "wait-test process outcome is invalid");
        }

        void VerifyEvent(KernelRuntime& runtime) noexcept {
            if (g_Fixture.Event.Initialize(Synchronization::EventResetMode::Manual) != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Event", "failed to initialize integration event");

            PrepareProcess(runtime, g_Fixture.EventWaiter, EventWaiterPid, EventWaiterTid, Contract::Role::EventWaiter, g_Fixture.Event, Synchronization::WaitRight);
            PrepareProcess(runtime, g_Fixture.EventSignaler, EventSignalerPid, EventSignalerTid, Contract::Role::EventSignaler, g_Fixture.Event, Synchronization::EventSignalRight);

            volatile Contract::State* waiter = TestState(g_Fixture.EventWaiter);
            volatile Contract::State* signaler = TestState(g_Fixture.EventSignaler);

            StartProcess(runtime, g_Fixture.EventWaiter);

            WaitUntilBlocked(runtime, g_Fixture.EventWaiter, *waiter);

            VerifyBlockedForTicks(runtime, g_Fixture.EventWaiter, *waiter);

            if (g_Fixture.Event.WaiterCount() != 1 || !g_Fixture.Event.Validate())
                Diagnostics::Fatal("Event", "event wait queue is invalid");

            StartProcess(runtime, g_Fixture.EventSignaler);

            WaitForTermination(runtime, g_Fixture.EventWaiter, &g_Fixture.EventSignaler);

            VerifyExited(g_Fixture.EventWaiter, *waiter, EventWaiterPid, true, false);
            VerifyExited(g_Fixture.EventSignaler, *signaler, EventSignalerPid, false, true);

            if (g_Fixture.Event.WaiterCount() != 0 || !g_Fixture.Event.IsSignaled())
                Diagnostics::Fatal("Event", "event did not preserve manual-reset signal state");

            if (g_Fixture.Event.Reset() != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Event", "failed to reset integration event");
        }

        void VerifySemaphore(KernelRuntime& runtime) noexcept {
            if (g_Fixture.Semaphore.Initialize(0, 1) != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Semaphore", "failed to initialize integration semaphore");

            PrepareProcess(runtime, g_Fixture.SemaphoreWaiter, SemaphoreWaiterPid, SemaphoreWaiterTid, Contract::Role::SemaphoreWaiter, g_Fixture.Semaphore, Synchronization::WaitRight);
            PrepareProcess(runtime, g_Fixture.SemaphoreReleaser, SemaphoreReleaserPid, SemaphoreReleaserTid, Contract::Role::SemaphoreReleaser, g_Fixture.Semaphore, Synchronization::SemaphoreReleaseRight);

            volatile Contract::State* waiter = TestState(g_Fixture.SemaphoreWaiter);
            volatile Contract::State* releaser = TestState(g_Fixture.SemaphoreReleaser);

            StartProcess(runtime, g_Fixture.SemaphoreWaiter);

            WaitUntilBlocked(runtime, g_Fixture.SemaphoreWaiter, *waiter);

            VerifyBlockedForTicks(runtime, g_Fixture.SemaphoreWaiter, *waiter);

            StartProcess(runtime, g_Fixture.SemaphoreReleaser);

            WaitForTermination(runtime, g_Fixture.SemaphoreWaiter, &g_Fixture.SemaphoreReleaser);

            VerifyExited(g_Fixture.SemaphoreWaiter, *waiter, SemaphoreWaiterPid, true, false);
            VerifyExited(g_Fixture.SemaphoreReleaser, *releaser, SemaphoreReleaserPid, false, true);

            if (g_Fixture.Semaphore.Count() != 0 || g_Fixture.Semaphore.WaiterCount() != 0 ||
                !g_Fixture.Semaphore.Validate())
                Diagnostics::Fatal("Semaphore", "semaphore token handoff state is invalid");
        }

        void VerifyMutex(KernelRuntime& runtime) noexcept {
            if (g_Fixture.Mutex.Initialize() != Synchronization::SynchronizationError::Success ||
                g_Fixture.MutexGate.Initialize(Synchronization::EventResetMode::Manual) != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Mutex", "failed to initialize mutex integration objects");

            PrepareProcess(runtime, g_Fixture.MutexOwner, MutexOwnerPid, MutexOwnerTid, Contract::Role::MutexOwner, g_Fixture.Mutex, Synchronization::WaitRight | Synchronization::MutexReleaseRight, &g_Fixture.MutexGate, Synchronization::WaitRight);
            PrepareProcess(runtime, g_Fixture.MutexWaiter, MutexWaiterPid, MutexWaiterTid, Contract::Role::MutexWaiter, g_Fixture.Mutex, Synchronization::WaitRight | Synchronization::MutexReleaseRight);

            volatile Contract::State* owner = TestState(g_Fixture.MutexOwner);
            volatile Contract::State* waiter = TestState(g_Fixture.MutexWaiter);

            StartProcess(runtime, g_Fixture.MutexOwner);

            WaitUntilBlocked(runtime, g_Fixture.MutexOwner, *owner);

            if (owner->Acquired != 1 || g_Fixture.Mutex.Owner() != &g_Fixture.MutexOwner.MainThread() ||
                g_Fixture.MutexGate.WaiterCount() != 1)
                Diagnostics::Fatal("Mutex", "mutex owner did not block while retaining ownership");

            StartProcess(runtime, g_Fixture.MutexWaiter);

            WaitUntilBlocked(runtime, g_Fixture.MutexWaiter, *waiter);

            if (runtime.Scheduling.BootstrapScheduler.BlockedCount() < 2 ||
                g_Fixture.Mutex.WaiterCount() != 1)
                Diagnostics::Fatal("Mutex", "mutex waiter did not enter the blocked state");

            VerifyBlockedForTicks(runtime, g_Fixture.MutexWaiter, *waiter);

            if (g_Fixture.MutexGate.Signal(runtime.Scheduling.BootstrapScheduler) != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Mutex", "failed to release mutex owner gate");

            WaitForTermination(runtime, g_Fixture.MutexOwner, &g_Fixture.MutexWaiter);

            VerifyExited(g_Fixture.MutexOwner, *owner, MutexOwnerPid, true, true);
            VerifyExited(g_Fixture.MutexWaiter, *waiter, MutexWaiterPid, true, true);

            if (g_Fixture.Mutex.Owner() != nullptr || g_Fixture.Mutex.WaiterCount() != 0 || !g_Fixture.Mutex.Validate())
                Diagnostics::Fatal("Mutex", "mutex ownership handoff did not complete");
        }

        void VerifyTimer(KernelRuntime& runtime) noexcept {
            if (g_Fixture.Timer.Initialize() != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Timer", "failed to initialize integration timer");

            PrepareProcess(runtime, g_Fixture.TimerWaiter, TimerWaiterPid, TimerWaiterTid, Contract::Role::TimerWaiter, g_Fixture.Timer, Synchronization::WaitRight);

            volatile Contract::State* waiter = TestState(g_Fixture.TimerWaiter);

            StartProcess(runtime, g_Fixture.TimerWaiter);

            WaitUntilBlocked(runtime, g_Fixture.TimerWaiter, *waiter);

            VerifyBlockedForTicks(runtime, g_Fixture.TimerWaiter, *waiter, 3);

            if (g_Fixture.Timer.Arm(runtime.Time.Clock, 50'000'000ULL) != Synchronization::SynchronizationError::Success)
                Diagnostics::Fatal("Timer", "failed to arm integration waitable timer");

            const Uint64 deadline = g_Fixture.Timer.DeadlineNanoseconds();

            WaitForTermination(runtime, g_Fixture.TimerWaiter);

            VerifyExited(g_Fixture.TimerWaiter, *waiter, TimerWaiterPid, true, false);

            if (runtime.Time.Clock.Nanoseconds() < deadline || g_Fixture.Timer.IsArmed() || !g_Fixture.Timer.IsSignaled() ||
                g_Fixture.Timer.WaiterCount() != 0 || !g_Fixture.Timer.Validate())
                Diagnostics::Fatal("Timer", "waitable timer expiration state is invalid");
        }
    }

    void Run(KernelRuntime& runtime) noexcept {
        VerifyEvent(runtime);
        Diagnostics::Write("[zOS/Event] Blocking wake verified.\n");

        VerifySemaphore(runtime);
        Diagnostics::Write("[zOS/Semaphore] Blocking token handoff verified.\n");

        VerifyMutex(runtime);
        Diagnostics::Write("[zOS/Mutex] Blocking ownership transfer verified.\n");

        VerifyTimer(runtime);
        Diagnostics::Write("[zOS/Timer] Waitable timer expiration verified.\n");

        if (runtime.Scheduling.BootstrapScheduler.BlockedCount() != 0)
            Diagnostics::Fatal("Scheduler", "blocked-thread accounting did not return to zero");

        Diagnostics::Write("[zOS/Wait] Wait queue invariants verified.\n");
        Diagnostics::Write("[zOS/Scheduler] Blocked threads removed from runnable rotation.\n");
        Diagnostics::Write("[zOS/Scheduler] Signaled threads returned to ready state.\n");
        Diagnostics::Write("[zOS/User] Native wait and wake round trip verified.\n");
        Diagnostics::Write("[zOS/Sync] Waitable synchronization model verified.\n");
    }
}
