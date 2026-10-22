#pragma once

namespace Zos::User::Runtime {
    using ProcessId = unsigned long long;

    [[nodiscard]] ProcessId GetProcessId() noexcept;
    [[noreturn]] void ExitProcess(int status) noexcept;
}
