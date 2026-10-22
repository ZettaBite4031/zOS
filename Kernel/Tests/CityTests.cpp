#include <Kernel/Tests/CityTests.hpp>
#include <Kernel/Kernel.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Tests/Fixtures/TestRuntime.hpp>
#include <User/CityPrograms/Contract.hpp>

namespace Zos::Kernel::Tests::City {
    namespace {
        using namespace Memory;
        using namespace ::Zos::User::CityPrograms;
        using Processes::UserProcessInstance;
        inline constexpr Processes::ProcessId ExitPid{ 2 };
        inline constexpr Processes::ProcessId FaultPid{ 3 };
        inline constexpr Processes::ProcessId SurvivorPid{ 4 };
        inline constexpr Scheduling::ThreadId ExitTid{ 3 };
        inline constexpr Scheduling::ThreadId FaultTid{ 4 };
        inline constexpr Uint64 TimeoutNanoseconds{ 2'000'000'000ULL };
        [[nodiscard]] volatile State* ProcessState(UserProcessInstance& instance) noexcept {
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

            return reinterpret_cast<volatile State*>(direct.Value());
        }
    }

    void RunCitySelfTest(KernelRuntime& runtime) noexcept {
        volatile State* const exiting = ProcessState(Tests::Fixtures::Get().CityExit);
        volatile State* const faulting = ProcessState(Tests::Fixtures::Get().CityFault);
        volatile State* const surviving = ProcessState(Tests::Fixtures::Get().CitySurvivor);

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (exiting->Ready != 1 || faulting->Ready != 1 || surviving->Ready != 1) {
            if (runtime.Clock.Nanoseconds() - start > TimeoutNanoseconds)
                Diagnostics::Fatal("City", "three-process startup timed out");
            __asm__ volatile("pause");
        }

        if (exiting->Marker != EntryMarker || faulting->Marker != EntryMarker ||
            surviving->Marker != EntryMarker ||
            exiting->RoleId != static_cast<Uint64>(Role::Exit) ||
            faulting->RoleId != static_cast<Uint64>(Role::Fault) ||
            surviving->RoleId != static_cast<Uint64>(Role::Survivor) ||
            exiting->ProcessId != ExitPid || faulting->ProcessId != FaultPid ||
            surviving->ProcessId != SurvivorPid ||
            exiting->Progress == 0 || faulting->Progress == 0 || surviving->Progress == 0 ||
            Tests::Fixtures::Get().CityExit.ProcessObject().State() != Processes::ProcessState::Active ||
            Tests::Fixtures::Get().CityFault.ProcessObject().State() != Processes::ProcessState::Active ||
            Tests::Fixtures::Get().CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active)
            Diagnostics::Fatal("City", "concurrent process execution contract failed");

        Diagnostics::Write("[zOS/City] All three native user programs reached main().\n");

        const Uint64 initial_terminations = runtime.BootstrapScheduler.TerminationCount();
        exiting->Arm = 1;
        faulting->Arm = 1;
        surviving->Arm = 1;

        const Uint64 completion_start = runtime.Clock.Nanoseconds();
        while (Tests::Fixtures::Get().CityExit.ProcessObject().State() != Processes::ProcessState::Terminated ||
               Tests::Fixtures::Get().CityFault.ProcessObject().State() != Processes::ProcessState::Terminated) {
            if (runtime.Clock.Nanoseconds() - completion_start > TimeoutNanoseconds)
                Diagnostics::Fatal("City", "independent process termination timed out");
            __asm__ volatile("pause");
        }

        const auto& exit_record = Tests::Fixtures::Get().CityExit.ProcessObject().ExitRecord();
        const auto& fault_record = Tests::Fixtures::Get().CityFault.ProcessObject().FaultRecord();
        const UserFault& fault = fault_record.Fault;

        if (!exit_record.Valid || exit_record.ThreadId != ExitTid ||
            exit_record.Status != static_cast<Uint32>(ExitStatus) ||
            Tests::Fixtures::Get().CityExit.ProcessObject().FaultRecord().Valid ||
            Tests::Fixtures::Get().CityExit.MainThread().State() != Scheduling::ThreadState::Terminated)
            Diagnostics::Fatal("City", "normal-exit process outcome was incorrect");

        if (!fault_record.Valid || fault_record.ThreadId != FaultTid ||
            Tests::Fixtures::Get().CityFault.ProcessObject().ExitRecord().Valid ||
            Tests::Fixtures::Get().CityFault.MainThread().State() != Scheduling::ThreadState::Terminated ||
            fault.Kind != UserFaultKind::PageFault || !fault.AddressValid ||
            fault.Address != VirtualAddress(StackGuardAddress) ||
            !Tests::Fixtures::Get().CityFault.Image().ContainsExecutableAddress(fault.InstructionPointer) ||
            fault.Access != UserFaultAccess::Read ||
            fault.Reason != UserFaultReason::NonPresent || fault.ReservedBitViolation)
            Diagnostics::Fatal("City", "faulted process outcome was incorrect");

        if (runtime.BootstrapScheduler.TerminationCount() != initial_terminations + 2 ||
            Tests::Fixtures::Get().CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active ||
            Tests::Fixtures::Get().CitySurvivor.ProcessObject().FaultRecord().Valid ||
            Tests::Fixtures::Get().CitySurvivor.ProcessObject().ExitRecord().Valid ||
            !runtime.KernelPageMap.IsActive())
            Diagnostics::Fatal("City", "survivor or kernel did not remain active");

        const Uint64 exit_progress = exiting->Progress;
        const Uint64 fault_progress = faulting->Progress;
        const Uint64 survivor_progress = surviving->Progress;
        const Uint64 initial_ticks = runtime.Timer.InterruptCount();
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();

        while (runtime.Timer.InterruptCount() < initial_ticks + 8 ||
               surviving->Progress == survivor_progress) {
            if (runtime.Clock.Nanoseconds() - continuation_start > TimeoutNanoseconds)
                Diagnostics::Fatal("City", "surviving process made no forward progress");
            __asm__ volatile("pause");
        }

        if (exiting->Progress != exit_progress || faulting->Progress != fault_progress ||
            Tests::Fixtures::Get().CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active ||
            Tests::Fixtures::Get().CityExit.MainThread().State() != Scheduling::ThreadState::Terminated ||
            Tests::Fixtures::Get().CityFault.MainThread().State() != Scheduling::ThreadState::Terminated ||
            runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("City", "terminated process resumed or surviving process stopped");

        Diagnostics::Write("[zOS/City] Independent normal exit verified.\n");
        Diagnostics::Write("[zOS/City] Fatal CPL3 page fault isolated.\n");
        Diagnostics::Write("[zOS/City] Third process continued under timer preemption.\n");
        Diagnostics::Write("[zOS/City] First City verified.\n");
    }
}
