#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    [[noreturn]] void EnterUserMode(
        Memory::VirtualAddress entry,
        Memory::VirtualAddress stack_top
    ) noexcept;
}
