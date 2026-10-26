#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Tests::Integration {
    void RunBootSuite(KernelRuntime& runtime) noexcept;
}
