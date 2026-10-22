#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization::ExecutionBootstrap {
    void InitializeKernelProcess(KernelRuntime& runtime) noexcept;
    void InitializeKernelThreads(KernelRuntime& runtime) noexcept;
    void RunCooperativeContextSwitchSelfTest(KernelRuntime& runtime) noexcept;

    void InitializeScheduler(KernelRuntime& runtime) noexcept;
    void RunSchedulerSelfTest(KernelRuntime& runtime) noexcept;
    void RunPreemptionSelfTest(KernelRuntime& runtime) noexcept;

    void InitializeProcessAddressSpaces(KernelRuntime& runtime) noexcept;
    void RunProcessAddressSpaceSelfTest(KernelRuntime& runtime) noexcept;

    void LoadFirstUserExecutable(KernelRuntime& runtime) noexcept;
    void RunRing3SelfTest(KernelRuntime& runtime) noexcept;
    void RunSystemCallSelfTest(KernelRuntime& runtime) noexcept;
    void RunUserFaultIsolationSelfTest(KernelRuntime& runtime) noexcept;
}
