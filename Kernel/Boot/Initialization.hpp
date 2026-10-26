#pragma once

#include <Boot/Protocol.hpp>
#include <Kernel/Boot/Context.hpp>

namespace Zos::Kernel::Boot {
    /*
     * Validates and internalizes the firmware handoff, then prints the boot
     * summary. After this returns, permanent kernel code should consume Context
     * rather than retaining semantic dependence on BootEnvironment.
     */
    void Initialize(Context& context, const ::Zos::Boot::BootEnvironment& environment) noexcept;
}
