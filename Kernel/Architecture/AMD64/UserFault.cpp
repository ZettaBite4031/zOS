#include <Kernel/Architecture/AMD64/UserFault.hpp>

#include <Kernel/Architecture/AMD64/Interrupts.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    namespace {
        [[nodiscard]] Memory::Uint64 ReadFaultAddress() noexcept {
            Memory::Uint64 value{};
            __asm__ volatile(
                "mov %%cr2, %0"
                : "=r"(value)
            );
            return value;
        }
    }

    bool TranslateUserFault(const InterruptContext& context, UserFault& fault) noexcept {
        fault = {};
        fault.InstructionPointer = Memory::VirtualAddress(context.Rip);
        fault.ArchitectureCode = context.ErrorCode;

        switch (context.Vector) {
        case 6: // #UD
            fault.Kind = UserFaultKind::InvalidOpcode;
            fault.Access = UserFaultAccess::Execute;
            return true;

        case 13: // #GP
            fault.Kind = UserFaultKind::GeneralProtection;
            return true;

        case 14: { // #PF
            const Memory::Uint64 error = context.ErrorCode;

            fault.Kind = UserFaultKind::PageFault;
            fault.Address = Memory::VirtualAddress(ReadFaultAddress());
            fault.AddressValid = true;
            fault.Reason = (error & (1ULL << 0)) != 0
                ? UserFaultReason::ProtectionViolation
                : UserFaultReason::NonPresent;

            if ((error & (1ULL << 4)) != 0)
                fault.Access = UserFaultAccess::Execute;
            else if ((error & (1ULL << 1)) != 0)
                fault.Access = UserFaultAccess::Write;
            else
                fault.Access = UserFaultAccess::Read;

            fault.ReservedBitViolation = (error & (1ULL << 3)) != 0;
            return true;
        }

        default:
            return false;
        }
    }
}
