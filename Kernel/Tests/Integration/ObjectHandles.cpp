#include <Kernel/Tests/Integration/ObjectHandles.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Kernel.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Objects/Object.hpp>
#include <Kernel/Processes/UserProcessLauncher.hpp>

#include <User/ObjectTest/Contract.hpp>

namespace Zos::Kernel::Tests::Integration::ObjectHandles {
    namespace {
        using namespace Memory;
        using namespace ::Zos::User::ObjectTest;

        inline constexpr Processes::ProcessId FirstPid{ 5 };
        inline constexpr Processes::ProcessId SecondPid{ 6 };

        inline constexpr Scheduling::ThreadId FirstTid{ 6 };
        inline constexpr Scheduling::ThreadId SecondTid{ 7 };

        inline constexpr Uint64 KernelStackPages{ 8 };
        inline constexpr Uint64 TimeoutNanoseconds{
            2'000'000'000ULL
        };

        struct Fixture final {
            Processes::UserProcessInstance First{};
            Processes::UserProcessInstance Second{};
        };

        constinit Fixture g_Fixture{};

        void LaunchOne(KernelRuntime& runtime, Processes::UserProcessInstance& instance, Processes::ProcessId pid, Scheduling::ThreadId tid) noexcept {
            const Executables::UserExecutableLoadOptions options{
                .StackBase = VirtualAddress(StackBaseAddress),
                .StackPageCount = StackPageCount,
            };

            const auto error = instance.Launch(pid, tid, Executables::ObjectTestImage(), options, KernelStackPages, runtime.Memory.Physical,
                runtime.Memory.KernelAddresses, runtime.Memory.KernelPageMap, runtime.Scheduling.BootstrapScheduler);

            if (error != Processes::UserProcessLaunchError::Success)
                Diagnostics::Fatal("Object", Processes::UserProcessInstance::Describe(error));
        }

        [[nodiscard]] volatile ::Zos::User::ObjectTest::State* TestState(Processes::UserProcessInstance& instance) noexcept {
            const auto* space = instance.ProcessObject().AddressSpace();
            if (space == nullptr)
                Diagnostics::Fatal("Object", "object test process has no address space");

            const auto translation = space->Translate(VirtualAddress(StateAddress));
            if (!translation.Mapped ||
                !HasAccess(translation.Options.Access, PageAccess::Read) ||
                !HasAccess(translation.Options.Access, PageAccess::Write) ||
                !HasAccess(translation.Options.Access, PageAccess::User))
                Diagnostics::Fatal("Object", "object test state mapping is invalid");

            const VirtualAddress direct = Layout::DirectMapAddress(translation.Physical);
            if (direct.IsNull())
                Diagnostics::Fatal("Object", "object test state has no direct-map alias");

            return reinterpret_cast<volatile ::Zos::User::ObjectTest::State*>(direct.Value());
        }

        void WaitForTermination(KernelRuntime& runtime) noexcept {
            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (g_Fixture.First.ProcessObject().State() != Processes::ProcessState::Terminated ||
                   g_Fixture.Second.ProcessObject().State() != Processes::ProcessState::Terminated) {
                if (runtime.Time.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                    Diagnostics::Fatal("Object", "native handle integration test timed out");

                __asm__ volatile("pause");
            }
        }

        void VerifyProcess(Processes::UserProcessInstance& instance, volatile ::Zos::User::ObjectTest::State& state, Processes::ProcessId expected_pid, Scheduling::ThreadId expected_tid) noexcept {
            const auto& process_object = static_cast<const Objects::Object&>(instance.ProcessObject());
            const auto& thread_object = static_cast<const Objects::Object&>(instance.MainThread());

            if (state.Marker != EntryMarker || state.Passed != 1 || state.ProcessId != expected_pid ||
                state.ThreadId != expected_tid || state.FirstProcessHandle == 0 || state.FirstThreadHandle == 0 ||
                state.ReopenedProcessHandle == 0 || state.ReopenedProcessHandle == state.FirstProcessHandle ||
                state.WrongTypeRejected != 1 || state.StaleHandleRejected != 1 || state.HandlesClosed != 1)
                Diagnostics::Fatal("Object", "userspace handle contract failed");

            const auto& exit = instance.ProcessObject().ExitRecord();
            if (!exit.Valid || exit.Status != static_cast<Memory::Uint32>(TestExitStatus) ||
                instance.ProcessObject().FaultRecord().Valid ||
                instance.MainThread().State() != Scheduling::ThreadState::Terminated)
                Diagnostics::Fatal("Object", "object test process did not exit cleanly");

            if (!instance.ProcessObject().Handles().Validate() || instance.ProcessObject().Handles().ActiveCount() != 0 ||
                process_object.ReferenceCount() != 1 || thread_object.ReferenceCount() != 1)
                Diagnostics::Fatal("Object", "process handle references were not released");
        }
    }

    void Run(KernelRuntime& runtime) noexcept {
        LaunchOne(runtime, g_Fixture.First, FirstPid, FirstTid);
        LaunchOne(runtime, g_Fixture.Second, SecondPid, SecondTid);

        WaitForTermination(runtime);

        volatile auto* first = TestState(g_Fixture.First);
        volatile auto* second = TestState(g_Fixture.Second);

        VerifyProcess(g_Fixture.First, *first, FirstPid, FirstTid);

        VerifyProcess(g_Fixture.Second, *second, SecondPid, SecondTid);

        const auto& first_process_object = static_cast<const Objects::Object&>(g_Fixture.First.ProcessObject());
        const auto& second_process_object = static_cast<const Objects::Object&>(g_Fixture.Second.ProcessObject());

        const auto& first_thread_object = static_cast<const Objects::Object&>(g_Fixture.First.MainThread());
        const auto& second_thread_object = static_cast<const Objects::Object&>(g_Fixture.Second.MainThread());

        if (first->FirstProcessHandle != second->FirstProcessHandle ||
            first->FirstThreadHandle != second->FirstThreadHandle ||
            first_process_object.Id() == second_process_object.Id() ||
            first_thread_object.Id() == second_thread_object.Id())
            Diagnostics::Fatal("Object", "per-process handle locality was not preserved");

        Diagnostics::Write("[zOS/User] Native process and thread handles verified.\n");
        Diagnostics::Write("[zOS/User] Cross-process handle locality verified.\n");
        Diagnostics::Write("[zOS/Object] Kernel object and handle model verified.\n");
    }
}
