#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::SystemCalls {
    struct Context;
}

namespace Zos::Kernel::Objects::SystemCalls {
    [[nodiscard]] Memory::Uint64 CloseHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;
}
