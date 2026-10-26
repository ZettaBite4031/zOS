#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Tests::Integration::FirstCity {
    void Run(KernelRuntime& runtime) noexcept;
}
