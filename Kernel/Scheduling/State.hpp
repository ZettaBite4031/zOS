#pragma once

#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>

namespace Zos::Kernel::Scheduling {
    struct State final {
        Thread BootstrapThread{};
        Scheduler BootstrapScheduler{};
    };
}
