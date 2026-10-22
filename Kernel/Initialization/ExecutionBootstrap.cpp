#include <Kernel/Initialization/ExecutionBootstrap.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>

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

        inline constexpr Uint64 FirstUserModeStackPages{ 4 };
        inline constexpr VirtualAddress FirstUserCodeAddress{ 0x0000000040000000ULL };
        inline constexpr VirtualAddress FirstUserDataAddress{ 0x0000000040001000ULL };
        inline constexpr VirtualAddress FirstUserStackGuard{ 0x0000000070000000ULL };
        inline constexpr VirtualAddress FirstUserStackBase{ 0x0000000070001000ULL };
        inline constexpr VirtualAddress FirstUserStackTop{
            FirstUserStackBase.Value() + FirstUserModeStackPages * PageSize
        };
        inline constexpr Uint64 FirstUserModeMarker{ 0x5A4F535553455231ULL };
        inline constexpr Uint64 FirstUserSystemCallStackMarker{ 0x5A4F535359534331ULL };
        inline constexpr VirtualAddress FirstUserFaultAddress{ FirstUserStackGuard.Value() };
        volatile bool g_UserModeReady{};

        static_assert((FirstUserStackTop.Value() & 0xFULL) == 0);
        static_assert(static_cast<Uint64>(SystemCalls::NativeSystemCall::GetProcessId) == 0);

        void WriteLittleEndian64(Uint8* destination, Uint64 value) noexcept {
            for (Uint64 byte = 0; byte < 8; byte++)
                destination[byte] = static_cast<Uint8>(value >> (byte * 8));
        }

        [[nodiscard]] bool ZeroDirectMappedAllocation(const PhysicalAllocation& allocation) noexcept {
            if (!allocation.IsValid()) return false;

            const VirtualAddress direct = Layout::DirectMapAddress(allocation.Base());
            if (direct.IsNull()) return false;

            auto* bytes = reinterpret_cast<volatile Uint8*>(direct.Value());
            for (Uint64 index = 0; index < allocation.SizeBytes(); index++)
                bytes[index] = 0;

            return true;
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

    void InitializeRing3State(KernelRuntime& runtime) noexcept {
        g_UserModeReady = false;

        PageMap* address_space = runtime.FirstUserProcess.AddressSpace();
        if (address_space == nullptr || !runtime.KernelPageMap.IsActive() || address_space->IsActive())
            Diagnostics::Fatal("User", "Ring 3 setup began with invalid address-space state");

        if (runtime.KernelPageMap.IsMapped(FirstUserCodeAddress) ||
            runtime.KernelPageMap.IsMapped(FirstUserDataAddress) ||
            runtime.KernelPageMap.IsMapped(FirstUserStackBase) ||
            address_space->IsMapped(FirstUserCodeAddress) ||
            address_space->IsMapped(FirstUserDataAddress) ||
            address_space->IsMapped(FirstUserStackGuard) ||
            address_space->IsMapped(FirstUserStackBase))
            Diagnostics::Fatal("User", "Ring 3 virtual addresses are already occupied");

        if (runtime.PhysicalMemory.AllocatePage(runtime.FirstUserCodePage) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("User", "failed to allocate the first user code page");

        if (runtime.PhysicalMemory.AllocatePage(runtime.FirstUserDataPage) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("User", "failed to allocate the first user data page");

        if (runtime.PhysicalMemory.AllocateContiguous(FirstUserModeStackPages, runtime.FirstUserStackBacking) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("User", "failed to allocate the first user stack");

        if (!ZeroDirectMappedAllocation(runtime.FirstUserCodePage) ||
            !ZeroDirectMappedAllocation(runtime.FirstUserDataPage) ||
            !ZeroDirectMappedAllocation(runtime.FirstUserStackBacking))
            Diagnostics::Fatal("User", "failed to initialize user physical backing");

        const VirtualAddress code_direct = Layout::DirectMapAddress(runtime.FirstUserCodePage.Base());
        const VirtualAddress data_direct = Layout::DirectMapAddress(runtime.FirstUserDataPage.Base());
        if (code_direct.IsNull() || data_direct.IsNull())
            Diagnostics::Fatal("User", "user backing is outside the direct map");

        auto* code = reinterpret_cast<Uint8*>(code_direct.Value());

        /*
         * Tiny raw AMD64 user program:
         *
         *   movabs FirstUserDataAddress, %rbx
         *   movabs FirstUserModeMarker, %rdx
         *   mov    %rdx, (%rbx)
         *   incq   8(%rbx)
         *
         *   movabs FirstUserSystemCallStackMarker, %r12
         *   push   %r12
         *   xor    %eax, %eax               ; GetProcessId = 0
         *   syscall
         *   pop    %r12
         *   mov    %rax, 24(%rbx)           ; syscall result
         *   mov    %r12, 32(%rbx)           ; stack/register round trip
         *
         * loop:
         *   incq   8(%rbx)
         *   cmpq   $0, 16(%rbx)
         *   jne    fault
         *   pause
         *   jmp    loop
         *
         * fault:
         *   movabs FirstUserFaultAddress, %rcx
         *   mov    (%rcx), %rdx
         *   ud2
         *
         * The push before SYSCALL deliberately makes user RSP non-16-byte
         * aligned. Returning through SYSRET must restore that exact value so
         * the pop succeeds and recovers the marker.
         */

        /* movabs FirstUserDataAddress, %rbx */
        code[0] = 0x48;
        code[1] = 0xBB;
        WriteLittleEndian64(code + 2, FirstUserDataAddress.Value());

        /* movabs FirstUserModeMarker, %rdx */
        code[10] = 0x48;
        code[11] = 0xBA;
        WriteLittleEndian64(code + 12, FirstUserModeMarker);

        /* mov %rdx, (%rbx) */
        code[20] = 0x48;
        code[21] = 0x89;
        code[22] = 0x13;

        /* incq 8(%rbx) */
        code[23] = 0x48;
        code[24] = 0xFF;
        code[25] = 0x43;
        code[26] = 0x08;

        /* movabs FirstUserSystemCallStackMarker, %r12 */
        code[27] = 0x49;
        code[28] = 0xBC;
        WriteLittleEndian64(code + 29, FirstUserSystemCallStackMarker);

        /* push %r12 */
        code[37] = 0x41;
        code[38] = 0x54;

        /* xor %eax, %eax ; NativeSystemCall::GetProcessId */
        code[39] = 0x31;
        code[40] = 0xC0;

        /* syscall */
        code[41] = 0x0F;
        code[42] = 0x05;

        /* pop %r12 */
        code[43] = 0x41;
        code[44] = 0x5C;

        /* mov %rax, 24(%rbx) */
        code[45] = 0x48;
        code[46] = 0x89;
        code[47] = 0x43;
        code[48] = 0x18;

        /* mov %r12, 32(%rbx) */
        code[49] = 0x4C;
        code[50] = 0x89;
        code[51] = 0x63;
        code[52] = 0x20;

        /* loop: incq 8(%rbx) */
        code[53] = 0x48;
        code[54] = 0xFF;
        code[55] = 0x43;
        code[56] = 0x08;

        /* cmpq $0, 16(%rbx) */
        code[57] = 0x48;
        code[58] = 0x83;
        code[59] = 0x7B;
        code[60] = 0x10;
        code[61] = 0x00;

        /* jne fault */
        code[62] = 0x75;
        code[63] = 0x04;

        /* pause */
        code[64] = 0xF3;
        code[65] = 0x90;

        /* jmp loop */
        code[66] = 0xEB;
        code[67] = 0xF1;

        /* fault: movabs FirstUserFaultAddress, %rcx */
        code[68] = 0x48;
        code[69] = 0xB9;
        WriteLittleEndian64(code + 70, FirstUserFaultAddress.Value());

        /* mov (%rcx), %rdx -- expected #PF */
        code[78] = 0x48;
        code[79] = 0x8B;
        code[80] = 0x11;

        /* Safety guard if the invalid load somehow succeeds. */
        code[81] = 0x0F;
        code[82] = 0x0B;

        auto* user_data = reinterpret_cast<volatile Uint64*>(data_direct.Value());

        user_data[0] = 0; // Step 15 marker
        user_data[1] = 0; // progress counter
        user_data[2] = 0; // Step 16 fault request
        user_data[3] = 0; // Step 17 GetProcessId result
        user_data[4] = 0; // Step 17 user RSP/R12 preservation marker

        const MappingOptions user_code{
            .Access = PageAccess::Read | PageAccess::Execute | PageAccess::User,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions user_data_options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::User,
            .Cache = CachePolicy::WriteBack,
        };

        if (address_space->MapPage(FirstUserCodeAddress, runtime.FirstUserCodePage.Base(), user_code) != MappingError::Success)
            Diagnostics::Fatal("User", "failed to map the first user code page");

        if (address_space->MapPage(FirstUserDataAddress, runtime.FirstUserDataPage.Base(), user_data_options) != MappingError::Success)
            Diagnostics::Fatal("User", "failed to map the first user data page");

        if (address_space->MapRange(FirstUserStackBase, runtime.FirstUserStackBacking.Base(), FirstUserModeStackPages, user_data_options) != MappingError::Success)
            Diagnostics::Fatal("User", "failed to map the first user stack");

        if (address_space->IsMapped(FirstUserStackGuard))
            Diagnostics::Fatal("User", "first user stack guard page is mapped");

        /*
         * Configuration must become visible atomically with respect to the
         * already-running preemptive scheduler.
         */
        __asm__ volatile("cli" ::: "memory");

        if (runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("User", "user thread was not quiescent during Ring 3 configuration");
        }

        const auto user_mode_error = runtime.FirstUserThread.ConfigureUserMode(FirstUserCodeAddress, FirstUserStackTop);
        if (user_mode_error != Scheduling::ThreadUserModeError::Success) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("User", Scheduling::Thread::Describe(user_mode_error));
        }

        if (!runtime.Interrupts.BindUserFaultHandler(&Processes::HandleCurrentUserFault) ||
            !runtime.SystemCallEntry.BindUserFaultHandler(&Processes::HandleCurrentUserFault)) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("User", "failed to bind user fault containment");
        }

        g_UserModeReady = true;

        __asm__ volatile("sti" ::: "memory");

        Diagnostics::Write("[zOS/User] Ring 3 execution state prepared.\n");
        Diagnostics::Write("[zOS/User] Entry: ");
        Diagnostics::WriteHex(FirstUserCodeAddress.Value());
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/User] Stack top: ");
        Diagnostics::WriteHex(FirstUserStackTop.Value());
        Diagnostics::Write("\n");
    }

    void RunRing3SelfTest(KernelRuntime& runtime) noexcept {
        const VirtualAddress data_direct = Layout::DirectMapAddress(runtime.FirstUserDataPage.Base());
        if (data_direct.IsNull())
            Diagnostics::Fatal("User", "first user data page lost its direct-map alias");

        auto* user_data = reinterpret_cast<volatile Uint64*>(data_direct.Value());
        const Uint64 start = runtime.Clock.Nanoseconds();
        while (user_data[0] != FirstUserModeMarker || user_data[1] == 0 || !runtime.Timer.UserModeObservedInInterrupt()) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("User", "Ring 3 execution self-test timed out");
            __asm__ volatile("pause");
        }

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive() || !runtime.FirstUserThread.UserModeConfigured())
            Diagnostics::Fatal("User", "kernel state was not restored after user preemption");

        if (CurrentKernelEntryStack() != runtime.BootstrapThread.Stack()->Top())
            Diagnostics::Fatal("User", "kernel entry stack does not follow the running thread");

        Diagnostics::Write("[zOS/User] CPL3 execution observed.\n");
        Diagnostics::Write("[zOS/User] User-mode timer entry verified.\n");
        Diagnostics::Write("[zOS/User] Conditional SWAPGS path verified.\n");
        Diagnostics::Write("[zOS/User] Ring 3 architectural transition verified.\n");
    }

    void RunSystemCallSelfTest(KernelRuntime& runtime) noexcept {
        const VirtualAddress data_direct = Layout::DirectMapAddress(runtime.FirstUserDataPage.Base());
        if (data_direct.IsNull())
            Diagnostics::Fatal("Syscall", "syscall test lost the user data-page direct map");

        auto* user_data = reinterpret_cast<volatile Uint64*>(data_direct.Value());

        if (!runtime.SystemCallEntry.IsInitialized() ||
            runtime.FirstUserProcess.State() != Processes::ProcessState::Active ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready)
            Diagnostics::Fatal("Syscall", "syscall self-test began with invalid execution state");

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (user_data[3] != FirstUserProcessId ||
               user_data[4] != FirstUserSystemCallStackMarker) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("Syscall", "native syscall round-trip self-test timed out");
            __asm__ volatile("pause");
        }

        if (runtime.SystemCallEntry.DispatchCount() != 1)
            Diagnostics::Fatal("Syscall", "unexpected native syscall dispatch count");

        const Uint64 progress_before = user_data[1];
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();
        while (user_data[1] == progress_before) {
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
        Diagnostics::WriteDecimal(user_data[3]);
        Diagnostics::Write(".\n");
        Diagnostics::Write("[zOS/Syscall] User RSP and preserved register state restored.\n");
        Diagnostics::Write("[zOS/Syscall] Native syscall round trip verified.\n");
    }

    void RunUserFaultIsolationSelfTest(KernelRuntime& runtime) noexcept {
        const VirtualAddress data_direct = Layout::DirectMapAddress(runtime.FirstUserDataPage.Base());
        if (data_direct.IsNull())
            Diagnostics::Fatal("User", "user fault test lost the data-page direct map");

        auto* user_data = reinterpret_cast<volatile Uint64*>(data_direct.Value());
        if (runtime.FirstUserProcess.State() != Processes::ProcessState::Active ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Ready ||
            !runtime.FirstUserThread.UserModeConfigured())
            Diagnostics::Fatal("User", "fault-isolation test began with invalid user state");

        const Uint64 initial_terminations = runtime.BootstrapScheduler.TerminationCount();

        /*
        * The CPL3 loop polls this third qword. On its next timeslice it performs
        * a read from the intentionally unmapped stack-guard page.
        */
        user_data[2] = 1;

        const Uint64 timeout_start = runtime.Clock.Nanoseconds();
        while (runtime.FirstUserProcess.State() != Processes::ProcessState::Terminated) {
            /*
            * The process state changes on this same CPU from interrupt context.
            * Prevent the compiler from treating it as loop-invariant.
            */
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
            fault.Address != FirstUserFaultAddress ||
            fault.InstructionPointer != FirstUserCodeAddress + 78 ||
            fault.Access != UserFaultAccess::Read ||
            fault.Reason != UserFaultReason::NonPresent ||
            fault.ReservedBitViolation)
            Diagnostics::Fatal("User", "contained page-fault record is invalid");

        if (!runtime.KernelPageMap.IsActive() ||
            CurrentProcessor() != &runtime.BootstrapProcessor ||
            CurrentProcessor()->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("User", "kernel execution was not restored after user fault");

        /*
         * Prove that the kernel timer and remaining runnable kernel threads keep
         * moving after the user process is dead, while the dead user's progress
         * counter never advances again.
         */
        const Uint64 stopped_counter = user_data[1];
        const Uint64 initial_interrupts = runtime.Timer.InterruptCount();
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();
        while (runtime.Timer.InterruptCount() < initial_interrupts + 6) {
            if (runtime.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                Diagnostics::Fatal("User", "kernel scheduling stopped after user fault");
            __asm__ volatile("pause");
        }

        if (user_data[1] != stopped_counter ||
            runtime.FirstUserThread.State() != Scheduling::ThreadState::Terminated ||
            runtime.FirstUserProcess.State() != Processes::ProcessState::Terminated)
            Diagnostics::Fatal("User", "terminated user execution resumed");

        Diagnostics::Write("[zOS/User] User page fault contained.\n");
        Diagnostics::Write("[zOS/User] Faulting process terminated.\n");
        Diagnostics::Write("[zOS/User] Kernel scheduling continued after user fault.\n");
        Diagnostics::Write("[zOS/User] User fault isolation verified.\n");
    }

}
