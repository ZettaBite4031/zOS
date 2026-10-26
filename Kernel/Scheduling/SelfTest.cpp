#include <Kernel/Scheduling/SelfTest.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Scheduling::SelfTest {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

        inline constexpr ThreadId WorkerThreadId{ 1 };
        inline constexpr Uint64 WorkerStackPages{ 8 };
        inline constexpr Uint64 RoundRobinRounds{ 4 };
        inline constexpr Uint64 MinimumPreemptions{ 6 };
        inline constexpr Uint64 PreemptionTimeoutNanoseconds{ 1'000'000'000ULL };

        constinit Thread g_WorkerThread{};

        bool g_WorkerEntered{};
        bool g_WorkerStackVerified{};
        volatile Uint64 g_RoundRobinIterations{};
        volatile Uint64 g_PreemptionIterations{};
        volatile bool g_PreemptionComplete{};

        [[noreturn]] void WorkerEntry(void* context) noexcept {
            if (context == nullptr)
                Diagnostics::Fatal("Thread", "scheduler test worker received a null context");

            auto& runtime = *static_cast<KernelRuntime*>(context);

            ProcessorLocal* processor = CurrentProcessor();

            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &g_WorkerThread)
                Diagnostics::Fatal("Thread", "scheduler test worker does not own the current processor");

            Uint64 rsp{};
            __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));

            const Memory::KernelStack* stack = g_WorkerThread.Stack();

            if (stack == nullptr || !stack->Contains(VirtualAddress(rsp)))
                Diagnostics::Fatal("Thread", "scheduler test worker is not executing on its own stack");

            g_WorkerEntered = true;
            g_WorkerStackVerified = true;

            Diagnostics::Write("[zOS/Thread] Worker thread entered on dedicated stack.\n");

            const auto switch_error = g_WorkerThread.SwitchTo(runtime.Scheduling.BootstrapThread);
            if (switch_error != ThreadSwitchError::Success)
                Diagnostics::Fatal("Thread", Thread::Describe(switch_error));

            processor = CurrentProcessor();
            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &g_WorkerThread ||
                processor->LocalScheduler == nullptr ||
                !processor->LocalScheduler->IsInitialized())
                Diagnostics::Fatal("Scheduler", "scheduler test worker resumed without a valid local scheduler");

            Diagnostics::Write("[zOS/Scheduler] Worker thread joined scheduler rotation.\n");

            for (;;) {
                processor = CurrentProcessor();

                if (processor == nullptr || processor->LocalScheduler == nullptr)
                    Diagnostics::Fatal("Scheduler", "scheduler test worker lost its local scheduler");

                if (processor->LocalScheduler->PreemptionEnabled()) {
                    __asm__ volatile("sti" ::: "memory");

                    while (!g_PreemptionComplete) {
                        g_PreemptionIterations = g_PreemptionIterations + 1;
                        __asm__ volatile("pause");
                    }

                    for (;;) __asm__ volatile("hlt");
                }

                g_RoundRobinIterations = g_RoundRobinIterations + 1;

                const auto yield_error = processor->LocalScheduler->Yield();
                if (yield_error != SchedulerYieldError::Success)
                    Diagnostics::Fatal("Scheduler", Scheduler::Describe(yield_error));

                processor = CurrentProcessor();
                if (processor != &runtime.Platform.BootstrapProcessor ||
                    processor->CurrentThread != &g_WorkerThread ||
                    processor->LocalScheduler != &runtime.Scheduling.BootstrapScheduler)
                    Diagnostics::Fatal("Scheduler", "scheduler test worker resumed with invalid scheduler ownership");
            }
        }
    }

    void PrepareWorker(KernelRuntime& runtime) noexcept {
        const auto error = g_WorkerThread.InitializeKernel(WorkerThreadId, runtime.Processes.KernelProcess, &WorkerEntry, &runtime, 
            runtime.Memory.Physical, runtime.Memory.KernelAddresses, runtime.Memory.KernelPageMap, WorkerStackPages);
            
        if (error != ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Thread::Describe(error));

        const Memory::KernelStack* worker_stack = g_WorkerThread.Stack();
        if (!g_WorkerThread.IsInitialized() ||
            g_WorkerThread.Id() != WorkerThreadId ||
            g_WorkerThread.State() != ThreadState::Ready ||
            g_WorkerThread.OwningProcess() != &runtime.Processes.KernelProcess ||
            g_WorkerThread.Entry() != &WorkerEntry ||
            g_WorkerThread.EntryContext() != &runtime ||
            !g_WorkerThread.OwnsStack() || worker_stack == nullptr ||
            worker_stack == &runtime.Memory.PrimaryStack)
            Diagnostics::Fatal("Thread", "scheduler test worker state is invalid");

        if (!worker_stack->IsInitialized() ||
            worker_stack->UsableSpan().PageCount != WorkerStackPages ||
            runtime.Memory.KernelPageMap.IsMapped(worker_stack->LowerGuard()) ||
            runtime.Memory.KernelPageMap.IsMapped(worker_stack->UpperGuard()))
            Diagnostics::Fatal("Thread", "scheduler test worker stack validation failed");

        if (runtime.Memory.PrimaryStack.Contains(worker_stack->Bottom()) ||
            worker_stack->Contains(runtime.Memory.PrimaryStack.Bottom()))
            Diagnostics::Fatal("Thread", "kernel thread stacks overlap");

        Diagnostics::Write("[zOS/Thread] Kernel thread foundation established.\n");
        if (runtime.Processes.KernelProcess.ThreadCount() != 2 ||
            !runtime.Processes.KernelProcess.OwnsThread(runtime.Scheduling.BootstrapThread) ||
            !runtime.Processes.KernelProcess.OwnsThread(g_WorkerThread))
            Diagnostics::Fatal("Process", "kernel process thread membership is invalid");

        Diagnostics::Write("[zOS/Process] Kernel process thread membership verified.\n");
        Diagnostics::Write("[zOS/Process] Threads: ");
        Diagnostics::WriteDecimal(runtime.Processes.KernelProcess.ThreadCount());
        Diagnostics::Write("\n");
    }

    void VerifyCooperativeSwitch(KernelRuntime& runtime) noexcept {
        g_WorkerEntered = false;
        g_WorkerStackVerified = false;

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.Platform.BootstrapProcessor ||
            processor->CurrentThread != &runtime.Scheduling.BootstrapThread)
            Diagnostics::Fatal("Thread", "bootstrap thread is not current before context-switch test");

        Uint64 rsp_before{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp_before));

        if (!runtime.Memory.PrimaryStack.Contains(VirtualAddress(rsp_before)))
            Diagnostics::Fatal("Thread", "bootstrap thread is not executing on the primary stack");

        const auto switch_error = runtime.Scheduling.BootstrapThread.SwitchTo(g_WorkerThread);
        if (switch_error != ThreadSwitchError::Success)
            Diagnostics::Fatal("Thread", Thread::Describe(switch_error));

        if (!g_WorkerEntered || !g_WorkerStackVerified)
            Diagnostics::Fatal("Thread", "scheduler test worker did not execute successfully");

        processor = CurrentProcessor();
        if (processor != &runtime.Platform.BootstrapProcessor ||
            processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
            runtime.Scheduling.BootstrapThread.State() != ThreadState::Running ||
            g_WorkerThread.State() != ThreadState::Ready ||
            !runtime.Scheduling.BootstrapThread.HasContext() ||
            !g_WorkerThread.HasContext())
            Diagnostics::Fatal("Thread", "thread state was not restored after cooperative switch");

        Uint64 rsp_after{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp_after));

        if (!runtime.Memory.PrimaryStack.Contains(VirtualAddress(rsp_after)))
            Diagnostics::Fatal("Thread", "bootstrap thread resumed on the wrong stack");

        Diagnostics::Write("[zOS/Thread] Cooperative context switch verified.\n");
    }

    void RegisterWorker(KernelRuntime& runtime) noexcept {
        const auto error = runtime.Scheduling.BootstrapScheduler.RegisterThread(g_WorkerThread);
        if (error != SchedulerRegistrationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduler::Describe(error));
    }

    void VerifyRoundRobin(KernelRuntime& runtime) noexcept {
        g_RoundRobinIterations = 0;

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.Platform.BootstrapProcessor ||
            processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
            processor->LocalScheduler != &runtime.Scheduling.BootstrapScheduler)
            Diagnostics::Fatal("Scheduler", "scheduler self-test began with invalid processor state");

        const Uint64 initial_switch_count = runtime.Scheduling.BootstrapScheduler.SwitchCount();
        for (Uint64 round = 0; round < RoundRobinRounds; ++round) {
            const auto yield_error = runtime.Scheduling.BootstrapScheduler.Yield();
            if (yield_error != SchedulerYieldError::Success)
                Diagnostics::Fatal("Scheduler", Scheduler::Describe(yield_error));

            const Uint64 expected_iterations = round + 1;
            const Uint64 expected_switches = initial_switch_count + expected_iterations * 2;

            processor = CurrentProcessor();

            if (g_RoundRobinIterations != expected_iterations ||
                processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
                runtime.Scheduling.BootstrapThread.State() != ThreadState::Running ||
                g_WorkerThread.State() != ThreadState::Ready ||
                runtime.Scheduling.BootstrapScheduler.ReadyCount() != 1 ||
                runtime.Scheduling.BootstrapScheduler.SwitchCount() != expected_switches)
                Diagnostics::Fatal("Scheduler", "round-robin scheduler self-test state is invalid");
        }

        Diagnostics::Write("[zOS/Scheduler] Cooperative round-robin verified.\n");
        Diagnostics::Write("[zOS/Scheduler] Context switches: ");
        Diagnostics::WriteDecimal(runtime.Scheduling.BootstrapScheduler.SwitchCount());
        Diagnostics::Write("\n");
    }

    void VerifyPreemption(KernelRuntime& runtime) noexcept {
        g_PreemptionIterations = 0;
        g_PreemptionComplete = false;

        ProcessorLocal* processor = CurrentProcessor();

        if (processor != &runtime.Platform.BootstrapProcessor ||
            processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
            processor->LocalScheduler != &runtime.Scheduling.BootstrapScheduler)
            Diagnostics::Fatal("Scheduler", "preemption self-test began with invalid processor state");

        __asm__ volatile("cli" ::: "memory");

        if (!runtime.Scheduling.BootstrapScheduler.EnablePreemption())
            Diagnostics::Fatal("Scheduler", "failed to enable timer-driven preemption");

        __asm__ volatile("sti" ::: "memory");

        Diagnostics::Write("[zOS/Scheduler] Preemptive scheduling enabled.\n");
        Diagnostics::Write("[zOS/Scheduler] Quantum: 1 timer tick\n");

        const Uint64 start = runtime.Time.Clock.Nanoseconds();

        Uint64 bootstrap_iterations{};

        while (runtime.Scheduling.BootstrapScheduler.PreemptionCount() < MinimumPreemptions ||
               g_PreemptionIterations == 0) {
            ++bootstrap_iterations;

            if (runtime.Time.Clock.Nanoseconds() - start > PreemptionTimeoutNanoseconds)
                Diagnostics::Fatal("Scheduler", "timer-driven preemption self-test timed out");

            __asm__ volatile("pause");
        }

        if (bootstrap_iterations == 0 || g_PreemptionIterations == 0)
            Diagnostics::Fatal("Scheduler", "preemptive threads did not both make forward progress");

        processor = CurrentProcessor();
        if (processor != &runtime.Platform.BootstrapProcessor ||
            processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
            processor->LocalScheduler != &runtime.Scheduling.BootstrapScheduler ||
            processor->CurrentThread->OwningProcess() != &runtime.Processes.KernelProcess ||
            runtime.Scheduling.BootstrapScheduler.ReadyCount() != 1)
            Diagnostics::Fatal("Scheduler", "preemption final scheduler state is invalid");

        g_PreemptionComplete = true;

        Diagnostics::Write("[zOS/Scheduler] Both runnable threads made forward progress.\n");
        Diagnostics::Write("[zOS/Scheduler] Timer preemptions: ");
        Diagnostics::WriteDecimal(runtime.Scheduling.BootstrapScheduler.PreemptionCount());
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/Scheduler] Preemptive round-robin verified.\n");
    }
}
