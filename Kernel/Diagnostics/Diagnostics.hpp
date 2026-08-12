#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Diagnostics {
    void WriteChar(char value) noexcept;
    void Write(const char* message) noexcept;
    void WriteHex(Memory::Uint64 value) noexcept;
    void WriteDecimal(Memory::Uint64 value) noexcept;

    [[noreturn]] void Halt() noexcept;
    [[noreturn]] void Fatal(const char* subsystem, const char* reason) noexcept;
}
