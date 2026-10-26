#pragma once

#include <Kernel/Kernel.hpp>

namespace Zos::Kernel::Scheduling {
    /*
     * Establishes the bootstrap thread, scheduler and timer-driven preemption.
     * Scheduler-specific verification is owned and invoked here.
     */
    void Initialize(KernelRuntime& runtime) noexcept;
}
