#pragma once

#include <Boot/Protocol.hpp>

namespace Zos::Kernel {
    struct KernelRuntime;
}

namespace Zos::Kernel::Initialization {
    /*
     * Performs the complete temporary bootstrap sequence.
     * 
     * On success, all permanent kernel infrastructure is owned
     * by KernelRuntime rather than the loader-provided stack.
     */
    void Bootstrap(const Boot::BootEnvironment& environment, KernelRuntime& runtime) noexcept;

    /*
     * Establishes the formal transition from bootstrap into
     * permanent kernel runtime.
     * 
     * There is currently no scheduler, so the runtime simply
     * enters the idle halt loop.
     */
    [[noreturn]] void EnterRuntime(KernelRuntime& runtime) noexcept;
}