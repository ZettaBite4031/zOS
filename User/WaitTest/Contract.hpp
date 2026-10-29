#pragma once

namespace Zos::User::WaitTest {
    using Uint64 = unsigned long long;

    inline constexpr Uint64 ImageBase{ 0x0000000040000000ULL };

    inline constexpr Uint64 StateAddress{ 0x0000000040010000ULL };

    inline constexpr Uint64 StackGuardAddress{ 0x0000000070000000ULL };

    inline constexpr Uint64 StackBaseAddress{ 0x0000000070001000ULL };

    inline constexpr Uint64 StackPageCount{ 4 };
    inline constexpr Uint64 PageSize{ 4096 };

    inline constexpr Uint64 StackTopAddress{ StackBaseAddress + StackPageCount * PageSize };

    inline constexpr Uint64 CompletionMarker{ 0x5A4F535741495432ULL };

    inline constexpr int TestExitStatus{ 22 };

    enum class Role : Uint64 {
        EventWaiter = 1,
        EventSignaler = 2,
        SemaphoreWaiter = 3,
        SemaphoreReleaser = 4,
        MutexOwner = 5,
        MutexWaiter = 6,
        TimerWaiter = 7,
    };

    struct State final {
        Uint64 RoleId{};
        Uint64 PrimaryHandle{};
        Uint64 SecondaryHandle{};

        Uint64 ProcessId{};
        Uint64 Entered{};
        Uint64 Acquired{};
        Uint64 Released{};
        Uint64 Completed{};
        Uint64 Marker{};
    };

    static_assert(sizeof(State) == 9 * sizeof(Uint64));
}
