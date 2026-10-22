#include <Kernel/Initialization/CityBootstrap.hpp>

#include <Kernel/Kernel.hpp>
#include <Kernel/Tests/Fixtures/TestRuntime.hpp>
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



        void VerifyPrivateMappings(KernelRuntime& runtime) noexcept {
            const VirtualAddress address(StateAddress);
            const VirtualAddress stack(StackBaseAddress);
            const VirtualAddress guard(StackGuardAddress);

            if (runtime.KernelPageMap.IsMapped(address) ||
                runtime.KernelPageMap.IsMapped(stack) ||
                runtime.KernelPageMap.IsMapped(guard))
                Diagnostics::Fatal("City", "City user mapping leaked into kernel CR3");

            UserProcessInstance* const programs[]{ &Tests::Fixtures::Get().CityExit, &Tests::Fixtures::Get().CityFault, &Tests::Fixtures::Get().CitySurvivor };
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
        if (Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Terminated ||
            !Tests::Fixtures::Get().FirstUserProcess.ExitRecord().Valid ||
            runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive())
            Diagnostics::Fatal("City", "launch invoked before native runtime verification");

        LaunchOne(runtime, Tests::Fixtures::Get().CityExit, ExitPid, ExitTid, Executables::CityExitImage());
        LaunchOne(runtime, Tests::Fixtures::Get().CityFault, FaultPid, FaultTid, Executables::CityFaultImage());
        LaunchOne(runtime, Tests::Fixtures::Get().CitySurvivor, SurvivorPid, SurvivorTid, Executables::CitySurvivorImage());

        VerifyPrivateMappings(runtime);
        Diagnostics::Write("[zOS/City] Three isolated ELF processes launched.\n");
        Diagnostics::Write("[zOS/City] Private data and stack backing verified.\n");
    }

}
