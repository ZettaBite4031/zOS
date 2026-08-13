#pragma once

#include <Boot/Protocol.hpp>

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization {
    /*
     * Performs the complete bootstrap transition and enters
     * permanent kernel runtime.
     *
     * This function never returns because the loader-provided
     * call stack is explicitly retired during bootstrap.
     */
    [[noreturn]] void Bootstrap(const Boot::BootEnvironment& environment, KernelRuntime& runtime) noexcept;
}
