#pragma once

namespace Zos::User::ObjectTest {
    using Uint64 = unsigned long long;

    inline constexpr Uint64 ImageBase{ 0x0000000040000000ULL };
    inline constexpr Uint64 StateAddress{ 0x0000000040001000ULL };
    inline constexpr Uint64 StackGuardAddress{ 0x0000000070000000ULL };
    inline constexpr Uint64 StackBaseAddress{ 0x0000000070001000ULL };

    inline constexpr Uint64 StackPageCount{ 4 };
    inline constexpr Uint64 PageSize{ 4096 };

    inline constexpr Uint64 StackTopAddress{ StackBaseAddress + StackPageCount * PageSize };

    inline constexpr Uint64 EntryMarker{ 0x5A4F534F424A3231ULL };

    inline constexpr int TestExitStatus{ 21 };

    struct State final {
        Uint64 Marker{};
        Uint64 Passed{};
        Uint64 ProcessId{};
        Uint64 ThreadId{};

        Uint64 FirstProcessHandle{};
        Uint64 FirstThreadHandle{};
        Uint64 ReopenedProcessHandle{};

        Uint64 WrongTypeRejected{};
        Uint64 StaleHandleRejected{};
        Uint64 HandlesClosed{};
    };

    static_assert(sizeof(State) == 10 * sizeof(Uint64));
}
