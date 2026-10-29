#pragma once

#include <User/Runtime/Handles.hpp>

namespace Zos::User::Runtime {
    [[nodiscard]] bool WaitForObject(Handle handle) noexcept;

    [[nodiscard]] bool SignalEvent(Handle event) noexcept;

    [[nodiscard]] bool ResetEvent(Handle event) noexcept;

    [[nodiscard]] bool ReleaseSemaphore(Handle semaphore, unsigned long long count = 1) noexcept;

    [[nodiscard]] bool ReleaseMutex(Handle mutex) noexcept;
}
