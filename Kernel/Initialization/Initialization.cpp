#include <Kernel/Initialization/Initialization.hpp>
#include <Kernel/Initialization/MemoryBootstrap.hpp>
#include <Kernel/Initialization/SelfTests.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Architecture/AMD64/Stack.hpp>
#include <Kernel/Architecture/AMD64/LegacyPIC.hpp>

namespace Zos::Kernel::Initialization {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

        inline constexpr Uint64 PrimaryKernelStackPages{ 16 };
        inline constexpr Uint64 FirstKernelThreadStackPages{ 8 };

        static_assert(PrimaryKernelStackPages * PageSize == 64 * 1024);
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

        /*
         * Leave one unmapped guard page at 0x70000000.
         */
        inline constexpr VirtualAddress FirstUserStackGuard{ 0x0000000070000000ULL };
        inline constexpr VirtualAddress FirstUserStackBase{ 0x0000000070001000ULL };

        inline constexpr VirtualAddress FirstUserStackTop{ FirstUserStackBase.Value() + FirstUserModeStackPages * PageSize };
        inline constexpr Uint64 FirstUserModeMarker{ 0x5A4F535553455231ULL };

        volatile bool g_UserModeReady{};

        static_assert((FirstUserStackTop.Value() & 0xFULL) == 0);

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

        void AdvancePhase(KernelRuntime& runtime, KernelPhase expected, KernelPhase next) noexcept {
            if (runtime.Phase != expected)
                Diagnostics::Fatal("Startup", "kernel initialization phase ordering was violated");
            runtime.Phase = next;
        }

        void PrintSignature(Boot::Uint64 signature) noexcept {
            for (Boot::Uint32 i = 0; i < 8; i++) {
                const Boot::Uint8 byte = static_cast<Boot::Uint8>((signature >> (i * 8)) & 0xFF);
                if (byte == '\0') break;
                Diagnostics::WriteChar(static_cast<char>(byte));
            }
        }

        [[nodiscard]] bool IsPageRange(const Boot::PhysicalRange& range) noexcept {
            return range.Base != 0 && range.Size != 0 && (range.Base & (PageSize - 1)) == 0 && (range.Size & (PageSize - 1)) == 0;
        }

        void ValidateBootEnvironment(const Boot::BootEnvironment& environment) noexcept {
            if (environment.Signature != Boot::EnvironmentSignature)
                Diagnostics::Fatal("Startup", "boot environment signature mismatch");

            if (environment.Version != Boot::ProtocolVersion)
                Diagnostics::Fatal("Startup", "unsupported boot protocol version");

            if (environment.Size < sizeof(Boot::BootEnvironment))
                Diagnostics::Fatal("Startup", "boot environment structure is too small");

            if (!IsPageRange(environment.KernelImage) ||
                !IsPageRange(environment.KernelStack) ||
                !IsPageRange(environment.EnvironmentStorage) ||
                !IsPageRange(environment.MemoryMapStorage))
                Diagnostics::Fatal("Startup", "boot-owned physical ranges are not page aligned");

            if (environment.MemoryMapStorage.Base == 0 || environment.MemoryMapSize == 0 ||
                environment.MemoryMapDescriptorSize == 0 || environment.MemoryMapSize > environment.MemoryMapStorage.Size ||
                (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0)
                Diagnostics::Fatal("Startup", "firmware memory-map metadata is inconsistent");

            if (environment.AcpiRsdp == 0)
                Diagnostics::Fatal("Startup", "ACPI RSDP was not supplied");

            /*
             * Prove that this structure actually lives inside the
             * storage allocation the loader told us about;
             */
            const Uint64 environment_address = reinterpret_cast<Uint64>(&environment);
            if (environment_address < environment. EnvironmentStorage.Base ||
                environment_address - environment.EnvironmentStorage.Base > environment. EnvironmentStorage.Size - sizeof(Boot::BootEnvironment))
                Diagnostics::Fatal("Startup", "boot environment lies outside its declared storage");
        }

        void PrintBootEnvironment(const Boot::BootEnvironment& environment) noexcept {
            Diagnostics::Write("[zOS/Startup] Boot protocol signature: ");
            PrintSignature(environment.Signature);
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Startup] Boot protocol version: ");
            Diagnostics::WriteDecimal(environment.Version);
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Startup] Kernel image: ");
            Diagnostics::WriteHex(environment.KernelImage.Base);
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(environment.KernelImage.Size);
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Startup] Bootstrap stack: ");
            Diagnostics::WriteHex(environment.KernelStack.Base);
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(environment.KernelStack.Size);
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Startup] Firmware memory map: ");
            Diagnostics::WriteDecimal(environment.MemoryMapSize);
            Diagnostics::Write(" bytes, descriptor size ");
            Diagnostics::WriteDecimal(environment.MemoryMapDescriptorSize);
            Diagnostics::Write("\n");
        }

        void InitializeInterrupts(KernelRuntime& runtime) noexcept {
            const auto error = runtime.Interrupts.Initialize(runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap);
            if (error != InterruptInitializationError::Success)
                Diagnostics::Fatal("Interrupt", InterruptManager::Describe(error));

            Diagnostics::Write("[zOS/Interrupt] GDT, TSS, and IDT established.\n");

            if (!runtime.Interrupts.RunBreakpointSelfTest())
                Diagnostics::Fatal("Interrupt", "INT3 round-trip self-test failed");

            Diagnostics::Write("[zOS/Interrupt] INT3 dispatch and IRETQ self-test passed.\n");
        }

        [[nodiscard]] bool ConvertBootRange(const Boot::PhysicalRange& source, PhysicalSpan& destination) noexcept {
            if (!IsPageRange(source)) return false;
            destination = PhysicalSpan{ PhysicalAddress(source.Base), source.Size / PageSize };
            return !destination.IsEmpty();
        }

        void PreparePermanentKernelStack(KernelRuntime& runtime) noexcept {
            const auto error = runtime.PrimaryStack.Initialize(runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, PrimaryKernelStackPages);
            if (error != KernelStackInitializationError::Success)
                Diagnostics::Fatal("Stack", KernelStack::Describe(error));

            const VirtualSpan span = runtime.PrimaryStack.UsableSpan();
            Diagnostics::Write("[zOS/Stack] Permanent kernel stack: ");
            Diagnostics::WriteHex(span.Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(span.SizeBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Stack] Lower guard: ");
            Diagnostics::WriteHex(runtime.PrimaryStack.LowerGuard().Value());
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Stack] Upper guard: ");
            Diagnostics::WriteHex(runtime.PrimaryStack.UpperGuard().Value());
            Diagnostics::Write("\n");
        }

        void ValidatePermanentStackActive(KernelRuntime& runtime) noexcept {
            Uint64 rsp = 0;
            __asm__ volatile(
                "mov %%rsp, %0"
                : "=r"(rsp)
            );

            if (!runtime.PrimaryStack.Contains(VirtualAddress(rsp)))
                Diagnostics::Fatal("Stack", "RSP did not transition to the permanent kernel stack");

            if (runtime.KernelPageMap.IsMapped(runtime.PrimaryStack.LowerGuard()) ||
                runtime.KernelPageMap.IsMapped(runtime.PrimaryStack.UpperGuard()))
                Diagnostics::Fatal("Stack", "permanent kernel stack guard page is mapped");
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

            const auto mapping_error = process_space->MapPage(ProcessPrivateTestAddress, private_page.Base(),user_private );
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
            if (user_stack == nullptr || !runtime.FirstUserProcess.OwnsThread(runtime.FirstUserThread ) ||
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
             *   movabs FirstUserDataAddress, %rax
             *   movabs FirstUserModeMarker, %rdx
             *   mov    %rdx, (%rax)
             * loop:
             *   incq   8(%rax)
             *   pause
             *   jmp    loop
             *
             * It deliberately contains no syscall and no privileged instruction.
             */
            code[0] = 0x48;
            code[1] = 0xB8;
            WriteLittleEndian64(code + 2, FirstUserDataAddress.Value());

            code[10] = 0x48;
            code[11] = 0xBA;
            WriteLittleEndian64(code + 12, FirstUserModeMarker);

            code[20] = 0x48;
            code[21] = 0x89;
            code[22] = 0x10;

            code[23] = 0x48;
            code[24] = 0xFF;
            code[25] = 0x40;
            code[26] = 0x08;

            code[27] = 0xF3;
            code[28] = 0x90;

            code[29] = 0xEB;
            code[30] = 0xF8;

            auto* user_data = reinterpret_cast<volatile Uint64*>(data_direct.Value());

            user_data[0] = 0;
            user_data[1] = 0;

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

            if (CurrentKernelPrivilegeStack() != runtime.BootstrapThread.Stack()->Top())
                Diagnostics::Fatal("User", "TSS RSP0 does not follow the running kernel thread");

            Diagnostics::Write("[zOS/User] CPL3 execution observed.\n");
            Diagnostics::Write("[zOS/User] User-mode timer entry verified.\n");
            Diagnostics::Write("[zOS/User] Conditional SWAPGS path verified.\n");
            Diagnostics::Write("[zOS/User] Ring 3 architectural transition verified.\n");
        }

        void InternalizeBootContext(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept {
            if (runtime.Boot.Initialized)
                Diagnostics::Fatal("Startup", "boot context was internalized more than once");

            BootContext context{};
            if (!ConvertBootRange(environment.KernelImage, context.KernelImage) ||
                !ConvertBootRange(environment.KernelStack, context.BootstrapStack) ||
                !ConvertBootRange(environment.EnvironmentStorage, context.EnvironmentStorage) ||
                !ConvertBootRange(environment.MemoryMapStorage, context.MemoryMapStorage))
                Diagnostics::Fatal("Startup", "failed to internalize boot-owned physical ranges");

            context.AcpiRsdp = PhysicalAddress(environment.AcpiRsdp);
            if (context.AcpiRsdp.IsNull())
                Diagnostics::Fatal("Startup", "internalized ACPI RSDP is null");

            context.Initialized = true;
            runtime.Boot = context;

            /*
            * Verify all copied state while the original handoff is
            * still available.
            */
            if (runtime.Boot.KernelImage.Base != PhysicalAddress(environment.KernelImage.Base) ||
                runtime.Boot.KernelImage.SizeBytes() != environment.KernelImage.Size ||
                runtime.Boot.BootstrapStack.Base != PhysicalAddress(environment.KernelStack.Base) ||
                runtime.Boot.BootstrapStack.SizeBytes() != environment.KernelStack.Size ||
                runtime.Boot.EnvironmentStorage.Base != PhysicalAddress(environment.EnvironmentStorage.Base) ||
                runtime.Boot.EnvironmentStorage.SizeBytes() != environment.EnvironmentStorage.Size ||
                runtime.Boot.MemoryMapStorage.Base != PhysicalAddress(environment.MemoryMapStorage.Base) ||
                runtime.Boot.MemoryMapStorage.SizeBytes() != environment.MemoryMapStorage.Size ||
                runtime.Boot.AcpiRsdp != PhysicalAddress(environment.AcpiRsdp))
                Diagnostics::Fatal("Startup", "internalized boot context does not match the loader handoff");

            Diagnostics::Write("[zOS/Startup] Boot context internalized.\n");
            Diagnostics::Write("[zOS/Startup] ACPI RSDP: ");
            Diagnostics::WriteHex(runtime.Boot.AcpiRsdp.Value());
            Diagnostics::Write("\n");
        }

    }

    [[noreturn]] void EnterRuntime(KernelRuntime& runtime) noexcept {
        AdvancePhase(runtime, KernelPhase::Ring3Ready, KernelPhase::Runtime);

        Diagnostics::Write("[zOS/Kernel] Permanent kernel runtime entered.\n");

        /*
         * Preemptive scheduling is active. The bootstrap thread is idle
         * whenever it has no runtime initialization work remaining.
         */
        for (;;) __asm__ volatile("hlt");
    }

    [[noreturn]] void ContinueBootstrapOnPermanentStack(void* context) noexcept {
        if (context == nullptr)
            Diagnostics::Fatal("Stack", "permanent-stack continuation context is null");

        auto& runtime = *static_cast<KernelRuntime*>(context);
        if (runtime.Phase != KernelPhase::PermanentStackPrepared)
            Diagnostics::Fatal("Startup", "permanent-state continuation phase is invalid");

        ValidatePermanentStackActive(runtime);

        AdvancePhase(runtime, KernelPhase::PermanentStackPrepared, KernelPhase::PermanentStackActive);
        Diagnostics::Write("[zOS/Stack] Permanent kernel stack active.\n");

        /*
         * We are now executing entirely from the new stack and use only
         * the internalized BootContext. The original BootEnvironment,
         * loader stack, and firmware-map storage are no longer live.
         */
        MemoryBootstrap::ReleaseBootstrapResources(runtime);
        if (!runtime.PhysicalMemory.AreBootstrapResourcesReleased())
            Diagnostics::Fatal("Memory", "bootstrap resources remain reserved");

        if (!runtime.Boot.BootstrapStack.IsEmpty() ||
            !runtime.Boot.EnvironmentStorage.IsEmpty() ||
            !runtime.Boot.MemoryMapStorage.IsEmpty())
            Diagnostics::Fatal("Startup", "released bootstrap ranges remain in BootContext");

        AdvancePhase(runtime, KernelPhase::PermanentStackActive, KernelPhase::BootstrapResourcesReleased);

        MemoryBootstrap::InitializeKernelHeap(runtime);
        SelfTests::RunKernelHeapSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::BootstrapResourcesReleased, KernelPhase::KernelHeapReady);

        /*
         * The raw heap is independently proven before the language runtime is
         * allowed to depend on it.
         */
        MemoryBootstrap::BindCxxAllocationRuntime(runtime);
        SelfTests::RunCxxAllocationSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::KernelHeapReady, KernelPhase::CxxAllocationReady);

        MemoryBootstrap::PromoteVirtualAddressMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::CxxAllocationReady, KernelPhase::VirtualAddressMetadataPromoted);

        MemoryBootstrap::PromotePageMapMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::VirtualAddressMetadataPromoted, KernelPhase::PageMapMetadataPromoted);

        MemoryBootstrap::RetireBootstrapMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::PageMapMetadataPromoted, KernelPhase::BootstrapMetadataRetired);

        SelfTests::RunPermanentMemorySelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::BootstrapMetadataRetired, KernelPhase::BootstrapComplete);
        Diagnostics::Write("[zOS/Startup] Bootstrap infrastructure complete.\n");

        const auto acpi_error = runtime.Acpi.Initialize(runtime.Boot.AcpiRsdp);
        if (acpi_error != Platform::ACPI::InitializationError::Success)
            Diagnostics::Fatal("ACPI", Platform::ACPI::TableDirectory::Describe(acpi_error));

        AdvancePhase(runtime, KernelPhase::BootstrapComplete, KernelPhase::AcpiReady);

        const auto madt_error = runtime.Madt.Initialize(runtime.Acpi);
        if (madt_error != Platform::ACPI::MadtInitializationError::Success) 
            Diagnostics::Fatal("ACPI", Platform::ACPI::Madt::Describe(madt_error));

        AdvancePhase(runtime, KernelPhase::AcpiReady, KernelPhase::InterruptTopologyReady);

        const auto cpu_error = runtime.Cpu.Initialize();
        if (cpu_error != Architecture::AMD64::CpuInitializationError::Success)
            Diagnostics::Fatal("CPU", Architecture::AMD64::CpuInfo::Describe(cpu_error));

        AdvancePhase(runtime, KernelPhase::InterruptTopologyReady, KernelPhase::CpuCapabilitiesReady);

        const auto legacy_pic_error = LegacyPic::Quiesce(runtime.Madt);
        if (legacy_pic_error != LegacyPic::QuiesceError::Success) 
            Diagnostics::Fatal("Interrupt", LegacyPic::Describe(legacy_pic_error));

        AdvancePhase(runtime, KernelPhase::CpuCapabilitiesReady, KernelPhase::LegacyInterruptsQuiesced);

        const auto apic_error = runtime.Apic.Initialize(runtime.Cpu, runtime.Madt, runtime.Interrupts, runtime.KernelMmioAddresses, runtime.KernelPageMap);
        if (apic_error != Architecture::AMD64::LocalApicInitializationError::Success)
            Diagnostics::Fatal("APIC", Architecture::AMD64::LocalApic::Describe(apic_error));

        AdvancePhase(runtime, KernelPhase::LegacyInterruptsQuiesced, KernelPhase::LocalApicReady);

        if (!runtime.Interrupts.BindLocalApic(runtime.Apic))
            Diagnostics::Fatal("Interrupt", "failed to bind Local APIC acknowledgement");

        if (!runtime.Interrupts.RunVectorPolicySelfTest())
            Diagnostics::Fatal("Interrupt", "interrupt vector policy self-test failed");

        Diagnostics::Write("[zOS/Interrupt] Vector policy and Local APIC acknowledgement established.\n");

        AdvancePhase(runtime, KernelPhase::LocalApicReady, KernelPhase::InterruptPolicyReady);

        const auto hpet_error = runtime.Hpet.Initialize(runtime.Acpi, runtime.KernelMmioAddresses, runtime.KernelPageMap);
        if (hpet_error != Platform::HpetInitializationError::Success)
            Diagnostics::Fatal("HPET", Platform::Hpet::Describe(hpet_error));

        const auto clock_error = runtime.Clock.Initialize(runtime.Cpu, runtime.Hpet);
        if (clock_error != Time::MonotonicClockInitializationError::Success)
            Diagnostics::Fatal("Time", Time::MonotonicClock::Describe(clock_error));

        AdvancePhase(runtime, KernelPhase::InterruptPolicyReady, KernelPhase::MonotonicClockReady );

        const auto processor_error = runtime.BootstrapProcessor.Initialize(0, runtime.Apic, true);
        if (processor_error != Architecture::AMD64::ProcessorLocalInitializationError::Success)
            Diagnostics::Fatal("CPU", Architecture::AMD64::ProcessorLocal::Describe(processor_error));

        AdvancePhase(runtime, KernelPhase::MonotonicClockReady, KernelPhase::ProcessorLocalReady);

        InitializeKernelProcess(runtime);

        AdvancePhase(runtime, KernelPhase::ProcessorLocalReady, KernelPhase::KernelProcessReady);

        InitializeKernelThreads(runtime);

        AdvancePhase(runtime, KernelPhase::KernelProcessReady, KernelPhase::KernelThreadsReady);

        RunCooperativeContextSwitchSelfTest(runtime);

        AdvancePhase(runtime, KernelPhase::KernelThreadsReady, KernelPhase::CooperativeContextSwitchReady);

        InitializeScheduler(runtime);
        RunSchedulerSelfTest(runtime);

        ProcessorLocal* processor = CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread == nullptr || processor->CurrentThread->OwningProcess() != &runtime.KernelProcess)
            Diagnostics::Fatal("Process", "current thread lost kernel process ownership");

        AdvancePhase(runtime, KernelPhase::CooperativeContextSwitchReady, KernelPhase::SchedulerReady);

        const auto timer_error = runtime.Timer.Initialize(runtime.Apic, runtime.Interrupts, runtime.Clock, runtime.BootstrapProcessor);
        if (timer_error != Time::LocalApicTimerInitializationError::Success)
            Diagnostics::Fatal("Timer", Time::LocalApicTimer::Describe(timer_error));

        if (!runtime.Timer.EnableAndVerifyInterruptDelivery())
            Diagnostics::Fatal("Timer", "Local APIC timer interrupt delivery validation failed");

        if (!runtime.Timer.ProcessorLocalObservedInInterrupt())
            Diagnostics::Fatal("CPU", "processor-local state was not observed in interrupt context");

        if (CurrentProcessor()->CurrentThread != &runtime.BootstrapThread)
            Diagnostics::Fatal("Thread", "current thread changed unexpectedly during interrupt delivery");

        Diagnostics::Write("[zOS/CPU] Processor-local interrupt access verified.\n");

        AdvancePhase(runtime, KernelPhase::SchedulerReady, KernelPhase::LocalApicTimerReady);

        RunPreemptionSelfTest(runtime);

        AdvancePhase(runtime, KernelPhase::LocalApicTimerReady, KernelPhase::PreemptionReady);

        InitializeProcessAddressSpaces(runtime);
        RunProcessAddressSpaceSelfTest(runtime);

        AdvancePhase(runtime, KernelPhase::PreemptionReady, KernelPhase::ProcessAddressSpacesReady);

        InitializeRing3State(runtime);
        RunRing3SelfTest(runtime);

        AdvancePhase(runtime, KernelPhase::ProcessAddressSpacesReady, KernelPhase::Ring3Ready);

        EnterRuntime(runtime);
    }

    void Bootstrap(const Boot::BootEnvironment& environment, KernelRuntime& runtime) noexcept {
        if (runtime.Phase != KernelPhase::Entry)
            Diagnostics::Fatal("Startup", "Kernel bootstrap was entered more than once");

        ValidateBootEnvironment(environment);
        PrintBootEnvironment(environment);

        AdvancePhase(runtime, KernelPhase::Entry, KernelPhase::BootEnvironmentValidated);
        Diagnostics::Write("[zOS/Startup] Firmware handoff validated.\n");

        MemoryBootstrap::InitializePhysicalMemory(runtime, environment);
        AdvancePhase(runtime, KernelPhase::BootEnvironmentValidated, KernelPhase::PhysicalMemoryReady);

        MemoryBootstrap::InitializeVirtualMemory(runtime);
        SelfTests::RunVirtualMemorySelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::PhysicalMemoryReady, KernelPhase::VirtualMemoryReady);
        Diagnostics::Write("[zOS/Startup] Virtual-memory infrastructure established.\n");

        MemoryBootstrap::ActivateKernelAddressSpace(runtime, environment);
        AdvancePhase(runtime, KernelPhase::VirtualMemoryReady, KernelPhase::AddressSpaceActive);

        /*
         * The first zOS-owned page map intentionally carried the PMM's low
         * metadata alias through CR3 activation. The permanent direct map is
         * now authoritative, so detach the PMM from that bootstrap mapping
         * before bringing up additional runtime infrastructure.
         */
        MemoryBootstrap::PromotePhysicalMemoryMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::AddressSpaceActive, KernelPhase::PhysicalMemoryMetadataPromoted);

        InitializeInterrupts(runtime);
        AdvancePhase(runtime, KernelPhase::PhysicalMemoryMetadataPromoted, KernelPhase::InterruptsReady);

        /*
         * ExitBootServices has already occurred in the loader and zOS now
         * owns its page tabels and exception infrastructure.
         *
         * Pages still classified DeferredBoot are no longer required by
         * any current bootstrap dependency. Explicitly reserved handoff
         * ranges remain Reserved and are therefore unaffected.
         */
        MemoryBootstrap::ReclaimBootMemory(runtime);
        SelfTests::RunBootMemoryReclamationSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::InterruptsReady, KernelPhase::BootMemoryReclaimed);

        /*
         * Copy every remaining handoff fact into permanent
         * kernel-owned storage.
         *
         * After this call no persistent subsystem has any reason
         * to retain BootEnvironment itself.
         */
        InternalizeBootContext(runtime, environment);
        AdvancePhase(runtime, KernelPhase::BootMemoryReclaimed, KernelPhase::BootContextInternalized);

        /*
         * Everything needed from BootEnvironment now exists in
         * KernelRuntime::Boot.
         *
         * Prepare the permanent VMM-owned stack while the loader stack is
         * still valid.
         */
        PreparePermanentKernelStack(runtime);
        AdvancePhase(runtime, KernelPhase::BootContextInternalized, KernelPhase::PermanentStackPrepared);
        Diagnostics::Write("[zOS/Stack] Switching away from loader-provided stack.\n");

        /*
         * This call NEVER returns.
         *
         * Returning would require touching the caller frame on the loader
         * stack, which the continuation will unmap and return to this PMM.
         */
        SwitchToPermanentKernelStack(runtime.PrimaryStack.Top().Value(), &ContinueBootstrapOnPermanentStack, &runtime);

        __builtin_unreachable();
    }
}
