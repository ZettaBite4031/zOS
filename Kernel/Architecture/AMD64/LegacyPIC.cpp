#include <Kernel/Architecture/AMD64/LegacyPIC.hpp>

#include <Kernel/Architecture/AMD64/PortIO.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Architecture::AMD64::LegacyPic {
    namespace {
        inline constexpr Memory::Uint16 MasterMaskPort{ 0x21 };
        inline constexpr Memory::Uint16 SlaveMaskPort{ 0xA1 };
        inline constexpr Memory::Uint8 AllMasked{ 0xFF };
    }

    QuiesceError Quiesce(const Platform::ACPI::Madt& madt) noexcept {
        if (!madt.IsInitialized())
            return QuiesceError::InvalidDependency;

        if (!madt.HasLegacy8259()) {
            Diagnostics::Write("[zOS/Interrupt] Legacy 8259 compatibility not present; no masking required.\n");
            return QuiesceError::Success;
        }

        /*
         * ACPI PCAT_COMPAT tells us a dual-8259 setup exists alongside
         * the APIC model. Mask every input and leave the controllers
         * otherwise untouched. zOS does not remap or use them.
         */
        WritePort8(MasterMaskPort, AllMasked);
        WritePort8(SlaveMaskPort, AllMasked);

        const Memory::Uint8 master_mask = ReadPort8(MasterMaskPort);
        const Memory::Uint8 slave_mask = ReadPort8(SlaveMaskPort);

        if (master_mask != AllMasked || slave_mask != AllMasked)
            return QuiesceError::MaskVerificationFailed;

        Diagnostics::Write("[zOS/Interrupt] Legacy 8259 interrupt delivery masked.\n");
        return QuiesceError::Success;
    }

    const char* Describe(QuiesceError error) noexcept {
        switch (error) {
        case QuiesceError::Success: return "success";
        case QuiesceError::InvalidDependency: return "MADT topology is unavailable";
        case QuiesceError::MaskVerificationFailed: return "legacy 8259 interrupt masks did not latch";
        default: return "unknown legacy 8259 quiesce error";
        }
    }
}
