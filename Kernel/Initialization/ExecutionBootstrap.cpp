#include <Kernel/Initialization/ExecutionBootstrap.hpp>
#include <Kernel/Tests/ExecutionTests.hpp>
#include <Kernel/Kernel.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Tests/Fixtures/TestRuntime.hpp>

namespace Zos::Kernel::Initialization::ExecutionBootstrap {
    using namespace Memory;
    using namespace Architecture::AMD64;
    inline constexpr Uint64 FirstKernelThreadStackPages{ 8 };
    inline constexpr Scheduling::ThreadId BootstrapThreadId{ 0 };
    inline constexpr Scheduling::ThreadId FirstWorkerThreadId{ 1 };
    inline constexpr Processes::ProcessId KernelProcessId{ 0 };
    inline constexpr Processes::ProcessId FirstUserProcessId{ 1 };

    void InitializeKernelProcess(KernelRuntime& runtime) noexcept {
        const auto process_error = runtime.KernelProcess.InitializeKernel(KernelProcessId, runtime.KernelPageMap);
        if (process_error != Processes::ProcessInitializationError::Success)
            Diagnostics::Fatal("Process", Processes::Process::Describe(process_error));

        if (!runtime.KernelProcess.IsInitialized() || runtime.KernelProcess.Id() != KernelProcessId ||
            runtime.KernelProcess.State() != Processes::ProcessState::Active || runtime.KernelProcess.ThreadCount() != 0)
            Diagnostics::Fatal("Process", "kernel process initial state is invalid");
    }

    void InitializeKernelThreads(KernelRuntime& runtime) noexcept {
        const auto bootstrap_error = runtime.BootstrapThread.InitializeBootstrap(BootstrapThreadId, runtime.KernelProcess, runtime.PrimaryStack);
        if (bootstrap_error != Scheduling::ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(bootstrap_error));

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || !processor->IsInitialized())
            Diagnostics::Fatal("Thread", "current processor is invalid during bootstrap-thread binding");

        /*
        * The BSP is already executing this context. Binding the Thread object
        * records that reality rather than performing a context transition.
        */
        processor->CurrentThread = &runtime.BootstrapThread;

        if (!SetCurrentKernelEntryStack(runtime.PrimaryStack.Top()))
            Diagnostics::Fatal("Thread", "failed to bind bootstrap kernel entry stack");

        if (processor->CurrentThread != &runtime.BootstrapThread || !runtime.BootstrapThread.IsInitialized() || runtime.BootstrapThread.Id() != BootstrapThreadId ||
            runtime.BootstrapThread.State() != Scheduling::ThreadState::Running || runtime.BootstrapThread.OwningProcess() != &runtime.KernelProcess ||
            runtime.BootstrapThread.Stack() != &runtime.PrimaryStack || runtime.BootstrapThread.OwnsStack() || runtime.BootstrapThread.HasContext())
            Diagnostics::Fatal("Thread", "bootstrap-thread state or processor binding is invalid");

        Diagnostics::Write("[zOS/Thread] Current processor binding verified.\n");

        const auto worker_error = Tests::Fixtures::Get().FirstWorkerThread.InitializeKernel(FirstWorkerThreadId, runtime.KernelProcess, &Tests::Execution::FirstWorkerThreadEntry,
            &runtime, runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, FirstKernelThreadStackPages);

        if (worker_error != Scheduling::ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(worker_error));

        const Memory::KernelStack* worker_stack = Tests::Fixtures::Get().FirstWorkerThread.Stack();
        if (!Tests::Fixtures::Get().FirstWorkerThread.IsInitialized() || Tests::Fixtures::Get().FirstWorkerThread.Id() != FirstWorkerThreadId || Tests::Fixtures::Get().FirstWorkerThread.State() != Scheduling::ThreadState::Ready ||
            Tests::Fixtures::Get().FirstWorkerThread.OwningProcess() != &runtime.KernelProcess || Tests::Fixtures::Get().FirstWorkerThread.Entry() != &Tests::Execution::FirstWorkerThreadEntry || Tests::Fixtures::Get().FirstWorkerThread.EntryContext() != &runtime ||
            !Tests::Fixtures::Get().FirstWorkerThread.OwnsStack() || worker_stack == nullptr || worker_stack == &runtime.PrimaryStack)
            Diagnostics::Fatal("Thread", "first kernel thread state is invalid");

        if (!worker_stack->IsInitialized() || worker_stack->UsableSpan().PageCount != FirstKernelThreadStackPages ||
            runtime.KernelPageMap.IsMapped(worker_stack->LowerGuard()) || runtime.KernelPageMap.IsMapped(worker_stack->UpperGuard()))
            Diagnostics::Fatal("Thread", "first kernel thread stack validation failed");

        if (runtime.PrimaryStack.Contains(worker_stack->Bottom()) || worker_stack->Contains(runtime.PrimaryStack.Bottom()))
            Diagnostics::Fatal("Thread", "kernel thread stacks overlap");
        
        Diagnostics::Write("[zOS/Thread] Kernel thread foundation established.\n");

        if (runtime.KernelProcess.ThreadCount() != 2 || !runtime.KernelProcess.OwnsThread(runtime.BootstrapThread) ||
            !runtime.KernelProcess.OwnsThread(Tests::Fixtures::Get().FirstWorkerThread))
            Diagnostics::Fatal("Process", "kernel process thread membership is invalid");

        Diagnostics::Write("[zOS/Process] Kernel process thread membership verified.\n");
        Diagnostics::Write("[zOS/Process] Threads: ");
        Diagnostics::WriteDecimal(runtime.KernelProcess.ThreadCount());
        Diagnostics::Write("\n");
    }

    void InitializeScheduler(KernelRuntime& runtime) noexcept {
        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("Scheduler", "bootstrap processor state is invalid during scheduler initialization");

        const auto scheduler_error = runtime.BootstrapScheduler.Initialize(runtime.BootstrapProcessor);
        if (scheduler_error != Scheduling::SchedulerInitializationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(scheduler_error));

        const auto registration_error = runtime.BootstrapScheduler.RegisterThread(Tests::Fixtures::Get().FirstWorkerThread);
        if (registration_error != Scheduling::SchedulerRegistrationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(registration_error));

        if (processor->LocalScheduler != &runtime.BootstrapScheduler || runtime.BootstrapScheduler.ReadyCount() != 1)
            Diagnostics::Fatal("Scheduler", "initial ready queue state is invalid");

        Diagnostics::Write("[zOS/Scheduler] BSP scheduler initialized.\n");
        Diagnostics::Write("[zOS/Scheduler] Ready threads: ");
        Diagnostics::WriteDecimal(runtime.BootstrapScheduler.ReadyCount());
        Diagnostics::Write("\n");
    }

    void InitializeProcessAddressSpaces(KernelRuntime& runtime) noexcept {
        const auto process_error = Tests::Fixtures::Get().FirstUserProcess.InitializeIsolated(FirstUserProcessId, runtime.PhysicalMemory, runtime.KernelPageMap);
        if (process_error != Processes::ProcessInitializationError::Success)
            Diagnostics::Fatal("Process", Processes::Process::Describe(process_error));

        if (!Tests::Fixtures::Get().FirstUserProcess.IsInitialized() || Tests::Fixtures::Get().FirstUserProcess.Id() != FirstUserProcessId ||
            Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Active || !Tests::Fixtures::Get().FirstUserProcess.OwnsAddressSpace() ||
            Tests::Fixtures::Get().FirstUserProcess.AddressSpace() == nullptr || Tests::Fixtures::Get().FirstUserProcess.AddressSpace()->RootTable() == runtime.KernelPageMap.RootTable())
            Diagnostics::Fatal("Process", "isolated process address-space state is invalid");

        Diagnostics::Write("[zOS/Process] Independent process address space established.\n");
    }
}
