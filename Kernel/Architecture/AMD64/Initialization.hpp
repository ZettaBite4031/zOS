#pragma once

#include <Kernel/Architecture/AMD64/State.hpp>
#include <Kernel/Memory/State.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    /*
     * Establishes CPU capability state plus kernel-owned descriptor and
     * exception/interrupt entry infrastructure.
     */
    void Initialize(State& architecture, Memory::State& memory) noexcept;
}
