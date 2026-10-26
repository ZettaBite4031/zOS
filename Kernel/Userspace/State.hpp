#pragma once

#include <Kernel/Architecture/AMD64/SystemCalls.hpp>

namespace Zos::Kernel::Userspace {
    struct State final {
        Architecture::AMD64::SystemCallEntry SystemCalls{};
    };
}
