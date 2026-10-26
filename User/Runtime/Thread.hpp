#pragma once

#include <User/Runtime/Handles.hpp>

namespace Zos::User::Runtime {
    using ThreadId = unsigned long long;

    inline constexpr ThreadId InvalidThreadId{
        ~ThreadId{ 0 }
    };

    [[nodiscard]] Handle OpenCurrentThread() noexcept;

    [[nodiscard]] ThreadId GetThreadId(Handle thread) noexcept;
}
