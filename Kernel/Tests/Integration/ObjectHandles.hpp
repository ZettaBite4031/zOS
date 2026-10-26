#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Tests::Integration::ObjectHandles {
    void Run(KernelRuntime& runtime) noexcept;
}
