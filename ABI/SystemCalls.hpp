#pragma once

namespace Zos::ABI {
    using SystemCallNumber = unsigned long long;
    static_assert(sizeof(SystemCallNumber) == 8);

    enum class NativeSystemCall : SystemCallNumber {
        GetProcessId = 0,
    };

    inline constexpr SystemCallNumber InvalidSystemCallResult{ ~SystemCallNumber{ 0 } };
}
