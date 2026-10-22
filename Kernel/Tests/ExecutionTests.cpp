// Boot-time execution regression tests. Kept callable through ExecutionBootstrap
// until test policy is independent of boot orchestration.
#include <Kernel/Tests/ExecutionTests.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Executables/UserExecutableLoader.hpp>

#include <User/FirstProgram/Contract.hpp>
#include <Kernel/Tests/Fixtures/TestRuntime.hpp>

namespace Zos::Kernel::Tests::Execution {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

        inline constexpr Uint64 FirstKernelThreadStackPages{ 8 };
        static_assert(FirstKernelThreadStackPages * PageSize == 32 * 1024);


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
            (void)runtime;
            const auto* address_space = Tests::Fixtures::Get().FirstUserProcess.AddressSpace();
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

        [[noreturn]] void FirstWorkerThreadEntryImpl(void* context) noexcept {
            if (context == nullptr) Diagnostics::Fatal("Thread", "first worker thread received a null context");

            auto& runtime = *static_cast<KernelRuntime*>(context);
            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &Tests::Fixtures::Get().FirstWorkerThread)
                Diagnostics::Fatal("Thread", "first worker thread does not own the current processor");

            Uint64 rsp{};
            __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));

            const Memory::KernelStack* stack = Tests::Fixtures::Get().FirstWorkerThread.Stack();
            if (stack == nullptr || !stack->Contains(VirtualAddress(rsp)))
                Diagnostics::Fatal("Thread", "first worker thread is not executing on its own stack");

            g_FirstWorkerEntered = true;
            g_FirstWorkerStackVerified = true;

            Diagnostics::Write("[zOS/Thread] Worker thread entered on dedicated stack.\n");

            const auto switch_error = Tests::Fixtures::Get().FirstWorkerThread.SwitchTo(runtime.BootstrapThread);
            if (switch_error != Scheduling::ThreadSwitchError::Success)
                Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(switch_error));

            /*
            * Returning here means Step 11's scheduler selected this thread
            * after the Step 10 bootstrap round trip.
            */
            processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &Tests::Fixtures::Get().FirstWorkerThread ||
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
                if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &Tests::Fixtures::Get().FirstWorkerThread ||
                    processor->LocalScheduler != &runtime.BootstrapScheduler)
                    Diagnostics::Fatal("Scheduler", "worker thread resumed with invalid scheduler ownership");
            }
        }

        [[noreturn]] void FirstUserProcessThreadEntry(void* context) noexcept {
            if (context == nullptr)
                Diagnostics::Fatal("Process", "isolated process thread received null context");
            
            auto& runtime = *static_cast<KernelRuntime*>(context);
            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &Tests::Fixtures::Get().FirstUserThread ||
                Tests::Fixtures::Get().FirstUserThread.OwningProcess() != &Tests::Fixtures::Get().FirstUserProcess)
                Diagnostics::Fatal("Process", "isolated process thread ownership invalid");

            const auto* address_space = Tests::Fixtures::Get().FirstUserProcess.AddressSpace();
            if (address_space == nullptr || !address_space->IsActive() ||
                PageMap::CurrentRootTable() != address_space->RootTable())
                Diagnostics::Fatal("Process", "isolated process thread entered under the wrong CR3");

            volatile Uint64* private_value = reinterpret_cast<volatile Uint64*>(ProcessPrivateTestAddress.Value());
            if (*private_value != ProcessPrivateInitialMarker)
                Diagnostics::Fatal("Process", "isolated process private mapping contains unexpected data");

            *private_value = ProcessPrivateWorkerMarker;
            g_ProcessAddressSpaceWorkerObserved = true;

            while (!g_UserModeReady) __asm__ volatile("hlt");
            Tests::Fixtures::Get().FirstUserThread.EnterUserMode();
        }
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
        const auto switch_error = runtime.BootstrapThread.SwitchTo(Tests::Fixtures::Get().FirstWorkerThread);
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
            runtime.BootstrapThread.State() != Scheduling::ThreadState::Running || Tests::Fixtures::Get().FirstWorkerThread.State() != Scheduling::ThreadState::Ready ||
            !runtime.BootstrapThread.HasContext() || !Tests::Fixtures::Get().FirstWorkerThread.HasContext())
            Diagnostics::Fatal("Thread", "thread state was not restored after cooperative switch");

        Uint64 rsp_after{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp_after));
        if (!runtime.PrimaryStack.Contains(VirtualAddress(rsp_after)))
            Diagnostics::Fatal("Thread", "bootstrap thread resumed on the wrong stack");

        Diagnostics::Write("[zOS/Thread] Cooperative context switch verified.\n");
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
                Tests::Fixtures::Get().FirstWorkerThread.State() != Scheduling::ThreadState::Ready || runtime.BootstrapScheduler.ReadyCount() != 1 ||
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



    void RunProcessAddressSpaceSelfTest(KernelRuntime& runtime) noexcept {
        g_ProcessAddressSpaceWorkerObserved = false;

        auto* process_space = Tests::Fixtures::Get().FirstUserProcess.AddressSpace();
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

        const auto thread_error = Tests::Fixtures::Get().FirstUserThread.InitializeKernel(FirstUserThreadId, Tests::Fixtures::Get().FirstUserProcess, &FirstUserProcessThreadEntry, &runtime,
                runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, FirstUserThreadStackPages);
        if (thread_error != Scheduling::ThreadInitializationError::Success)
            Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(thread_error));

        const Memory::KernelStack* user_stack = Tests::Fixtures::Get().FirstUserThread.Stack();
        if (user_stack == nullptr || !Tests::Fixtures::Get().FirstUserProcess.OwnsThread(Tests::Fixtures::Get().FirstUserThread) ||
            Tests::Fixtures::Get().FirstUserProcess.ThreadCount() != 1)
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

        const auto registration_error = runtime.BootstrapScheduler.RegisterThread(Tests::Fixtures::Get().FirstUserThread);
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
            Tests::Fixtures::Get().FirstUserProcess,
            runtime.PhysicalMemory,
            load_options,
            Tests::Fixtures::Get().FirstUserImage
        );
        if (load_error != Executables::UserExecutableLoadError::Success)
            Diagnostics::Fatal("ELF", Executables::UserExecutableLoader::Describe(load_error));

        if (!Tests::Fixtures::Get().FirstUserImage.IsLoaded() ||
            Tests::Fixtures::Get().FirstUserImage.EntryPoint().IsNull() ||
            Tests::Fixtures::Get().FirstUserImage.StackGuard() != VirtualAddress(StackGuardAddress) ||
            Tests::Fixtures::Get().FirstUserImage.Stack().Base != VirtualAddress(StackBaseAddress) ||
            Tests::Fixtures::Get().FirstUserImage.Stack().PageCount != StackPageCount ||
            Tests::Fixtures::Get().FirstUserImage.StackTop() != VirtualAddress(StackTopAddress) ||
            Tests::Fixtures::Get().FirstUserImage.SegmentCount() < 2)
            Diagnostics::Fatal("ELF", "loaded first user image state is invalid");

        bool bss_tail_observed = false;
        for (Uint64 index = 0; index < Tests::Fixtures::Get().FirstUserImage.SegmentCount(); index++) {
            const auto& segment = Tests::Fixtures::Get().FirstUserImage.Segment(index);
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

        for (Uint64 page = 0; page < Tests::Fixtures::Get().FirstUserImage.Stack().PageCount; page++) {
            if (runtime.KernelPageMap.IsMapped(Tests::Fixtures::Get().FirstUserImage.Stack().Base + page * PageSize))
                Diagnostics::Fatal("ELF", "user stack mapping leaked into the kernel address space");
        }

        if (Tests::Fixtures::Get().FirstUserProcess.AddressSpace()->IsMapped(Tests::Fixtures::Get().FirstUserImage.StackGuard()) ||
            runtime.KernelPageMap.IsMapped(Tests::Fixtures::Get().FirstUserImage.StackGuard()))
            Diagnostics::Fatal("ELF", "user stack guard page is mapped");

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("ELF", "first user program state mapping is unavailable");

        if (state->Marker != 0 || state->Progress != 0 || state->ExitRequest != 0 ||
            state->ProcessId != 0 || state->PreservedRegister != 0 ||
            state->StackBeforeSystemCall != 0 || state->StackAfterSystemCall != 0 ||
            state->BssZeroVerified != 0 || state->ConstructorRan != 0 ||
            state->DestructorRan != 0)
            Diagnostics::Fatal("ELF", "first user program data was not initialized to zero");

        /*
         * Configure the already-created isolated thread while it is quiescent.
         * The worker entry remains in kernel mode until g_UserModeReady becomes
         * visible, so the image and fault/syscall policy become live atomically.
         */
        __asm__ volatile("cli" ::: "memory");

        if (runtime.BootstrapProcessor.CurrentThread != &runtime.BootstrapThread ||
            Tests::Fixtures::Get().FirstUserThread.State() != Scheduling::ThreadState::Ready) {
            __asm__ volatile("sti" ::: "memory");
            Diagnostics::Fatal("ELF", "user thread was not quiescent during executable installation");
        }

        const auto user_mode_error = Tests::Fixtures::Get().FirstUserThread.ConfigureUserMode(
            Tests::Fixtures::Get().FirstUserImage.EntryPoint(),
            Tests::Fixtures::Get().FirstUserImage.StackTop()
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
        Diagnostics::WriteHex(Tests::Fixtures::Get().FirstUserImage.EntryPoint().Value());
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/User] Stack top: ");
        Diagnostics::WriteHex(Tests::Fixtures::Get().FirstUserImage.StackTop().Value());
        Diagnostics::Write("\n");
    }

    void RunRing3SelfTest(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("User", "first user program state mapping is unavailable");

        const Uint64 start = runtime.Clock.Nanoseconds();
        while (state->Marker != EntryMarker || state->Progress == 0 || state->BssZeroVerified != 1 || state->ConstructorRan != 1 ||
            !runtime.Timer.UserModeObservedInInterrupt()) {
            if (runtime.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                Diagnostics::Fatal("User", "ELF Ring 3 execution self-test timed out");
            __asm__ volatile("pause");
        }

        ProcessorLocal* processor = CurrentProcessor();
        if (processor != &runtime.BootstrapProcessor || processor->CurrentThread != &runtime.BootstrapThread ||
            !runtime.KernelPageMap.IsActive() || !Tests::Fixtures::Get().FirstUserThread.UserModeConfigured() ||
            !Tests::Fixtures::Get().FirstUserImage.ContainsExecutableAddress(Tests::Fixtures::Get().FirstUserImage.EntryPoint()))
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
            Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Active ||
            Tests::Fixtures::Get().FirstUserThread.State() != Scheduling::ThreadState::Ready)
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
            state->StackBeforeSystemCall < Tests::Fixtures::Get().FirstUserImage.Stack().Base.Value() ||
            state->StackBeforeSystemCall >= Tests::Fixtures::Get().FirstUserImage.StackTop().Value())
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

    void RunUserRuntimeExitSelfTest(KernelRuntime& runtime) noexcept {
        using namespace ::Zos::User::FirstProgram;

        volatile State* state = FirstProgramState(runtime);
        if (state == nullptr)
            Diagnostics::Fatal("Runtime", "native user runtime state mapping is unavailable");

        if (Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Active || Tests::Fixtures::Get().FirstUserThread.State() != Scheduling::ThreadState::Ready ||
            !Tests::Fixtures::Get().FirstUserThread.UserModeConfigured() || Tests::Fixtures::Get().FirstUserProcess.ExitRecord().Valid || Tests::Fixtures::Get().FirstUserProcess.FaultRecord().Valid ||
            state->ConstructorRan != 1 || state->DestructorRan != 0)
            Diagnostics::Fatal("Runtime", "normal-exit test began with invalid state");

        const Uint64 initial_terminations = runtime.BootstrapScheduler.TerminationCount();
        const Uint64 initial_syscalls = runtime.SystemCallEntry.DispatchCount();

        if (initial_syscalls != 1)
            Diagnostics::Fatal("Runtime", "unexpected syscall count before process exit");

        /*
        * The native application polls this field. Once it observes the
        * request, main() returns TestExitStatus. The user runtime runs
        * termination callbacks before invoking ExitProcess.
        */
        state->ExitRequest = 1;

        const Uint64 timeout_start = runtime.Clock.Nanoseconds();
        while (Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Terminated) {
            __asm__ volatile("" ::: "memory");

            if (runtime.Clock.Nanoseconds() - timeout_start > 1'000'000'000ULL)
                Diagnostics::Fatal("Runtime", "normal process exit timed out");

            __asm__ volatile("pause");
        }

        const Processes::ProcessExitRecord& record = Tests::Fixtures::Get().FirstUserProcess.ExitRecord();

        if (!record.Valid || record.ThreadId != FirstUserThreadId || record.Status != static_cast<Memory::Uint32>(TestExitStatus) ||
            Tests::Fixtures::Get().FirstUserProcess.FaultRecord().Valid || state->DestructorRan != 1)
            Diagnostics::Fatal("Runtime", "normal process exit record is invalid");

        if (Tests::Fixtures::Get().FirstUserThread.State() != Scheduling::ThreadState::Terminated || runtime.BootstrapScheduler.TerminationCount() != initial_terminations + 1 ||
            runtime.SystemCallEntry.DispatchCount() != initial_syscalls + 1)
            Diagnostics::Fatal("Runtime", "native process termination did not complete");

        if (!runtime.KernelPageMap.IsActive() || CurrentProcessor() != &runtime.BootstrapProcessor || CurrentProcessor()->CurrentThread != &runtime.BootstrapThread || 
            CurrentKernelEntryStack() != runtime.BootstrapThread.Stack()->Top() || runtime.BootstrapProcessor.SystemCallUserStackPointer != 0)
            Diagnostics::Fatal("Runtime", "kernel execution was not restored after process exit");

        const Uint64 stopped_counter = state->Progress;
        const Uint64 initial_interrupts = runtime.Timer.InterruptCount();
        const Uint64 continuation_start = runtime.Clock.Nanoseconds();

        while (runtime.Timer.InterruptCount() < initial_interrupts + 6) {
            if (runtime.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                Diagnostics::Fatal("Runtime", "kernel scheduling stopped after process exit");

            __asm__ volatile("pause");
        }

        if (state->Progress != stopped_counter || Tests::Fixtures::Get().FirstUserThread.State() != Scheduling::ThreadState::Terminated ||
            Tests::Fixtures::Get().FirstUserProcess.State() != Processes::ProcessState::Terminated)
            Diagnostics::Fatal("Runtime", "terminated user process resumed execution");

        Diagnostics::Write("[zOS/Runtime] Native main() returned successfully.\n");
        Diagnostics::Write("[zOS/Runtime] Static constructor and destructor verified.\n");
        Diagnostics::Write("[zOS/Runtime] Exit status: ");
        Diagnostics::WriteDecimal(record.Status);
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/Runtime] Normal process termination verified.\n");
        Diagnostics::Write("[zOS/Runtime] Kernel scheduling continued after exit.\n");
        Diagnostics::Write("[zOS/Runtime] Native zOS user runtime verified.\n");
    }

    // The bootstrap uses this fixture worker; its implementation belongs to tests.
    [[noreturn]] void FirstWorkerThreadEntry(void* context) noexcept {
        FirstWorkerThreadEntryImpl(context);
    }
}
