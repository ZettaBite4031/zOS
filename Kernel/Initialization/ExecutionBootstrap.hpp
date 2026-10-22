#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization::ExecutionBootstrap {
    void InitializeKernelProcess(KernelRuntime& runtime) noexcept;
    void InitializeKernelThreads(KernelRuntime& runtime) noexcept;

    void InitializeScheduler(KernelRuntime& runtime) noexcept;

    void InitializeProcessAddressSpaces(KernelRuntime& runtime) noexcept;

}
