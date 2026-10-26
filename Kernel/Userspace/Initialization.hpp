#pragma once

#include <Kernel/Architecture/AMD64/State.hpp>
#include <Kernel/Userspace/State.hpp>

namespace Zos::Kernel::Userspace {
    /*
     * Establishes persistent Ring 3 entry support: native SYSCALL/SYSRET and
     * the shared user-fault containment path.
     */
    void Initialize(State& userspace, Architecture::AMD64::State& architecture) noexcept;
}
