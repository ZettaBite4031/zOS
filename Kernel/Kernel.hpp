#pragma once

#include <Kernel/Boot/Context.hpp>
#include <Kernel/Memory/State.hpp>
#include <Kernel/Architecture/AMD64/State.hpp>
#include <Kernel/Platform/State.hpp>
#include <Kernel/Time/State.hpp>
#include <Kernel/Processes/State.hpp>
#include <Kernel/Scheduling/State.hpp>
#include <Kernel/Userspace/State.hpp>

namespace Zos::Kernel {
    /*
     * Permanent ownership root only.
     *
     * Each subsystem owns the shape and initialization of its own state.
     * KernelRuntime simply keeps those subsystem states alive for the kernel
     * lifetime; it is not a service locator for normal runtime code.
     */
    struct KernelRuntime final {
        Boot::Context Boot{};
        Memory::State Memory{};
        Architecture::AMD64::State Architecture{};
        Platform::State Platform{};
        Time::State Time{};
        Processes::State Processes{};
        Scheduling::State Scheduling{};
        Userspace::State Userspace{};
    };

    [[nodiscard]] KernelRuntime& GetKernelRuntime() noexcept;
}
