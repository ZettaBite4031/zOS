#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::SystemCalls {
    struct Context;
}

namespace Zos::Kernel::Scheduling::SystemCalls {
    [[nodiscard]] Memory::Uint64 OpenCurrentThread(::Zos::Kernel::SystemCalls::Context& context) noexcept;

    [[nodiscard]] Memory::Uint64 GetThreadIdFromHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;
}
