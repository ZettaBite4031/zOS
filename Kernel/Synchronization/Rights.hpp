#pragma once

#include <Kernel/Objects/Handle.hpp>

namespace Zos::Kernel::Synchronization {
    inline constexpr Objects::AccessMask WaitRight{ 1ULL << 0 };

    inline constexpr Objects::AccessMask EventSignalRight{ 1ULL << 1 };
    inline constexpr Objects::AccessMask EventResetRight{ 1ULL << 2 };

    inline constexpr Objects::AccessMask SemaphoreReleaseRight{ 1ULL << 1 };

    inline constexpr Objects::AccessMask MutexReleaseRight{ 1ULL << 1 };

    inline constexpr Objects::AccessMask TimerArmRight{ 1ULL << 1 };
}
