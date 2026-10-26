#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Objects {
    using AccessMask = Memory::Uint64;

    inline constexpr AccessMask NoAccess{ 0 };

    [[nodiscard]] constexpr bool GrantsAccess(AccessMask granted, AccessMask required) noexcept { return (granted & required) == required; }

    class Handle final {
    public:
        constexpr Handle() noexcept = default;

        [[nodiscard]] static constexpr Handle FromValue(Memory::Uint64 value) noexcept { return Handle(value); }

        [[nodiscard]] constexpr Memory::Uint64 Value() const noexcept {
            return m_Value;
        }

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return m_Value != 0 && EncodedSlot() != 0 && Generation() != 0;
        }

        [[nodiscard]] constexpr Memory::Uint32 Slot() const noexcept {
            return IsValid() ? static_cast<Memory::Uint32>(EncodedSlot() - 1) : ~Memory::Uint32{ 0 };
        }

        [[nodiscard]] constexpr Memory::Uint32 Generation() const noexcept {
            return static_cast<Memory::Uint32>(m_Value >> 32);
        }

        [[nodiscard]] friend constexpr bool operator==(Handle left, Handle right) noexcept {
            return left.m_Value == right.m_Value;
        }

        [[nodiscard]] friend constexpr bool operator!=(Handle left, Handle right) noexcept {
            return !(left == right);
        }

    private:
        friend class HandleTable;

        constexpr explicit Handle(Memory::Uint64 value) noexcept : m_Value(value) {}

        [[nodiscard]] static constexpr Handle Encode(Memory::Uint32 slot, Memory::Uint32 generation) noexcept {
            return Handle((static_cast<Memory::Uint64>(generation) << 32) | static_cast<Memory::Uint64>(slot + 1));
        }

        [[nodiscard]] constexpr Memory::Uint32 EncodedSlot() const noexcept {
            return static_cast<Memory::Uint32>(m_Value & 0xFFFFFFFFULL);
        }

        Memory::Uint64 m_Value{};
    };
}
