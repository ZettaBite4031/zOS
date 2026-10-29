#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Tests::Integration::WaitableObjects {
    void Run(KernelRuntime& runtime) noexcept;
}
