#include <Kernel/Tests/Integration/NativeUserspace.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Executables/UserExecutableLoader.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Processes/SelfTest.hpp>

#include <User/FirstProgram/Contract.hpp>

namespace Zos::Kernel::Tests::Integration::NativeUserspace {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;
        using namespace ::Zos::User::FirstProgram;

        inline constexpr Processes::ProcessId TestProcessId{ 1 };
        inline constexpr Scheduling::ThreadId TestThreadId{ 2 };
        inline constexpr Uint64 KernelStackPages{ 8 };

        inline constexpr VirtualAddress PrivateTestAddress{ 0x0000010000000000ULL };

        inline constexpr Uint64 PrivateInitialMarker{ 0x5A4F535041434531ULL };

        inline constexpr Uint64 PrivateWorkerMarker{ 0x5A4F535041434532ULL };

        struct Fixture final {
            Executables::LoadedUserImage Image{};
            Processes::Process Process{};
            Scheduling::Thread Thread{};
        };

        constinit Fixture g_Fixture{};

        volatile bool g_ProcessWorkerObserved{};
        volatile bool g_UserModeReady{};

        [[nodiscard]] volatile ::Zos::User::FirstProgram::State* ProgramState() noexcept {
            const auto* address_space = g_Fixture.Process.AddressSpace();
            if (address_space == nullptr) return nullptr;

            const auto translation = address_space->Translate(VirtualAddress(StateAddress));
            if (!translation.Mapped ||
                !HasAccess(translation.Options.Access, PageAccess::Read) ||
                !HasAccess(translation.Options.Access, PageAccess::Write) ||
                !HasAccess(translation.Options.Access, PageAccess::User) ||
                HasAccess(translation.Options.Access, PageAccess::Execute) ||
                HasAccess(translation.Options.Access, PageAccess::Global))
                return nullptr;

            const VirtualAddress direct = Layout::DirectMapAddress(translation.Physical);
            if (direct.IsNull())
                return nullptr;

            return reinterpret_cast<volatile ::Zos::User::FirstProgram::State*>(direct.Value());
        }

        [[noreturn]] void ProcessThreadEntry(void* context) noexcept {
            if (context == nullptr)
                Diagnostics::Fatal("Process", "isolated process thread received null context");

            auto& runtime = *static_cast<KernelRuntime*>(context);
            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &g_Fixture.Thread ||
                g_Fixture.Thread.OwningProcess() != &g_Fixture.Process)
                Diagnostics::Fatal("Process", "isolated process thread ownership invalid");

            const auto* address_space = g_Fixture.Process.AddressSpace();
            if (address_space == nullptr || !address_space->IsActive() ||
                PageMap::CurrentRootTable() != address_space->RootTable())
                Diagnostics::Fatal("Process", "isolated process thread entered under the wrong CR3");

            volatile Uint64* private_value = reinterpret_cast<volatile Uint64*>(PrivateTestAddress.Value());
            if (*private_value != PrivateInitialMarker)
                Diagnostics::Fatal("Process", "isolated process private mapping contains unexpected data");

            *private_value = PrivateWorkerMarker;
            g_ProcessWorkerObserved = true;
            while (!g_UserModeReady)
                __asm__ volatile("hlt");

            g_Fixture.Thread.EnterUserMode();
        }

        void InitializeProcess(KernelRuntime& runtime) noexcept {
            const auto process_error = g_Fixture.Process.InitializeIsolated(TestProcessId, runtime.Memory.Physical, runtime.Memory.KernelPageMap);
            if (process_error != Processes::ProcessInitializationError::Success)
                Diagnostics::Fatal("Process", Processes::Process::Describe(process_error));

            Processes::SelfTest::VerifyIsolatedAddressSpace(g_Fixture.Process, runtime.Memory);
        }

        void VerifyCrossProcessScheduling(KernelRuntime& runtime) noexcept {
            g_ProcessWorkerObserved = false;

            auto* process_space = g_Fixture.Process.AddressSpace();
            if (process_space == nullptr ||
                runtime.Memory.KernelPageMap.IsMapped(PrivateTestAddress) ||
                process_space->IsMapped(PrivateTestAddress))
                Diagnostics::Fatal("Process", "private scheduling-test address is not initially isolated");

            PhysicalAllocation private_page{};

            const auto allocation_error = runtime.Memory.Physical.AllocatePage(private_page);
            if (allocation_error != PhysicalAllocationError::Success)
                Diagnostics::Fatal("Process", PhysicalMemoryManager::Describe(allocation_error));

            const VirtualAddress direct = Layout::DirectMapAddress(private_page.Base());

            if (direct.IsNull())
                Diagnostics::Fatal("Process", "private scheduling-test page is outside the direct map");

            auto* direct_value = reinterpret_cast<volatile Uint64*>(direct.Value());
            *direct_value = PrivateInitialMarker;

            const MappingOptions user_private{
                .Access = PageAccess::Read | PageAccess::Write | PageAccess::User,
                .Cache = CachePolicy::WriteBack,
            };

            const auto mapping_error = process_space->MapPage(PrivateTestAddress, private_page.Base(), user_private);
            if (mapping_error != MappingError::Success)
                Diagnostics::Fatal("Process", PageMap::Describe(mapping_error));

            const auto thread_error = g_Fixture.Thread.InitializeKernel(TestThreadId, g_Fixture.Process, &ProcessThreadEntry, &runtime,
                    runtime.Memory.Physical, runtime.Memory.KernelAddresses, runtime.Memory.KernelPageMap, KernelStackPages);

            if (thread_error != Scheduling::ThreadInitializationError::Success)
                Diagnostics::Fatal("Thread", Scheduling::Thread::Describe(thread_error));

            const Memory::KernelStack* user_stack = g_Fixture.Thread.Stack();
            if (user_stack == nullptr || !g_Fixture.Process.OwnsThread(g_Fixture.Thread) ||
                g_Fixture.Process.ThreadCount() != 1)
                Diagnostics::Fatal("Process", "isolated process thread membership is invalid");

            const auto kernel_stack_translation = runtime.Memory.KernelPageMap.Translate(user_stack->Bottom());
            const auto process_stack_translation = process_space->Translate(user_stack->Bottom());

            if (!kernel_stack_translation.Mapped || !process_stack_translation.Mapped ||
                kernel_stack_translation.Physical != process_stack_translation.Physical)
                Diagnostics::Fatal("Process", "shared kernel stack mapping is missing from process address space");

            __asm__ volatile("cli" ::: "memory");

            const auto registration_error = runtime.Scheduling.BootstrapScheduler.RegisterThread(g_Fixture.Thread);

            __asm__ volatile("sti" ::: "memory");

            if (registration_error != Scheduling::SchedulerRegistrationError::Success)
                Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(registration_error));

            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (!g_ProcessWorkerObserved) {
                if (runtime.Time.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                    Diagnostics::Fatal("Process", "cross-process address-space switch timed out");

                __asm__ volatile("pause");
            }

            if (*direct_value != PrivateWorkerMarker)
                Diagnostics::Fatal("Process", "isolated process did not modify its private mapping");

            if (!runtime.Memory.KernelPageMap.IsActive() || runtime.Memory.KernelPageMap.IsMapped(PrivateTestAddress))
                Diagnostics::Fatal("Process", "kernel address space was contaminated by private user mapping");

            if (process_space->UnmapPage(PrivateTestAddress) != MappingError::Success)
                Diagnostics::Fatal("Process", "failed to unmap scheduling-test private page");

            if (runtime.Memory.Physical.Release(private_page) != PhysicalAllocationError::Success)
                Diagnostics::Fatal("Process", "failed to release scheduling-test private page");

            Diagnostics::Write("[zOS/Process] Cross-process CR3 scheduling verified.\n");
        }

        void LoadExecutable(KernelRuntime& runtime) noexcept {
            g_UserModeReady = false;

            const Executables::EmbeddedImage embedded = Executables::FirstUserProgramImage();
            if (!embedded.IsValid())
                Diagnostics::Fatal("ELF", "embedded first user executable is unavailable");

            const Executables::UserExecutableLoadOptions options{
                .StackBase = VirtualAddress(StackBaseAddress),
                .StackPageCount = StackPageCount,
            };

            const auto load_error = Executables::UserExecutableLoader::LoadStaticElf(embedded.Data,embedded.Size,
                    g_Fixture.Process, runtime.Memory.Physical, options, g_Fixture.Image);

            if (load_error != Executables::UserExecutableLoadError::Success)
                Diagnostics::Fatal("ELF", Executables::UserExecutableLoader::Describe(load_error));

            if (!g_Fixture.Image.IsLoaded() || g_Fixture.Image.EntryPoint().IsNull() ||
                g_Fixture.Image.StackGuard() != VirtualAddress(StackGuardAddress) ||
                g_Fixture.Image.Stack().Base != VirtualAddress(StackBaseAddress) ||
                g_Fixture.Image.Stack().PageCount != StackPageCount ||
                g_Fixture.Image.StackTop() != VirtualAddress(StackTopAddress) ||
                g_Fixture.Image.SegmentCount() < 2)
                Diagnostics::Fatal("ELF", "loaded first user image state is invalid");

            bool bss_tail_observed = false;

            for (Uint64 index = 0; index < g_Fixture.Image.SegmentCount(); ++index) {
                const auto& segment = g_Fixture.Image.Segment(index);
                const PageAccess access = segment.Mapping.Access;

                if (segment.MemorySize > segment.FileSize)
                    bss_tail_observed = true;

                if (!HasAccess(access, PageAccess::Read) ||
                    !HasAccess(access, PageAccess::User) ||
                    HasAccess(access, PageAccess::Global) ||
                    (HasAccess(access, PageAccess::Write) &&
                    HasAccess(access, PageAccess::Execute)))
                    Diagnostics::Fatal("ELF", "loaded user segment permissions violate zOS policy");

                for (Uint64 page = 0; page < segment.Virtual.PageCount; ++page) {
                    if (runtime.Memory.KernelPageMap.IsMapped(segment.Virtual.Base + page * PageSize))
                        Diagnostics::Fatal("ELF", "user executable mapping leaked into the kernel address space");
                }
            }

            if (!bss_tail_observed)
                Diagnostics::Fatal("ELF", "first user executable does not exercise ELF zero-fill memory");

            for (Uint64 page = 0; page < g_Fixture.Image.Stack().PageCount; ++page) {
                if (runtime.Memory.KernelPageMap.IsMapped(g_Fixture.Image.Stack().Base + page * PageSize))
                    Diagnostics::Fatal("ELF", "user stack mapping leaked into the kernel address space");
            }

            if (g_Fixture.Process.AddressSpace()->IsMapped(g_Fixture.Image.StackGuard()) ||
                runtime.Memory.KernelPageMap.IsMapped(g_Fixture.Image.StackGuard()))
                Diagnostics::Fatal("ELF", "user stack guard page is mapped");

            volatile auto* state = ProgramState();

            if (state == nullptr)
                Diagnostics::Fatal("ELF", "first user program state mapping is unavailable");

            if (state->Marker != 0 || state->Progress != 0 || state->ExitRequest != 0 ||
                state->ProcessId != 0 || state->PreservedRegister != 0 ||
                state->StackBeforeSystemCall != 0 || state->StackAfterSystemCall != 0 ||
                state->BssZeroVerified != 0 || state->ConstructorRan != 0 || state->DestructorRan != 0)
                Diagnostics::Fatal("ELF","first user program data was not initialized to zero");

            __asm__ volatile("cli" ::: "memory");

            if (runtime.Platform.BootstrapProcessor.CurrentThread != &runtime.Scheduling.BootstrapThread ||
                g_Fixture.Thread.State() != Scheduling::ThreadState::Ready) {
                __asm__ volatile("sti" ::: "memory");
                Diagnostics::Fatal("ELF", "user thread was not quiescent during executable installation");
            }

            const auto user_mode_error = g_Fixture.Thread.ConfigureUserMode(g_Fixture.Image.EntryPoint(), g_Fixture.Image.StackTop());
            if (user_mode_error != Scheduling::ThreadUserModeError::Success) {
                __asm__ volatile("sti" ::: "memory");
                Diagnostics::Fatal("ELF", Scheduling::Thread::Describe(user_mode_error));
            }

            g_UserModeReady = true;

            __asm__ volatile("sti" ::: "memory");

            Diagnostics::Write("[zOS/Process] Native user executable loaded.\n");
            Diagnostics::Write("[zOS/User] ELF entry: ");
            Diagnostics::WriteHex(g_Fixture.Image.EntryPoint().Value());
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/User] Stack top: ");
            Diagnostics::WriteHex(g_Fixture.Image.StackTop().Value());
            Diagnostics::Write("\n");
        }

        void VerifyRing3(KernelRuntime& runtime) noexcept {
            volatile auto* state = ProgramState();
            if (state == nullptr)
                Diagnostics::Fatal("User", "first user program state mapping is unavailable");

            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (state->Marker != EntryMarker || state->Progress == 0 ||
                   state->BssZeroVerified != 1 || state->ConstructorRan != 1 ||
                   !runtime.Time.Timer.UserModeObservedInInterrupt()) {
                if (runtime.Time.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                    Diagnostics::Fatal("User", "ELF Ring 3 execution self-test timed out");

                __asm__ volatile("pause");
            }

            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
                !runtime.Memory.KernelPageMap.IsActive() || !g_Fixture.Thread.UserModeConfigured() ||
                !g_Fixture.Image.ContainsExecutableAddress(g_Fixture.Image.EntryPoint()))
                Diagnostics::Fatal("User", "kernel state was not restored after ELF userspace preemption");

            if (CurrentKernelEntryStack() != runtime.Scheduling.BootstrapThread.Stack()->Top())
                Diagnostics::Fatal("User", "kernel entry stack does not follow the running thread");

            Diagnostics::Write("[zOS/ELF] Native user executable reached its ELF entry point.\n");
            Diagnostics::Write("[zOS/ELF] BSS zero-fill verified.\n");
            Diagnostics::Write("[zOS/User] CPL3 execution observed.\n");
            Diagnostics::Write("[zOS/User] User-mode timer entry verified.\n");
            Diagnostics::Write("[zOS/User] Conditional SWAPGS path verified.\n");
            Diagnostics::Write("[zOS/User] Ring 3 architectural transition verified.\n");
        }

        void VerifySystemCalls(KernelRuntime& runtime) noexcept {
            volatile auto* state = ProgramState();
            if (state == nullptr)
                Diagnostics::Fatal("Syscall", "syscall test lost the first user program state mapping");

            if (!runtime.Userspace.SystemCalls.IsInitialized() ||
                g_Fixture.Process.State() != Processes::ProcessState::Active ||
                g_Fixture.Thread.State() != Scheduling::ThreadState::Ready)
                Diagnostics::Fatal("Syscall", "syscall self-test began with invalid execution state");

            const Uint64 start = runtime.Time.Clock.Nanoseconds();
            while (state->ProcessId != TestProcessId ||
                   state->PreservedRegister != PreservedRegisterMarker ||
                   state->StackBeforeSystemCall == 0 ||
                   state->StackAfterSystemCall == 0) {
                if (runtime.Time.Clock.Nanoseconds() - start > 1'000'000'000ULL)
                    Diagnostics::Fatal("Syscall", "native syscall round-trip self-test timed out");

                __asm__ volatile("pause");
            }

            if (state->StackBeforeSystemCall != state->StackAfterSystemCall ||
                state->StackBeforeSystemCall < g_Fixture.Image.Stack().Base.Value() ||
                state->StackBeforeSystemCall >= g_Fixture.Image.StackTop().Value())
                Diagnostics::Fatal("Syscall", "SYSRET did not restore the original user stack state");

            if (runtime.Userspace.SystemCalls.DispatchCount() != 1)
                Diagnostics::Fatal("Syscall", "unexpected native syscall dispatch count");

            const Uint64 progress_before = state->Progress;
            const Uint64 continuation_start = runtime.Time.Clock.Nanoseconds();
            while (state->Progress == progress_before) {
                if (runtime.Time.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                    Diagnostics::Fatal("Syscall", "userspace did not continue after SYSRET");

                __asm__ volatile("pause");
            }

            ProcessorLocal* processor = CurrentProcessor();
            if (processor != &runtime.Platform.BootstrapProcessor ||
                processor->CurrentThread != &runtime.Scheduling.BootstrapThread ||
                !runtime.Memory.KernelPageMap.IsActive() ||
                runtime.Platform.BootstrapProcessor.SystemCallUserStackPointer != 0 ||
                CurrentKernelEntryStack() != runtime.Scheduling.BootstrapThread.Stack()->Top())
                Diagnostics::Fatal("Syscall", "kernel state was not restored after syscall execution");

            Diagnostics::Write("[zOS/Syscall] User syscall entered on the current kernel stack.\n");
            Diagnostics::Write("[zOS/Syscall] GetProcessId returned process ");
            Diagnostics::WriteDecimal(state->ProcessId);
            Diagnostics::Write(".\n");
            Diagnostics::Write("[zOS/Syscall] User RSP and preserved register state restored.\n");
            Diagnostics::Write("[zOS/Syscall] Native syscall round trip verified.\n");
        }

        void VerifyRuntimeExit(KernelRuntime& runtime) noexcept {
            volatile auto* state = ProgramState();
            if (state == nullptr)
                Diagnostics::Fatal("Runtime", "native user runtime state mapping is unavailable");

            if (g_Fixture.Process.State() != Processes::ProcessState::Active ||
                g_Fixture.Thread.State() != Scheduling::ThreadState::Ready ||
                !g_Fixture.Thread.UserModeConfigured() ||
                g_Fixture.Process.ExitRecord().Valid ||
                g_Fixture.Process.FaultRecord().Valid ||
                state->ConstructorRan != 1 || state->DestructorRan != 0)
                Diagnostics::Fatal("Runtime", "normal-exit test began with invalid state");

            const Uint64 initial_terminations = runtime.Scheduling.BootstrapScheduler.TerminationCount();
            const Uint64 initial_syscalls = runtime.Userspace.SystemCalls.DispatchCount();
            if (initial_syscalls != 1)
                Diagnostics::Fatal("Runtime", "unexpected syscall count before process exit");

            state->ExitRequest = 1;
            const Uint64 timeout_start = runtime.Time.Clock.Nanoseconds();
            while (g_Fixture.Process.State() != Processes::ProcessState::Terminated) {
                __asm__ volatile("" ::: "memory");

                if (runtime.Time.Clock.Nanoseconds() - timeout_start > 1'000'000'000ULL)
                    Diagnostics::Fatal("Runtime", "normal process exit timed out");

                __asm__ volatile("pause");
            }

            const auto& record = g_Fixture.Process.ExitRecord();
            if (!record.Valid || record.ThreadId != TestThreadId ||
                record.Status != static_cast<Uint32>(TestExitStatus) ||
                g_Fixture.Process.FaultRecord().Valid ||
                state->DestructorRan != 1)
                Diagnostics::Fatal("Runtime", "normal process exit record is invalid");

            if (g_Fixture.Thread.State() != Scheduling::ThreadState::Terminated ||
                runtime.Scheduling.BootstrapScheduler.TerminationCount() != initial_terminations + 1 ||
                runtime.Userspace.SystemCalls.DispatchCount() != initial_syscalls + 1)
                Diagnostics::Fatal("Runtime", "native process termination did not complete");

            if (!runtime.Memory.KernelPageMap.IsActive() ||
                CurrentProcessor() != &runtime.Platform.BootstrapProcessor ||
                CurrentProcessor()->CurrentThread != &runtime.Scheduling.BootstrapThread ||
                CurrentKernelEntryStack() != runtime.Scheduling.BootstrapThread.Stack()->Top() ||
                runtime.Platform.BootstrapProcessor.SystemCallUserStackPointer != 0)
                Diagnostics::Fatal("Runtime", "kernel execution was not restored after process exit");

            const Uint64 stopped_counter = state->Progress;
            const Uint64 initial_interrupts = runtime.Time.Timer.InterruptCount();
            const Uint64 continuation_start = runtime.Time.Clock.Nanoseconds();

            while (runtime.Time.Timer.InterruptCount() < initial_interrupts + 6) {
                if (runtime.Time.Clock.Nanoseconds() - continuation_start > 500'000'000ULL)
                    Diagnostics::Fatal("Runtime", "kernel scheduling stopped after process exit");

                __asm__ volatile("pause");
            }

            if (state->Progress != stopped_counter ||
                g_Fixture.Thread.State() != Scheduling::ThreadState::Terminated ||
                g_Fixture.Process.State() != Processes::ProcessState::Terminated)
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
    }

    void Run(KernelRuntime& runtime) noexcept {
        InitializeProcess(runtime);
        VerifyCrossProcessScheduling(runtime);
        LoadExecutable(runtime);
        VerifyRing3(runtime);
        VerifySystemCalls(runtime);
        VerifyRuntimeExit(runtime);
    }
}
