#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;

    namespace Memory {
        class PhysicalMemoryManager;
    }
}

namespace Zos::Kernel::Initialization::SelfTests {
    void RunPhysicalMemorySelfTest(Memory::PhysicalMemoryManager& manager) noexcept;
    void RunVirtualMemorySelfTest(KernelRuntime& runtime);
    void RunBootMemoryReclamationSelfTest(KernelRuntime& runtime) noexcept;
    void RunKernelHeapSelfTest(KernelRuntime& runtime) noexcept;
    void RunCxxAllocationSelfTest(KernelRuntime& runtime) noexcept;
    void RunPermanentMemorySelfTest(KernelRuntime& runtime) noexcept;
}
