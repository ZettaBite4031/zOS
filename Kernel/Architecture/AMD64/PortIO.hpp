#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    [[nodiscard]] inline Memory::Uint8 ReadPort8(Memory::Uint16 port) noexcept {
        Memory::Uint8 value{};

        __asm__ volatile(
            "inb %1, %0"
            : "=a"(value)
            : "Nd"(port)
            : "memory"
        );

        return value;
    }

    inline void WritePort8(Memory::Uint16 port, Memory::Uint8 value) noexcept {
        __asm__ volatile(
            "outb %0, %1"
            :
            : "a"(value), "Nd"(port)
            : "memory"
        );
    }
}
