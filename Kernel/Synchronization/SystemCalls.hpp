#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::SystemCalls {
    struct Context;
}

namespace Zos::Kernel::Synchronization::SystemCalls {
    [[nodiscard]] Memory::Uint64 WaitForObject(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;

    [[nodiscard]] Memory::Uint64 SignalEvent(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;

    [[nodiscard]] Memory::Uint64 ResetEvent(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;

    [[nodiscard]] Memory::Uint64 ReleaseSemaphore(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle, Memory::Uint64 count) noexcept;

    [[nodiscard]] Memory::Uint64 ReleaseMutex(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept;
}
