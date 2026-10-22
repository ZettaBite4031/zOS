#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel {
    enum class UserFaultKind : Memory::Uint8 {
        InvalidOpcode,
        GeneralProtection,
        PageFault,
        InvalidSystemCallState,
    };

    enum class UserFaultAccess : Memory::Uint8 {
        Unknown,
        Read,
        Write,
        Execute,
    };

    enum class UserFaultReason : Memory::Uint8 {
        Unknown,
        NonPresent,
        ProtectionViolation,
    };

    /*
     * Architecture-neutral description of a fatal userspace fault.
     *
     * ArchitectureCode preserves the raw architecture-specific error code for
     * diagnostics without requiring process policy to understand an AMD64
     * interrupt frame. Address is meaningful only when AddressValid is true.
     */
    struct UserFault final {
        UserFaultKind Kind{};
        UserFaultAccess Access{ UserFaultAccess::Unknown };
        UserFaultReason Reason{ UserFaultReason::Unknown };

        Memory::VirtualAddress Address{};
        Memory::VirtualAddress InstructionPointer{};
        Memory::Uint64 ArchitectureCode{};

        bool AddressValid{};
        bool ReservedBitViolation{};
    };

    using UserFaultHandler = void(*)(const UserFault& fault) noexcept;
}
