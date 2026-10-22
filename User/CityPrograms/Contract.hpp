#pragma once

namespace Zos::User::CityPrograms {
    using Uint64 = unsigned long long;

    inline constexpr Uint64 StateAddress{ 0x0000000040010000ULL };
    inline constexpr Uint64 StackGuardAddress{ 0x0000000070000000ULL };
    inline constexpr Uint64 StackBaseAddress{ 0x0000000070001000ULL };
    inline constexpr Uint64 StackPageCount{ 4 };
    inline constexpr Uint64 StackTopAddress{ StackBaseAddress + StackPageCount * 4096 };

    inline constexpr Uint64 EntryMarker{ 0x5A4F534349545931ULL }; // "zOSCITY1"
    inline constexpr int ExitStatus{ 23 };

    enum class Role : Uint64 {
        Exit = 1,
        Fault = 2,
        Survivor = 3,
    };

    struct State final {
        Uint64 Marker{};
        Uint64 RoleId{};
        Uint64 ProcessId{};
        Uint64 Progress{};
        Uint64 Ready{};
        Uint64 Arm{};
    };

    static_assert(sizeof(State) == 6 * sizeof(Uint64));
}
