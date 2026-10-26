#pragma once

namespace Zos::ABI {
    using SystemCallNumber = unsigned long long;
    static_assert(sizeof(SystemCallNumber) == 8);

    enum class NativeSystemCall : SystemCallNumber {
        GetProcessId = 0,
        ExitProcess = 1,

        OpenCurrentProcess = 2,
        OpenCurrentThread = 3,
        CloseHandle = 4,
        GetProcessIdFromHandle = 5,
        GetThreadIdFromHandle = 6,
    };

    inline constexpr SystemCallNumber InvalidSystemCallResult{
        ~SystemCallNumber{ 0 }
    };
}
