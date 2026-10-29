#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Synchronization::SelfTest {
    void VerifyFoundation(KernelRuntime& runtime) noexcept;
}
