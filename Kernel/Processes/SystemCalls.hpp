#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::SystemCalls {
    struct Context;
}

namespace Zos::Kernel::Processes::SystemCalls {
    [[nodiscard]] Memory::Uint64 GetProcessId(::Zos::Kernel::SystemCalls::Context& context) noexcept;

    [[nodiscard]] Memory::Uint64 OpenCurrentProcess(::Zos::Kernel::SystemCalls::Context& context) noexcept;

    [[nodiscard]] Memory::Uint64 GetProcessIdFromHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;

    [[noreturn]] void ExitProcess(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 status) noexcept;
}
