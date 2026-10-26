#pragma once

#include <Kernel/Time/Clock.hpp>
#include <Kernel/Time/LocalApicTimer.hpp>

namespace Zos::Kernel::Time {
    struct State final {
        MonotonicClock Clock{};
        LocalApicTimer Timer{};
    };
}
