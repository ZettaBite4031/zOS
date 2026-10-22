#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Executables {
    // Borrowed immutable bytes; this view never owns or releases the image.
    // Callers must keep storage alive throughout synchronous ELF loading.
    struct ExecutableImageView final {
        const Memory::Uint8* Data{};
        Memory::Uint64 Size{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return Data != nullptr && Size != 0;
        }
    };
}
