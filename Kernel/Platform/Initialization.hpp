#pragma once

#include <Kernel/Boot/Context.hpp>
#include <Kernel/Memory/State.hpp>
#include <Kernel/Architecture/AMD64/State.hpp>
#include <Kernel/Platform/State.hpp>
#include <Kernel/Time/State.hpp>

namespace Zos::Kernel::Platform {
    /*
     * Discovers the machine and brings bootstrap-processor platform facilities
     * online. Timekeeping initialization is delegated to the Time subsystem.
     */
    void Initialize(State& platform, Time::State& time, const Boot::Context& boot, Architecture::AMD64::State& architecture, Memory::State& memory) noexcept;
}
