#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Executables {
    struct EmbeddedImage final {
        const Memory::Uint8* Data{};
        Memory::Uint64 Size{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return Data != nullptr && Size != 0;
        }
    };

    [[nodiscard]] EmbeddedImage FirstUserProgramImage() noexcept;

    [[nodiscard]] EmbeddedImage CityExitImage() noexcept;
    [[nodiscard]] EmbeddedImage CityFaultImage() noexcept;
    [[nodiscard]] EmbeddedImage CitySurvivorImage() noexcept;
}
