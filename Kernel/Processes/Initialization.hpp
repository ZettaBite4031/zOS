#pragma once

#include <Kernel/Memory/State.hpp>
#include <Kernel/Processes/State.hpp>

namespace Zos::Kernel::Processes {
    /*
     * Establishes process-global kernel state. Isolated user processes are
     * created by the userspace/integration paths that actually own them.
     */
    void Initialize(State& processes, Memory::State& memory) noexcept;
}
