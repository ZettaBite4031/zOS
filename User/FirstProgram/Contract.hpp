#pragma once

namespace Zos::User::FirstProgram {
    using Uint64 = unsigned long long;

    inline constexpr Uint64 ImageBase{ 0x0000000040000000ULL };
    inline constexpr Uint64 StateAddress{ 0x0000000040001000ULL };

    inline constexpr Uint64 StackGuardAddress{ 0x0000000070000000ULL };
    inline constexpr Uint64 StackBaseAddress{ 0x0000000070001000ULL };
    inline constexpr Uint64 StackPageCount{ 4 };
    inline constexpr Uint64 PageSize{ 4096 };
    inline constexpr Uint64 StackTopAddress{ StackBaseAddress + StackPageCount * PageSize };

    inline constexpr Uint64 EntryMarker{ 0x5A4F53454C463031ULL };       // "zOSELF01"
    inline constexpr Uint64 PreservedRegisterMarker{ 0x5A4F535359535231ULL };

    struct State final {
        Uint64 Marker{};
        Uint64 Progress{};
        Uint64 FaultRequest{};
        Uint64 ProcessId{};
        Uint64 PreservedRegister{};
        Uint64 StackBeforeSystemCall{};
        Uint64 StackAfterSystemCall{};
        Uint64 BssZeroVerified{};
    };

    static_assert(sizeof(State) == 8 * sizeof(Uint64));
}
