#include <Kernel/Initialization/ExecutionBootstrap.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Executables/UserExecutableLoader.hpp>

#include <User/FirstProgram/Contract.hpp>

namespace Zos::Kernel::Initialization::ExecutionBootstrap {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

        inline constexpr Uint64 FirstKernelThreadStackPages{ 8 };
        static_assert(FirstKernelThreadStackPages * PageSize == 32 * 1024);

        inline constexpr Scheduling::ThreadId BootstrapThreadId{ 0 };
        inline constexpr Scheduling::ThreadId FirstWorkerThreadId{ 1 };
        inline constexpr Processes::ProcessId KernelProcessId{ 0 };

        bool g_FirstWorkerEntered{};
        bool g_FirstWorkerStackVerified{};
        volatile Uint64 g_SchedulerWorkerIterations{};
        inline constexpr Uint64 SchedulerSelfTestRounds{ 4 };

        volatile Uint64 g_PreemptionWorkerIterations{};
        volatile bool g_PreemptionSelfTestComplete{};
        inline constexpr Uint64 PreemptionSelfTestMinimumSwitches{ 6 };
        inline constexpr Uint64 PreemptionSelfTestTimeoutNanoseconds{ 1'000'000'000ULL };

        inline constexpr Scheduling::ThreadId FirstUserThreadId{ 2 };
        inline constexpr Processes::ProcessId FirstUserProcessId{ 1 };
        inline constexpr Uint64 FirstUserThreadStackPages{ 8 };

        inline constexpr VirtualAddress ProcessPrivateTestAddress{ 0x0000010000000000ULL };
        inline constexpr Uint64 ProcessPrivateInitialMarker{ 0x5A4F535041434531ULL };
        inline constexpr Uint64 ProcessPrivateWorkerMarker{ 0x5A4F535041434532ULL };
        volatile bool g_ProcessAddressSpaceWorkerObserved{};

        volatile bool g_UserModeReady{};

        [[nodiscard]] volatile ::Zos::User::FirstProgram::State* FirstProgramState(KernelRuntime& runtime) noexcept {
            const auto* address_space = runtime.FirstUserProcess.AddressSpace();
            if (address_space == nullptr) return nullptr;

            const auto translation = address_space->Translate(
                VirtualAddress(::Zos::User::FirstProgram::StateAddress)
            );
            if (!translation.Mapped ||
                !HasAccess(translation.Options.Access, PageAccess::Read) ||
                !HasAccess(translation.Options.Access, PageAccess::Write) ||
                !HasAccess(translation.Options.Access, PageAccess::User) ||
                HasAccess(translation.Options.Access, PageAccess::Execute) ||
                HasAccess(translation.Options.Access, PageAccess::Global))
                return nullptr;

            const VirtualAddress direct = Layout::DirectMapAddress(translation.Physical);
            if (direct.IsNull()) return nullptr;

            return reinterpret_cast<volatile ::Zos::User::FirstProgram::State*>(direct.Value());
        }

        [[noreturn]] void FirstWorkerThreadEntry(void* context) noexcept {
            if (context == nullptr) Diagnostics::Fatal("Thread", "first worker thread received a null context");

            auto& runtime = *static_cast<KernelRuntime*>(context);
            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.FirstWorkerThread)
                Diagnostics::Fatal("Thread", "first worker thread does not own the current processor");

            Uint64 rsp{};
            __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));

            const Memory::KernelStack* stack = runtime.FirstWorkerThread.Stack();
            if (stack == nullptr || !stack->Contains(VirtualAddress(rsp)))
                Diagnostics::Fatal("Thread", "first worker thread is not executing on its own stack");

            g_FirstWorkerEntered = true;
            g_FirstWorkerStackVerified = true;

            Diagnostics::Write("[zOS/Thread] Worker thread entered on dedicated stack.\n");

            const auto switch_error = runtime.FirstWorkerThread.SwitchTo(runtime.BootstrapThread);
            if (switch_error != Scheduling::ThreadSwitchError::Success)
                Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(switch_error));

            /*
            * Returning here means Step 11's scheduler selected this thread
            * after the Step 10 bootstrap round trip.
            */
            processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.FirstWorkerThread ||
                processor->LocalScheduler == nullptr || !processor->LocalScheduler->IsInitialized())
                Diagnostics::Fatal("Scheduler", "worker thread resumed without a valid local scheduler");

            Diagnostics::Write("[zOS/Scheduler] Worker thread joined scheduler rotation.\n");

            for (;;) {
                processor = CurrentProcessor();

                if (processor == nullptr || processor->LocalScheduler == nullptr)
                    Diagnostics::Fatal("Scheduler", "worker thread lost its local scheduler");

                /*
                * The worker's existing saved context originated in the old
                * cooperative scheduler with IF clear. Its first timer-driven
                * dispatch therefore explicitly enters normal interrupt-enabled
                * preemptive execution.
                */
                if (processor->LocalScheduler->PreemptionEnabled()) {
                    __asm__ volatile("sti" ::: "memory");

                    while (!g_PreemptionSelfTestComplete) {
                        g_PreemptionWorkerIterations = g_PreemptionWorkerIterations + 1;
                        __asm__ volatile("pause");
                    }

                    /*
                    * The worker has finished its bring-up job. Keep it runnable but
                    * idle so timer preemption remains active without burning a core.
                    */
                    for (;;) __asm__ volatile("hlt");
                }

                g_SchedulerWorkerIterations = g_SchedulerWorkerIterations + 1;
                const auto yield_error = processor->LocalScheduler->Yield();
                if (yield_error != Scheduling::SchedulerYieldError::Success)
                    Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(yield_error));

                processor = CurrentProcessor();
                if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.FirstWorkerThread ||
                    processor->LocalScheduler != &runtime.BootstrapScheduler)
                    Diagnostics::Fatal("Scheduler", "worker thread resumed with invalid scheduler ownership");
            }
        }

        [[noreturn]] void FirstUserProcessThreadEntry(void* context) noexcept {
            if (context == nullptr)
                Diagnostics::Fatal("Process", "isolated process thread received null context");
            
            auto& runtime = *static_cast<KernelRuntime*>(context);
            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.FirstUserThread ||
                runtime.FirstUserThread.OwningProcess() != &runtime.FirstUserProcess)
                Diagnostics::Fatal("Process", "isolated process thread ownership invalid");

            const auto* address_space = runtime.FirstUserProcess.AddressSpace();
            if (address_space == nullptr || !address_space->IsActive() ||
                PageMap::CurrentRootTable() != address_space->RootTable())
                Diagnostics::Fatal("Process", "isolated process thread entered under the wrong CR3");

            volatile Uint64* private_value = reinterpret_cast<volatile Uint64*>(ProcessPrivateTestAddress.Value());
            if (*private_value != ProcessPrivateInitialMarker)
                Diagnostics::Fatal("Process", "isolated process private mapping contains unexpected data");

            *private_value = ProcessPrivateWorkerMarker;
            g_ProcessAddressSpaceWorkerObserved = true;

            while (!g_UserModeReady) __asm__ volatile("hlt");
            runtime.FirstUserThread.EnterUserMode();
        }
    }

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

        const auto worker_error = runtime.FirstWorkerThread.InitializeKernel(FirstWorkerThreadId, runtime.KernelProcess, &FirstWorkerThreadEntry,
            &runtime, runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, FirstKernelThreadStackPages);

        if (worker_error != Scheduling::ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(worker_error));

        const Memory::KernelStack* worker_stack = runtime.FirstWorkerThread.Stack();
        if (!runtime.FirstWorkerThread.IsInitialized() || runtime.FirstWorkerThread.Id() != FirstWorkerThreadId || runtime.FirstWorkerThread.State() != Scheduling::ThreadState::Ready ||
            runtime.FirstWorkerThread.OwningProcess() != &runtime.KernelProcess || runtime.FirstWorkerThread.Entry() != &FirstWorkerThreadEntry || runtime.FirstWorkerThread.EntryContext() != &runtime ||
            !runtime.FirstWorkerThread.OwnsStack() || worker_stack == nullptr || worker_stack == &runtime.PrimaryStack)
            Diagnostics::Fatal("Thread", "first kernel thread state is invalid");

        if (!worker_stack->IsInitialized() || worker_stack->UsableSpan().PageCount != FirstKernelThreadStackPages ||
            runtime.KernelPageMap.IsMapped(worker_stack->LowerGuard()) || runtime.KernelPageMap.IsMapped(worker_stack->UpperGuard()))
            Diagnostics::Fatal("Thread", "first kernel thread stack validation failed");

        if (runtime.PrimaryStack.Contains(worker_stack->Bottom()) || worker_stack->Contains(runtime.PrimaryStack.Bottom()))
            Diagnostics::Fatal("Thread", "kernel thread stacks overlap");
        
        Diagnostics::Write("[zOS/Thread] Kernel thread foundation established.\n");

        if (runtime.KernelProcess.ThreadCount() != 2 || !runtime.KernelProcess.OwnsThread(runtime.BootstrapThread) ||
            !runtime.KernelProcess.OwnsThread(runtime.FirstWorkerThread))
            Diagnostics::Fatal("Process", "kernel process thread membership is invalid");

        Diagnostics::Write("[zOS/Process] Kernel process thread membership verified.\n");
        Diagnostics::Write("[zOS/Process] Threads: ");
        Diagnostics::WriteDecimal(runtime.KernelProcess.ThreadCount());
        Diagnostics::Write("\n");
    }

    void RunCooperativeContextSwitchSelfTest(KernelRuntime& runtime) noexcept {
        g_FirstWorkerEntered = false;
        g_FirstWorkerStackVerified = false;

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("Thread", "bootstrap thread is not current before context-switch test");

        Uint64 rsp_before{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp_before));
        if (!runtime.PrimaryStack.Contains(VirtualAddress(rsp_before)))
            Diagnostics::Fatal("Thread", "bootstrap thread is not executing on the primary stack");

        /*
        * IF is still clear here. SwitchTo() deliberately requires that,
        * so CurrentThread cannot be observed halfway through the RSP
        * transition by a hardware interrupt.
        */
        const auto switch_error = runtime.BootstrapThread.SwitchTo(runtime.FirstWorkerThread);
        if (switch_error != Scheduling::ThreadSwitchError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(switch_error));

        /*
        * We only reach this point after Thread 1 explicitly switches
        * back to Thread 0.
        */
        if (!g_FirstWorkerEntered || !g_FirstWorkerStackVerified)
            Diagnostics::Fatal("Thread", "worker thread did not execute successfully");

        processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread ||
            runtime.BootstrapThread.State() != Scheduling::ThreadState::Running || runtime.FirstWorkerThread.State() != Scheduling::ThreadState::Ready ||
            !runtime.BootstrapThread.HasContext() || !runtime.FirstWorkerThread.HasContext())
            Diagnostics::Fatal("Thread", "thread state was not restored after cooperative switch");

        Uint64 rsp_after{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp_after));
        if (!runtime.PrimaryStack.Contains(VirtualAddress(rsp_after)))
            Diagnostics::Fatal("Thread", "bootstrap thread resumed on the wrong stack");

        Diagnostics::Write("[zOS/Thread] Cooperative context switch verified.\n");
    }

    void InitializeScheduler(KernelRuntime& runtime) noexcept {
        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("Scheduler", "bootstrap processor state is invalid during scheduler initialization");

        const auto scheduler_error = runtime.BootstrapScheduler.Initialize(runtime.BootstrapProcessor);
        if (scheduler_error != Scheduling::SchedulerInitializationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(scheduler_error));

        const auto registration_error = runtime.BootstrapScheduler.RegisterThread(runtime.FirstWorkerThread);
        if (registration_error != Scheduling::SchedulerRegistrationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(registration_error));

        if (processor->LocalScheduler != &runtime.BootstrapScheduler || runtime.BootstrapScheduler.ReadyCount() != 1)
            Diagnostics::Fatal("Scheduler", "initial ready queue state is invalid");

        Diagnostics::Write("[zOS/Scheduler] BSP scheduler initialized.\n");
        Diagnostics::Write("[zOS/Scheduler] Ready threads: ");
        Diagnostics::WriteDecimal(runtime.BootstrapScheduler.ReadyCount());
        Diagnostics::Write("\n");
    }

    void RunSchedulerSelfTest(KernelRuntime& runtime) noexcept {
        g_SchedulerWorkerIterations = 0;

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread || processor->LocalScheduler != &runtime.BootstrapScheduler)
            Diagnostics::Fatal("Scheduler", "scheduler self-test began with invalid processor state");

        const Uint64 initial_switch_count = runtime.BootstrapScheduler.SwitchCount();
        for (Uint64 round = 0; round < SchedulerSelfTestRounds; round++) {
            const auto yield_error = runtime.BootstrapScheduler.Yield();
            if (yield_error != Scheduling::SchedulerYieldError::Success)
                Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(yield_error));

            /*
            * One Bootstrap -> Worker -> Bootstrap round trip produces
            * two scheduler-selected context switches.
            */
            const Uint64 expected_worker_iterations = round + 1;
            const Uint64 expected_switches = initial_switch_count + expected_worker_iterations * 2;

            processor = CurrentProcessor();
            if (g_SchedulerWorkerIterations != expected_worker_iterations || processor != &runtime.BootstrapProcessor ||
                processor->CurrentThread != &runtime.BootstrapThread || runtime.BootstrapThread.State() != Scheduling::ThreadState::Running ||
                runtime.FirstWorkerThread.State() != Scheduling::ThreadState::Ready || runtime.BootstrapScheduler.ReadyCount() != 1 ||
                runtime.BootstrapScheduler.SwitchCount() != expected_switches)
                Diagnostics::Fatal("Scheduler", "round-robin scheduler self-test state is invalid");
        }

        Diagnostics::Write("[zOS/Scheduler] Cooperative round-robin verified.\n");
        Diagnostics::Write("[zOS/Scheduler] Context switches: ");
        Diagnostics::WriteDecimal(runtime.BootstrapScheduler.SwitchCount());
        Diagnostics::Write("\n");
    }

    void RunPreemptionSelfTest(KernelRuntime& runtime) noexcept {
        g_PreemptionWorkerIterations = 0;
        g_PreemptionSelfTestComplete = false;

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor ||
            processor->CurrentThread != &runtime.BootstrapThread ||
            processor->LocalScheduler != &runtime.BootstrapScheduler)
            Diagnostics::Fatal("Scheduler", "preemption self-test began with invalid processor state");

        /*
        * The periodic timer is already running and EnableAndVerify...
        * deliberately left IF enabled. Publish preemption atomically
        * with respect to that timer.
        */
        __asm__ volatile("cli" ::: "memory");

        if (!runtime.BootstrapScheduler.EnablePreemption())
            Diagnostics::Fatal("Scheduler", "failed to enable timer-driven preemption");

        __asm__ volatile("sti" ::: "memory");

        Diagnostics::Write("[zOS/Scheduler] Preemptive scheduling enabled.\n");
        Diagnostics::Write("[zOS/Scheduler] Quantum: 1 timer tick\n" );

        const Uint64 start = runtime.Clock.Nanoseconds();
        Uint64 bootstrap_iterations{};
        while (runtime.BootstrapScheduler.PreemptionCount() < PreemptionSelfTestMinimumSwitches || g_PreemptionWorkerIterations == 0) {
            bootstrap_iterations++;

            if (runtime.Clock.Nanoseconds() - start >
                PreemptionSelfTestTimeoutNanoseconds)
                Diagnostics::Fatal("Scheduler", "timer-driven preemption self-test timed out");

            __asm__ volatile("pause");
        }

        if (bootstrap_iterations == 0 || g_PreemptionWorkerIterations == 0)
            Diagnostics::Fatal("Scheduler", "preemptive threads did not both make forward progress");

        processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor ||
            processor->CurrentThread != &runtime.BootstrapThread ||
            processor->LocalScheduler != &runtime.BootstrapScheduler ||
            processor->CurrentThread->OwningProcess() != &runtime.KernelProcess ||
            runtime.BootstrapScheduler.ReadyCount() != 1)
            Diagnostics::Fatal("Scheduler", "preemption final scheduler state is invalid");

        g_PreemptionSelfTestComplete = true;

        Diagnostics::Write("[zOS/Scheduler] Both runnable threads made forward progress.\n");
        Diagnostics::Write("[zOS/Scheduler] Timer preemptions: ");
        Diagnostics::WriteDecimal(runtime.BootstrapScheduler.PreemptionCount());
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Scheduler] Preemptive round-robin verified.\n");
    }

    void InitializeProcessAddressSpaces(KernelRuntime& runtime) noexcept {
        const auto process_error = runtime.FirstUserProcess.InitializeIsolated(FirstUserProcessId, runtime.PhysicalMemory, runtime.KernelPageMap);
        if (process_error != Processes::ProcessInitializationError::Success)
            Diagnostics::Fatal("Process", Processes::Process::Describe(process_error));

        if (!runtime.FirstUserProcess.IsInitialized() || runtime.FirstUserProcess.Id() != FirstUserProcessId ||
            runtime.FirstUserProcess.State() != Processes::ProcessState::Active || !runtime.FirstUserProcess.OwnsAddressSpace() ||
            runtime.FirstUserProcess.AddressSpace() == nullptr || runtime.FirstUserProcess.AddressSpace()->RootTable() == runtime.KernelPageMap.RootTable())
            Diagnostics::Fatal("Process", "isolated process address-space state is invalid");

        Diagnostics::Write("[zOS/Process] Independent process address space established.\n");
    }

    void RunProcessAddressSpaceSelfTest(KernelRuntime& runtime) noexcept {
        g_ProcessAddressSpaceWorkerObserved = false;

        auto* process_space = runtime.FirstUserProcess.AddressSpace();
        if (process_space == nullptr || runtime.KernelPageMap.IsMapped(ProcessPrivateTestAddress) ||
            process_space->IsMapped(ProcessPrivateTestAddress))
            Diagnostics::Fatal("Process", "private test address is not initially isolated");

        PhysicalAllocation private_page{};
        const auto allocation_error = runtime.PhysicalMemory.AllocatePage(private_page);
        if (allocation_error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Process", PhysicalMemoryManager::Describe(allocation_error));

        const VirtualAddress direct = Layout::DirectMapAddress(private_page.Base());

        if (direct.IsNull())
            Diagnostics::Fatal("Process", "private test page is outside the direct map");

        auto* direct_value = reinterpret_cast<volatile Uint64*>(direct.Value());
        *direct_value = ProcessPrivateInitialMarker;

        const MappingOptions user_private{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::User,
            .Cache = CachePolicy::WriteBack,
        };

        const auto mapping_error = process_space->MapPage(ProcessPrivateTestAddress, private_page.Base(), user_private);
        if (mapping_error != MappingError::Success)
            Diagnostics::Fatal("Process", PageMap::Describe(mapping_error));

        const auto process_translation = process_space->Translate(ProcessPrivateTestAddress);
        if (!process_translation.Mapped || process_translation.Physical != private_page.Base() ||
            !HasAccess(process_translation.Options.Access, PageAccess::Read) ||
            !HasAccess(process_translation.Options.Access, PageAccess::Write) ||
            !HasAccess(process_translation.Options.Access, PageAccess::User) ||
            HasAccess(process_translation.Options.Access, PageAccess::Execute) ||
            HasAccess(process_translation.Options.Access, PageAccess::Global) ||
            runtime.KernelPageMap.IsMapped(ProcessPrivateTestAddress))
            Diagnostics::Fatal("Process", "private user mapping failed isolation validation");

        const auto thread_error = runtime.FirstUserThread.InitializeKernel(FirstUserThreadId, runtime.FirstUserProcess, &FirstUserProcessThreadEntry, &runtime,
                runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, FirstUserThreadStackPages);
        if (thread_error != Scheduling::ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(thread_error));

        const Memory::KernelStack* user_stack = runtime.FirstUserThread.Stack();
        if (user_stack == nullptr || !runtime.FirstUserProcess.OwnsThread(runtime.FirstUserThread) ||
            runtime.FirstUserProcess.ThreadCount() != 1)
            Diagnostics::Fatal("Process", "isolated process thread membership is invalid");

        const auto kernel_stack_translation = runtime.KernelPageMap.Translate(user_stack->Bottom());
        const auto process_stack_translation = process_space->Translate(user_stack->Bottom());
        if (!kernel_stack_translation.Mapped || !process_stack_translation.Mapped ||
            kernel_stack_translation.Physical != process_stack_translation.Physical)
            Diagnostics::Fatal("Process", "shared kernel stack mapping is missing from process address space");
        
        /*
         * RegisterThread mutates the BSP ready queue. Timer preemption is already
         * active, so exclude it while publishing the new runnable thread.
         */
        __asm__ volatile("cli" ::: "memory");

        const auto registration_error = runtime.BootstrapScheduler.RegisterThread(runtime.FirstUserThread);
        __asm__ volatile("sti" ::: "memory");
        if (registration_error != Scheduling::SchedulerRegistrationError::Success)
            Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(registration_error));

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (!g_ProcessAddressSpaceWorkerObserved) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("Process", "cross-process address-space switch timed out");
            __asm__ volatile("pause");
        }

        if (*direct_value != ProcessPrivateWorkerMarker)
            Diagnostics::Fatal("Process", "isolated process did not modify its private mapping");

        if (!runtime.KernelPageMap.IsActive() || runtime.KernelPageMap.IsMapped(ProcessPrivateTestAddress))
            Diagnostics::Fatal("Process", "kernel address space was contaminated by private user mapping");

        const auto unmap_error = process_space->UnmapPage(ProcessPrivateTestAddress);
        if (unmap_error != MappingError::Success)
            Diagnostics::Fatal("Process", PageMap::Describe(unmap_error));

        if (runtime.PhysicalMemory.Release(private_page) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Process", "failed to release private address-space test page");

        Diagnostics::Write("[zOS/Process] Private user mapping isolation verified.\n");
        Diagnostics::Write("[zOS/Process] Cross-process CR3 scheduling verified.\n");
    }

    void LoadFirstUserExecutable(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        g_UserModeReady = false;

        const Executables::EmbeddedImage embedded = Executables::FirstUserProgramImage();
        if (!embedded.IsValid())
            Diagnostics::Fatal("ELF", "embedded first user executable is unavailable");

        const Executables::UserExecutableLoadOptions load_options{
            .StackBase = VirtualAddress(StackBaseAddress),
            .StackPageCount = StackPageCount,
        };

        const auto load_error = Executables::UserExecutableLoader::LoadStaticElf(
            embedded.Data,
            embedded.Size,
            runtime.FirstUserProcess,
            runtime.PhysicalMemory,
            load_options,
            runtime.FirstUserImage
        );
        if (load_error != Executables::UserExecutableLoadError::Success)
            Diagnostics::Fatal("ELF", Executables::UserExecutableLoader::Describe(load_error));

        if (!runtime.FirstUserImage.IsLoaded() ||
            runtime.FirstUserImage.EntryPoint().IsNull() ||
            runtime.FirstUserImage.StackGuard() != VirtualAddress(StackGuardAddress) ||
            runtime.FirstUserImage.Stack().Base != VirtualAddress(StackBaseAddress) ||
            runtime.FirstUserImage.Stack().PageCount != StackPageCount ||
            runtime.FirstUserImage.StackTop() != VirtualAddress(StackTopAddress) ||
            runtime.FirstUserImage.SegmentCount() < 2)
            Diagnostics::Fatal("ELF", "loaded first user image state is invalid");

        bool bss_tail_observed = false;
        for (Uint64 index = 0; index < runtime.FirstUserImage.SegmentCount(); index++) {
            const auto& segment = runtime.FirstUserImage.Segment(index);
            const PageAccess access = segment.Mapping.Access;

            if (segment.MemorySize > segment.FileSize)
                bss_tail_observed = true;

            if (!HasAccess(access, PageAccess::Read) ||
                !HasAccess(access, PageAccess::User) ||
                HasAccess(access, PageAccess::Global) ||
                (HasAccess(access, PageAccess::Write) && HasAccess(access, PageAccess::Execute)))
                Diagnostics::Fatal("ELF", "loaded user segment permissions violate zOS policy");

            for (Uint64 page = 0; page < segment.Virtual.PageCount; page++) {
                if (runtime.KernelPageMap.IsMapped(segment.Virtual.Base + page * PageSize))
                    Diagnostics::Fatal("ELF", "user executable mapping leaked into the kernel address space");
            }
        }

        if (!bss_tail_observed)
            Diagnostics::Fatal("ELF", "first user executable does not exercise ELF zero-fill memory");

        for (Uint64 page = 0; page < runtime.FirstUserImage.Stack().PageCount; page++) {
            if (runtime.KernelPageMap.IsMapped(runtime.FirstUserImage.Stack().Base + page * PageSize))
                Diagnostics::Fatal("ELF", "user stack mapping leaked into the kernel address space");
        }

        if (runtime.FirstUserProcess.AddressSpace()->IsMapped(runtime.FirstUserImage.StackGuard()) ||
            runtime.KernelPageMap.IsMapped(runtime.FirstUserImage.StackGuard()))
            Diagnostics::Fatal("ELF", "user stack guard page is mapped");

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("ELF", "first user program state mapping is unavailable");

        if (state->Marker != 0 || state->Progress != 0 || state->FaultRequest != 0 ||
            state->ProcessId != 0 || state->PreservedRegister != 0 ||
            state->StackBeforeSystemCall != 0 || state->StackAfterSystemCall != 0 ||
            state->BssZeroVerified != 0)
            Diagnostics::Fatal("ELF", "first user program data was not initialized to zero");

        /*
         * Configure the already-created isolated thread while it is quiescent.
         * The worker entry remains in kernel mode until g_UserModeReady becomes
         * visible, so the image and fault/syscall policy become live atomically.
         */
        __asm__ volatile("cli" ::: "memory");

        if (runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("ELF", "user thread was not quiescent during executable installation");
        }

        const auto user_mode_error = runtime.FirstUserThread.ConfigureUserMode(
            runtime.FirstUserImage.EntryPoint(),
            runtime.FirstUserImage.StackTop()
        );
        if (user_mode_error != Scheduling::ThreadUserModeError::Success) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("ELF", Scheduling::Thread::Describe(user_mode_error));
        }

        if (!runtime.Interrupts.BindUserFaultHandler(&Processes::HandleCurrentUserFault) ||
            !runtime.SystemCallEntry.BindUserFaultHandler(&Processes::HandleCurrentUserFault)) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("ELF", "failed to bind user fault containment");
        }

        g_UserModeReady = true;
        __asm__ volatile("sti" ::: "memory");

        Diagnostics::Write("[zOS/Process] Native user executable loaded.\n");
        Diagnostics::Write("[zOS/User] ELF entry: ");
        Diagnostics::WriteHex(runtime.FirstUserImage.EntryPoint().Value());
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/User] Stack top: ");
        Diagnostics::WriteHex(runtime.FirstUserImage.StackTop().Value());
        Diagnostics::Write("\n");
    }

    void RunRing3SelfTest(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("User", "first user program state mapping is unavailable");

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (state->Marker != EntryMarker || state->Progress == 0 || state->BssZeroVerified != 1 ||
               !runtime.Timer.UserModeObservedInInterrupt()) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("User", "ELF Ring 3 execution self-test timed out");
            __asm__ volatile("pause");
        }

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive() || !runtime.FirstUserThread.UserModeConfigured() ||
            !runtime.FirstUserImage.ContainsExecutableAddress(runtime.FirstUserImage.EntryPoint()))
            Diagnostics::Fatal("User", "kernel state was not restored after ELF userspace preemption");

        if (CurrentKernelEntryStack() != runtime.BootstrapThread.Stack()->Top())
            Diagnostics::Fatal("User", "kernel entry stack does not follow the running thread");

        Diagnostics::Write("[zOS/ELF] Native user executable reached its ELF entry point.\n");
        Diagnostics::Write("[zOS/ELF] BSS zero-fill verified.\n");
        Diagnostics::Write("[zOS/User] CPL3 execution observed.\n");
        Diagnostics::Write("[zOS/User] User-mode timer entry verified.\n");
        Diagnostics::Write("[zOS/User] Conditional SWAPGS path verified.\n");
        Diagnostics::Write("[zOS/User] Ring 3 architectural transition verified.\n");
    }

    void RunSystemCallSelfTest(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("Syscall", "syscall test lost the first user program state mapping");

        if (!runtime.SystemCallEntry.IsInitialized() ||
            runtime.FirstUserProcess.State() != Processes::ProcessState::Active ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready)
            Diagnostics::Fatal("Syscall", "syscall self-test began with invalid execution state");

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (state->ProcessId != FirstUserProcessId ||
               state->PreservedRegister != PreservedRegisterMarker ||
               state->StackBeforeSystemCall == 0 ||
               state->StackAfterSystemCall == 0) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("Syscall", "native syscall round-trip self-test timed out");
            __asm__ volatile("pause");
        }

        if (state->StackBeforeSystemCall != state->StackAfterSystemCall ||
            state->StackBeforeSystemCall < runtime.FirstUserImage.Stack().Base.Value() ||
            state->StackBeforeSystemCall >= runtime.FirstUserImage.StackTop().Value())
            Diagnostics::Fatal("Syscall", "SYSRET did not restore the original user stack state");

        if (runtime.SystemCallEntry.DispatchCount() != 1)
            Diagnostics::Fatal("Syscall", "unexpected native syscall dispatch count");

        const Uint64 progress_before = state->Progress;
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();
        while (state->Progress == progress_before) {
            if (runtime.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                Diagnostics::Fatal("Syscall", "userspace did not continue after SYSRET");
            __asm__ volatile("pause");
        }

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor ||
            processor->CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive() ||
            runtime.BootstrapProcessor.SystemCallUserStackPointer != 0 ||
            CurrentKernelEntryStack() != runtime.BootstrapThread.Stack()->Top())
            Diagnostics::Fatal("Syscall", "kernel state was not restored after syscall execution");

        Diagnostics::Write("[zOS/Syscall] User syscall entered on the current kernel stack.\n");
        Diagnostics::Write("[zOS/Syscall] GetProcessId returned process ");
        Diagnostics::WriteDecimal(state->ProcessId);
        Diagnostics::Write(".\n");
        Diagnostics::Write("[zOS/Syscall] User RSP and preserved register state restored.\n");
        Diagnostics::Write("[zOS/Syscall] Native syscall round trip verified.\n");
    }

    void RunUserFaultIsolationSelfTest(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("User", "user fault test lost the first user program state mapping");

        if (runtime.FirstUserProcess.State() != Processes::ProcessState::Active ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready ||
            !runtime.FirstUserThread.UserModeConfigured())
            Diagnostics::Fatal("User", "fault-isolation test began with invalid user state");

        const Uint64 initial_terminations = runtime.BootstrapScheduler.TerminationCount();

        /*
         * The ELF program polls this field. On its next timeslice it reads from
         * the intentionally unmapped stack guard page and should receive #PF.
         */
        state->FaultRequest = 1;

        const Uint64 timeout_start = runtime.Clock.Nanoseconds();
        while (runtime.FirstUserProcess.State() != Processes::ProcessState::Terminated) {
            __asm__ volatile("" ::: "memory");
            if (runtime.Clock.Nanoseconds() - timeout_start > 1'000'000'000ULL)
                Diagnostics::Fatal("User", "user fault isolation self-test timed out");

            __asm__ volatile("pause");
        }

        if (runtime.FirstUserThread.State() != Scheduling::ThreadState::Terminated ||
            runtime.BootstrapScheduler.TerminationCount() != initial_terminations + 1)
            Diagnostics::Fatal("User", "faulting user thread was not removed from scheduling");

        const Processes::ProcessFaultRecord& record = runtime.FirstUserProcess.FaultRecord();
        const UserFault& fault = record.Fault;

        if (!record.Valid || record.ThreadId != FirstUserThreadId ||
            fault.Kind != UserFaultKind::PageFault || !fault.AddressValid ||
            fault.Address != runtime.FirstUserImage.StackGuard() ||
            !runtime.FirstUserImage.ContainsExecutableAddress(fault.InstructionPointer) ||
            fault.Access != UserFaultAccess::Read ||
            fault.Reason != UserFaultReason::NonPresent ||
            fault.ReservedBitViolation)
            Diagnostics::Fatal("User", "contained ELF page-fault record is invalid");

        if (!runtime.KernelPageMap.IsActive() ||
            CurrentProcessor() != &runtime.BootstrapProcessor ||
            CurrentProcessor()->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("User", "kernel execution was not restored after user fault");

        const Uint64 stopped_counter = state->Progress;
        const Uint64 initial_interrupts = runtime.Timer.InterruptCount();
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();
        while (runtime.Timer.InterruptCount() < initial_interrupts + 6) {
            if (runtime.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                Diagnostics::Fatal("User", "kernel scheduling stopped after user fault");
            __asm__ volatile("pause");
        }

        if (state->Progress != stopped_counter ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Terminated ||
            runtime.FirstUserProcess.State() != Processes::ProcessState::Terminated)
            Diagnostics::Fatal("User", "terminated user execution resumed");

        Diagnostics::Write("[zOS/User] User page fault contained.\n");
        Diagnostics::Write("[zOS/User] Faulting process terminated.\n");
        Diagnostics::Write("[zOS/User] Kernel scheduling continued after user fault.\n");
        Diagnostics::Write("[zOS/User] User fault isolation verified.\n");
    }
}
