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

        bool g_FirstWorkerEntered{};
        bool g_FirstWorkerStackVerified{};

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
            * Step 10 only expects one Bootstrap -> Worker -> Bootstrap
            * round trip. A later scheduler milestone will define what
            * happens when this worker is selected again.
            */
            Diagnostics::Fatal("Thread", "first worker thread resumed before scheduler initialization");
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

        void InitializeKernelThreads(KernelRuntime& runtime) noexcept {
            const auto bootstrap_error = runtime.BootstrapThread.InitializeBootstrap(BootstrapThreadId, runtime.PrimaryStack);
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
                runtime.BootstrapThread.State() != Scheduling::ThreadState::Running || runtime.BootstrapThread.Stack() != &runtime.PrimaryStack || runtime.BootstrapThread.OwnsStack() ||
                runtime.BootstrapThread.HasContext())
                Diagnostics::Fatal("Thread", "bootstrap-thread state or processor binding is invalid");

            Diagnostics::Write("[zOS/Thread] Current processor binding verified.\n");

            const auto worker_error = runtime.FirstWorkerThread.InitializeKernel(FirstWorkerThreadId, &FirstWorkerThreadEntry, &runtime,
                runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, FirstKernelThreadStackPages);

            if (worker_error != Scheduling::ThreadInitializationError::Success)
                Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(worker_error));

            const Memory::KernelStack* worker_stack = runtime.FirstWorkerThread.Stack();
            if (!runtime.FirstWorkerThread.IsInitialized() || runtime.FirstWorkerThread.Id() != FirstWorkerThreadId ||
                runtime.FirstWorkerThread.State() != Scheduling::ThreadState::Ready || runtime.FirstWorkerThread.Entry() != &FirstWorkerThreadEntry ||
                runtime.FirstWorkerThread.EntryContext() != &runtime || !runtime.FirstWorkerThread.OwnsStack() ||
                worker_stack == nullptr || worker_stack == &runtime.PrimaryStack)
                Diagnostics::Fatal("Thread", "first kernel thread state is invalid");

            if (!worker_stack->IsInitialized() || worker_stack->UsableSpan().PageCount != FirstKernelThreadStackPages ||
                runtime.KernelPageMap.IsMapped(worker_stack->LowerGuard()) || runtime.KernelPageMap.IsMapped(worker_stack->UpperGuard()))
                Diagnostics::Fatal("Thread", "first kernel thread stack validation failed");

            if (runtime.PrimaryStack.Contains(worker_stack->Bottom()) || worker_stack->Contains(runtime.PrimaryStack.Bottom()))
                Diagnostics::Fatal("Thread", "kernel thread stacks overlap");

            Diagnostics::Write("[zOS/Thread] Kernel thread foundation established.\n");
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
        AdvancePhase(runtime, KernelPhase::LocalApicTimerReady, KernelPhase::Runtime);

        Diagnostics::Write("[zOS/Kernel] Permanent kernel runtime entered.\n");

        /*
         * No scheduler exists yet. The BSP idles between Local APIC
         * timer interrupts with maskable interrupt delivery enabled.
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

        InitializeKernelThreads(runtime);

        AdvancePhase(runtime, KernelPhase::ProcessorLocalReady, KernelPhase::KernelThreadsReady);

        RunCooperativeContextSwitchSelfTest(runtime);

        AdvancePhase(runtime, KernelPhase::KernelThreadsReady, KernelPhase::CooperativeContextSwitchReady);

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

        AdvancePhase(runtime, KernelPhase::CooperativeContextSwitchReady, KernelPhase::LocalApicTimerReady);

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
