#pragma once

#include <User/Runtime/Handles.hpp>

namespace Zos::User::Runtime {
    using ProcessId = unsigned long long;

    inline constexpr ProcessId InvalidProcessId{
        ~ProcessId{ 0 }
    };

    [[nodiscard]] ProcessId GetProcessId() noexcept;

    [[nodiscard]] Handle OpenCurrentProcess() noexcept;

    [[nodiscard]] ProcessId GetProcessId(Handle process) noexcept;

    [[noreturn]] void ExitProcess(int status) noexcept;
}
