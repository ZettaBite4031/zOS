#pragma once

namespace Zos::Kernel { struct KernelRuntime; }
namespace Zos::Kernel::Tests::Execution {
    [[noreturn]] void FirstWorkerThreadEntry(void* context) noexcept;
    void RunCooperativeContextSwitchSelfTest(KernelRuntime& runtime) noexcept;
    void RunSchedulerSelfTest(KernelRuntime& runtime) noexcept;
    void RunPreemptionSelfTest(KernelRuntime& runtime) noexcept;
    void RunProcessAddressSpaceSelfTest(KernelRuntime& runtime) noexcept;
    void LoadFirstUserExecutable(KernelRuntime& runtime) noexcept;
    void RunRing3SelfTest(KernelRuntime& runtime) noexcept;
    void RunSystemCallSelfTest(KernelRuntime& runtime) noexcept;
    void RunUserRuntimeExitSelfTest(KernelRuntime& runtime) noexcept;
}
