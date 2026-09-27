#pragma once

#include <Boot/Protocol.hpp>

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization::MemoryBootstrap {
    void InitializePhysicalMemory(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept;
    void InitializeVirtualMemory(KernelRuntime& runtime) noexcept;
    void ActivateKernelAddressSpace(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept;
    void PromotePhysicalMemoryMetadata(KernelRuntime& runtime) noexcept;
    void ReclaimBootMemory(KernelRuntime& runtime) noexcept;
    void ReleaseBootstrapResources(KernelRuntime& runtime) noexcept;
    void InitializeKernelHeap(KernelRuntime& runtime) noexcept;
    void BindCxxAllocationRuntime(KernelRuntime& runtime) noexcept;
    void PromoteVirtualAddressMetadata(KernelRuntime& runtime);
    void PromotePageMapMetadata(KernelRuntime& runtime) noexcept;
    void RetireBootstrapMetadata(KernelRuntime& runtime) noexcept;
}
