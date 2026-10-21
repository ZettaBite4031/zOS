#pragma once

#include <Kernel/Platform/ACPI/MADT.hpp>

namespace Zos::Kernel::Architecture::AMD64::LegacyPic {
    enum class QuiesceError : Memory::Uint32 {
        Success,
        InvalidDependency,
        MaskVerificationFailed,
    };

    [[nodiscard]] QuiesceError Quiesce(const Platform::ACPI::Madt& madt) noexcept;

    [[nodiscard]] const char* Describe(QuiesceError error) noexcept;
}
