#include <Kernel/Initialization/CityBootstrap.hpp>

#include <Kernel/Kernel.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>

#include <User/CityPrograms/Contract.hpp>

namespace Zos::Kernel::Initialization::CityBootstrap {
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

        void VerifyPrivateMappings(KernelRuntime& runtime) noexcept {
            const VirtualAddress address(StateAddress);
            const VirtualAddress stack(StackBaseAddress);
            const VirtualAddress guard(StackGuardAddress);

            if (runtime.KernelPageMap.IsMapped(address) ||
                runtime.KernelPageMap.IsMapped(stack) ||
                runtime.KernelPageMap.IsMapped(guard))
                Diagnostics::Fatal("City", "City user mapping leaked into kernel CR3");

            UserProcessInstance* const programs[]{ &runtime.CityExit, &runtime.CityFault, &runtime.CitySurvivor };
            for (Uint64 i = 0; i < 3; ++i) {
                const auto* space = programs[i]->ProcessObject().AddressSpace();
                if (space == nullptr || space->RootTable() == runtime.KernelPageMap.RootTable() ||
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

        void LaunchOne(KernelRuntime& runtime, UserProcessInstance& instance,
            Processes::ProcessId pid, Scheduling::ThreadId tid,
            const Executables::EmbeddedImage& image) noexcept {
            const Executables::UserExecutableLoadOptions options{
                .StackBase = VirtualAddress(StackBaseAddress),
                .StackPageCount = StackPageCount,
            };

            const auto error = instance.Launch(pid, tid, image, options, KernelStackPages,
                runtime.PhysicalMemory, runtime.KernelAddresses,
                runtime.KernelPageMap, runtime.BootstrapScheduler);
            if (error != Processes::UserProcessLaunchError::Success)
                Diagnostics::Fatal("City", UserProcessInstance::Describe(error));
        }
    }

    void LaunchCity(KernelRuntime& runtime) noexcept {
        if (runtime.FirstUserProcess.State() != Processes::ProcessState::Terminated ||
            !runtime.FirstUserProcess.ExitRecord().Valid ||
            runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive())
            Diagnostics::Fatal("City", "launch invoked before native runtime verification");

        LaunchOne(runtime, runtime.CityExit, ExitPid, ExitTid, Executables::CityExitImage());
        LaunchOne(runtime, runtime.CityFault, FaultPid, FaultTid, Executables::CityFaultImage());
        LaunchOne(runtime, runtime.CitySurvivor, SurvivorPid, SurvivorTid, Executables::CitySurvivorImage());

        VerifyPrivateMappings(runtime);
        Diagnostics::Write("[zOS/City] Three isolated ELF processes launched.\n");
        Diagnostics::Write("[zOS/City] Private data and stack backing verified.\n");
    }

    void RunCitySelfTest(KernelRuntime& runtime) noexcept {
        volatile State* const exiting = ProcessState(runtime.CityExit);
        volatile State* const faulting = ProcessState(runtime.CityFault);
        volatile State* const surviving = ProcessState(runtime.CitySurvivor);

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
            runtime.CityExit.ProcessObject().State() != Processes::ProcessState::Active ||
            runtime.CityFault.ProcessObject().State() != Processes::ProcessState::Active ||
            runtime.CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active)
            Diagnostics::Fatal("City", "concurrent process execution contract failed");

        Diagnostics::Write("[zOS/City] All three native user programs reached main().\n");

        const Uint64 initial_terminations = runtime.BootstrapScheduler.TerminationCount();
        exiting->Arm = 1;
        faulting->Arm = 1;
        surviving->Arm = 1;

        const Uint64 completion_start = runtime.Clock.Nanoseconds();
        while (runtime.CityExit.ProcessObject().State() != Processes::ProcessState::Terminated ||
               runtime.CityFault.ProcessObject().State() != Processes::ProcessState::Terminated) {
            if (runtime.Clock.Nanoseconds() - completion_start > TimeoutNanoseconds)
                Diagnostics::Fatal("City", "independent process termination timed out");
            __asm__ volatile("pause");
        }

        const auto& exit_record = runtime.CityExit.ProcessObject().ExitRecord();
        const auto& fault_record = runtime.CityFault.ProcessObject().FaultRecord();
        const UserFault& fault = fault_record.Fault;

        if (!exit_record.Valid || exit_record.ThreadId != ExitTid ||
            exit_record.Status != static_cast<Uint32>(ExitStatus) ||
            runtime.CityExit.ProcessObject().FaultRecord().Valid ||
            runtime.CityExit.MainThread().State() != Scheduling::ThreadState::Terminated)
            Diagnostics::Fatal("City", "normal-exit process outcome was incorrect");

        if (!fault_record.Valid || fault_record.ThreadId != FaultTid ||
            runtime.CityFault.ProcessObject().ExitRecord().Valid ||
            runtime.CityFault.MainThread().State() != Scheduling::ThreadState::Terminated ||
            fault.Kind != UserFaultKind::PageFault || !fault.AddressValid ||
            fault.Address != VirtualAddress(StackGuardAddress) ||
            !runtime.CityFault.Image().ContainsExecutableAddress(fault.InstructionPointer) ||
            fault.Access != UserFaultAccess::Read ||
            fault.Reason != UserFaultReason::NonPresent || fault.ReservedBitViolation)
            Diagnostics::Fatal("City", "faulted process outcome was incorrect");

        if (runtime.BootstrapScheduler.TerminationCount() != initial_terminations + 2 ||
            runtime.CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active ||
            runtime.CitySurvivor.ProcessObject().FaultRecord().Valid ||
            runtime.CitySurvivor.ProcessObject().ExitRecord().Valid ||
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
            runtime.CitySurvivor.ProcessObject().State() != Processes::ProcessState::Active ||
            runtime.CityExit.MainThread().State() != Scheduling::ThreadState::Terminated ||
            runtime.CityFault.MainThread().State() != Scheduling::ThreadState::Terminated ||
            runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("City", "terminated process resumed or surviving process stopped");

        Diagnostics::Write("[zOS/City] Independent normal exit verified.\n");
        Diagnostics::Write("[zOS/City] Fatal CPL3 page fault isolated.\n");
        Diagnostics::Write("[zOS/City] Third process continued under timer preemption.\n");
        Diagnostics::Write("[zOS/City] First City verified.\n");
    }
}
