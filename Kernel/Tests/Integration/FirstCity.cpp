#include <Kernel/Tests/Integration/FirstCity.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Processes/UserProcessLauncher.hpp>

#include <User/CityPrograms/Contract.hpp>

namespace Zos::Kernel::Tests::Integration::FirstCity {
    namespace {
        using namespace Memory;
        using namespace ::Zos::User::CityPrograms;
        using Processes::UserProcessInstance;

        inline constexpr Processes::ProcessId ExitPid{ 2 };
        inline constexpr Processes::ProcessId FaultPid{ 3 };
        inline constexpr Processes::ProcessId SurvivorPid{ 4 };

        inline constexpr Scheduling::ThreadId ExitTid{ 3 };
        inline constexpr Scheduling::ThreadId FaultTid{ 4 };
        inline constexpr Scheduling::ThreadId SurvivorTid{ 5 };

        inline constexpr Uint64 KernelStackPages{ 8 };
        inline constexpr Uint64 TimeoutNanoseconds{
            2'000'000'000ULL
        };

        struct Fixture final {
            UserProcessInstance Exit{};
            UserProcessInstance Fault{};
            UserProcessInstance Survivor{};
        };

        constinit Fixture g_Fixture{};

        void LaunchOne(KernelRuntime& runtime, UserProcessInstance& instance, Processes::ProcessId pid, Scheduling::ThreadId tid, const Executables::EmbeddedImage& image) noexcept {
            const Executables::UserExecutableLoadOptions options{
                .StackBase = VirtualAddress(StackBaseAddress),
                .StackPageCount = StackPageCount,
            };

            const auto error = instance.Launch(pid, tid, image, options, KernelStackPages, runtime.Memory.Physical,
                    runtime.Memory.KernelAddresses, runtime.Memory.KernelPageMap, runtime.Scheduling.BootstrapScheduler);

            if (error != Processes::UserProcessLaunchError::Success)
                Diagnostics::Fatal("City", UserProcessInstance::Describe(error));
        }

        void VerifyPrivateMappings(KernelRuntime& runtime) noexcept {
            const VirtualAddress address{ StateAddress };
            const VirtualAddress stack{ StackBaseAddress };
            const VirtualAddress guard{ StackGuardAddress };

            if (runtime.Memory.KernelPageMap.IsMapped(address) ||
                runtime.Memory.KernelPageMap.IsMapped(stack) ||
                runtime.Memory.KernelPageMap.IsMapped(guard))
                Diagnostics::Fatal("City", "City user mapping leaked into kernel CR3");

            UserProcessInstance* programs[]{
                &g_Fixture.Exit,
                &g_Fixture.Fault,
                &g_Fixture.Survivor,
            };

            for (Uint64 i = 0; i < 3; ++i) {
                const auto* space = programs[i]->ProcessObject().AddressSpace();
                if (space == nullptr ||
                    space->RootTable() == runtime.Memory.KernelPageMap.RootTable() ||
                    space->IsMapped(guard) || !space->IsMapped(stack))
                    Diagnostics::Fatal("City", "City process address space has invalid stack isolation");

                const auto data = space->Translate(address);
                const auto user_stack = space->Translate(stack);
                if (!data.Mapped || !user_stack.Mapped)
                    Diagnostics::Fatal("City", "City process is missing private pages");

                for (Uint64 j = 0; j < i; ++j) {
                    const auto* previous = programs[j]->ProcessObject().AddressSpace();
                    if (space->RootTable() == previous->RootTable() ||
                        data.Physical == previous->Translate(address).Physical ||
                        user_stack.Physical == previous->Translate(stack).Physical)
                        Diagnostics::Fatal("City", "process-private virtual addresses alias physical pages");
                }
            }
        }

        [[nodiscard]] volatile ::Zos::User::CityPrograms::State* ProcessState(UserProcessInstance& instance) noexcept {
            const auto* space = instance.ProcessObject().AddressSpace();
            if (space == nullptr || !instance.IsLaunched())
                Diagnostics::Fatal("City", "process state requested before launch");

            const auto translation = space->Translate(VirtualAddress(StateAddress));
            if (!translation.Mapped ||
                !HasAccess(translation.Options.Access, PageAccess::Read) ||
                !HasAccess(translation.Options.Access, PageAccess::Write) ||
                !HasAccess(translation.Options.Access, PageAccess::User) ||
                HasAccess(translation.Options.Access, PageAccess::Execute) ||
                HasAccess(translation.Options.Access, PageAccess::Global))
                Diagnostics::Fatal("City", "user state is not mapped as private read/write memory");

            const VirtualAddress direct = Layout::DirectMapAddress(translation.Physical);
            if (direct.IsNull())
                Diagnostics::Fatal("City", "user state is missing its direct mapping");

            return reinterpret_cast<volatile ::Zos::User::CityPrograms::State*>(direct.Value());
        }

        void Launch(KernelRuntime& runtime) noexcept {
            if (runtime.Platform.BootstrapProcessor.CurrentThread != &runtime.Scheduling.BootstrapThread ||
                !runtime.Memory.KernelPageMap.IsActive())
                Diagnostics::Fatal("City", "First City launch began with invalid kernel execution state" );

            LaunchOne(runtime, g_Fixture.Exit, ExitPid, ExitTid, Executables::CityExitImage());
            LaunchOne(runtime, g_Fixture.Fault, FaultPid, FaultTid, Executables::CityFaultImage());
            LaunchOne(runtime, g_Fixture.Survivor, SurvivorPid, SurvivorTid, Executables::CitySurvivorImage());

            VerifyPrivateMappings(runtime);

            Diagnostics::Write("[zOS/City] Three isolated ELF processes launched.\n");
            Diagnostics::Write("[zOS/City] Private data and stack backing verified.\n");
        }

        void Verify(KernelRuntime& runtime) noexcept {
            volatile auto* const exiting = ProcessState(g_Fixture.Exit);
            volatile auto* const faulting = ProcessState(g_Fixture.Fault);
            volatile auto* const surviving = ProcessState(g_Fixture.Survivor);

            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (exiting->Ready != 1 || faulting->Ready != 1 || surviving->Ready != 1) {
                if (runtime.Time.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                    Diagnostics::Fatal("City", "three-process startup timed out");

                __asm__ volatile("pause");
            }

            if (exiting->Marker != EntryMarker || faulting->Marker != EntryMarker || surviving->Marker != EntryMarker ||
                exiting->RoleId != static_cast<Uint64>(Role::Exit) ||
                faulting->RoleId != static_cast<Uint64>(Role::Fault) ||
                surviving->RoleId != static_cast<Uint64>(Role::Survivor) ||
                exiting->ProcessId != ExitPid || faulting->ProcessId != FaultPid || surviving->ProcessId != SurvivorPid ||
                exiting->Progress == 0 || faulting->Progress == 0 || surviving->Progress == 0 ||
                g_Fixture.Exit.ProcessObject().State() != Processes::ProcessState::Active ||
                g_Fixture.Fault.ProcessObject().State() != Processes::ProcessState::Active ||
                g_Fixture.Survivor.ProcessObject().State() != Processes::ProcessState::Active)
                Diagnostics::Fatal("City", "concurrent process execution contract failed");

            Diagnostics::Write("[zOS/City] All three native user programs reached main().\n");

            const Uint64 initial_terminations = runtime.Scheduling.BootstrapScheduler.TerminationCount();

            exiting->Arm = 1;
            faulting->Arm = 1;
            surviving->Arm = 1;

            const Uint64 completion_start = runtime.Time.Clock.Nanoseconds();
            while (g_Fixture.Exit.ProcessObject().State() != Processes::ProcessState::Terminated ||
                   g_Fixture.Fault.ProcessObject().State() != Processes::ProcessState::Terminated) {
                if (runtime.Time.Clock.Nanoseconds() - completion_start > TimeoutNanoseconds)
                    Diagnostics::Fatal("City", "independent process termination timed out");

                __asm__ volatile("pause");
            }

            const auto& exit_record = g_Fixture.Exit.ProcessObject().ExitRecord();
            const auto& fault_record = g_Fixture.Fault.ProcessObject().FaultRecord();
            const UserFault& fault = fault_record.Fault;
            if (!exit_record.Valid || exit_record.ThreadId != ExitTid ||
                exit_record.Status != static_cast<Uint32>(ExitStatus ) ||
                g_Fixture.Exit.ProcessObject().FaultRecord().Valid ||
                g_Fixture.Exit.MainThread().State() != Scheduling::ThreadState::Terminated)
                Diagnostics::Fatal("City", "normal-exit process outcome was incorrect");

            if (!fault_record.Valid || fault_record.ThreadId != FaultTid ||
                g_Fixture.Fault.ProcessObject().ExitRecord().Valid ||
                g_Fixture.Fault.MainThread().State() != Scheduling::ThreadState::Terminated ||
                fault.Kind != UserFaultKind::PageFault || !fault.AddressValid ||
                fault.Address != VirtualAddress(StackGuardAddress) ||
                !g_Fixture.Fault.Image().ContainsExecutableAddress(fault.InstructionPointer) ||
                fault.Access != UserFaultAccess::Read || fault.Reason !=UserFaultReason::NonPresent ||
                fault.ReservedBitViolation)
                Diagnostics::Fatal("City", "faulted process outcome was incorrect");

            if (runtime.Scheduling.BootstrapScheduler.TerminationCount() != initial_terminations + 2 ||
                g_Fixture.Survivor.ProcessObject().State() != Processes::ProcessState::Active ||
                g_Fixture.Survivor.ProcessObject().FaultRecord().Valid ||
                g_Fixture.Survivor.ProcessObject().ExitRecord().Valid ||
                !runtime.Memory.KernelPageMap.IsActive())
                Diagnostics::Fatal("City", "survivor or kernel did not remain active");

            const Uint64 exit_progress = exiting->Progress;
            const Uint64 fault_progress = faulting->Progress;
            const Uint64 survivor_progress = surviving->Progress;
            const Uint64 initial_ticks = runtime.Time.Timer.InterruptCount();
            const Uint64 continuation_start = runtime.Time.Clock.Nanoseconds();
            while (runtime.Time.Timer.InterruptCount() < initial_ticks + 8 ||
                   surviving->Progress == survivor_progress) {
                if (runtime.Time.Clock.Nanoseconds() - continuation_start > TimeoutNanoseconds)
                    Diagnostics::Fatal("City", "surviving process made no forward progress");

                __asm__ volatile("pause");
            }

            if (exiting->Progress != exit_progress || faulting->Progress != fault_progress ||
                g_Fixture.Survivor.ProcessObject().State() != Processes::ProcessState::Active ||
                g_Fixture.Exit.MainThread().State() != Scheduling::ThreadState::Terminated ||
                g_Fixture.Fault.MainThread().State() != Scheduling::ThreadState::Terminated ||
                runtime.Platform.BootstrapProcessor.CurrentThread != &runtime.Scheduling.BootstrapThread)
                Diagnostics::Fatal("City", "terminated process resumed or surviving process stopped");

            Diagnostics::Write("[zOS/City] Independent normal exit verified.\n");
            Diagnostics::Write("[zOS/City] Fatal CPL3 page fault isolated.\n");
            Diagnostics::Write("[zOS/City] Third process continued under timer preemption.\n");
            Diagnostics::Write("[zOS/City] First City verified.\n");
        }
    }

    void Run(KernelRuntime& runtime) noexcept {
        Launch(runtime);
        Verify(runtime);
    }
}
