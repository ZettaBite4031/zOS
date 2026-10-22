#pragma once

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization::CityBootstrap {
    void LaunchCity(KernelRuntime& runtime) noexcept;
}
