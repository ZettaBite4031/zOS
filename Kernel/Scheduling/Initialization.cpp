#include <Kernel/Scheduling/Initialization.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Scheduling/SelfTest.hpp>
#include <Kernel/Time/Initialization.hpp>

namespace Zos::Kernel::Scheduling {
    namespace {
        using namespace Architecture::AMD64;

        inline constexpr ThreadId BootstrapThreadId{ 0 };

        void InitializeBootstrapThread(KernelRuntime& runtime) noexcept {
            const auto error = runtime.Scheduling.BootstrapThread.InitializeBootstrap(BootstrapThreadId, runtime.Processes.KernelProcess, runtime.Memory.PrimaryStack);
            if (error != ThreadInitializationError::Success)
                Diagnostics::Fatal("Thread", Thread::Describe(error));

            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.Platform.BootstrapProcessor ||
                !processor->IsInitialized())
                Diagnostics::Fatal("Thread", "current processor is invalid during bootstrap-thread binding");

            processor->CurrentThread = &runtime.Scheduling.BootstrapThread;
            if (!SetCurrentKernelEntryStack(runtime.Memory.PrimaryStack.Top()))
                Diagnostics::Fatal("Thread", "failed to bind bootstrap kernel entry stack");

            if (processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
                !runtime.Scheduling.BootstrapThread.IsInitialized() ||
                runtime.Scheduling.BootstrapThread.Id() != BootstrapThreadId ||
                runtime.Scheduling.BootstrapThread.State() != ThreadState::Running ||
                runtime.Scheduling.BootstrapThread.OwningProcess() != &runtime.Processes.KernelProcess ||
                runtime.Scheduling.BootstrapThread.Stack() != &runtime.Memory.PrimaryStack ||
                runtime.Scheduling.BootstrapThread.OwnsStack() ||
                runtime.Scheduling.BootstrapThread.HasContext())
                Diagnostics::Fatal("Thread", "bootstrap-thread state or processor binding is invalid");

            Diagnostics::Write("[zOS/Thread] Current processor binding verified.\n");
        }

        void InitializeScheduler(KernelRuntime& runtime) noexcept {
            ProcessorLocal* processor = CurrentProcessor();

            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &runtime.Scheduling.BootstrapThread)
                Diagnostics::Fatal("Scheduler", "bootstrap processor state is invalid during scheduler initialization");

            const auto error = runtime.Scheduling.BootstrapScheduler.Initialize(runtime.Platform.BootstrapProcessor);
            if (error != SchedulerInitializationError::Success)
                Diagnostics::Fatal("Scheduler", Scheduler::Describe(error));

            SelfTest::RegisterWorker(runtime);

            if (processor->LocalScheduler != &runtime.Scheduling.BootstrapScheduler ||
                runtime.Scheduling.BootstrapScheduler.ReadyCount() != 1)
                Diagnostics::Fatal("Scheduler", "initial ready queue state is invalid");

            Diagnostics::Write("[zOS/Scheduler] BSP scheduler initialized.\n");
            Diagnostics::Write("[zOS/Scheduler] Ready threads: ");
            Diagnostics::WriteDecimal(runtime.Scheduling.BootstrapScheduler.ReadyCount());
            Diagnostics::Write("\n");
        }
    }

    void Initialize(KernelRuntime& runtime) noexcept {
        InitializeBootstrapThread(runtime);

        SelfTest::PrepareWorker(runtime);
        SelfTest::VerifyCooperativeSwitch(runtime);

        InitializeScheduler(runtime);
        SelfTest::VerifyRoundRobin(runtime);

        Time::InitializeSchedulerTimer(
            runtime.Time,
            runtime.Platform.Apic,
            runtime.Architecture.Interrupts,
            runtime.Platform.BootstrapProcessor
        );

        if (Architecture::AMD64::CurrentProcessor()->CurrentThread != &runtime.Scheduling.BootstrapThread)
            Diagnostics::Fatal("Thread", "current thread changed unexpectedly during timer initialization");

        SelfTest::VerifyPreemption(runtime);
    }
}
