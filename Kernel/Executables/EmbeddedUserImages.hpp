#pragma once

#include <Kernel/Executables/ExecutableImageView.hpp>

namespace Zos::Kernel::Executables {
    using EmbeddedImage = ExecutableImageView;

    [[nodiscard]] EmbeddedImage FirstUserProgramImage() noexcept;

    [[nodiscard]] EmbeddedImage CityExitImage() noexcept;
    [[nodiscard]] EmbeddedImage CityFaultImage() noexcept;
    [[nodiscard]] EmbeddedImage CitySurvivorImage() noexcept;

    [[nodiscard]] EmbeddedImage ObjectTestImage() noexcept;
}
