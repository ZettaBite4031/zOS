#pragma once

#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel::Processes {
    struct State final {
        Process KernelProcess{};
    };
}
