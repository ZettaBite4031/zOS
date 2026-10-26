#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Tests::Integration::NativeUserspace {
    void Run(KernelRuntime& runtime) noexcept;
}
