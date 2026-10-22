#pragma once

namespace Zos::Kernel { struct KernelRuntime; }
namespace Zos::Kernel::Tests::City {
    void RunCitySelfTest(KernelRuntime& runtime) noexcept;
}
