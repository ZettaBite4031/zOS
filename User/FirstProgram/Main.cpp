#include <User/FirstProgram/Contract.hpp>

#include <User/Runtime/Process.hpp>
#include <User/Runtime/Memory.hpp>

namespace {
    using Zos::User::FirstProgram::Uint64;

    __attribute__((section(".zos_state"), used))
    volatile Zos::User::FirstProgram::State g_State{};

    volatile Uint64 g_BssProbe;

    struct LifetimeProbe final {
        LifetimeProbe() noexcept { g_State.ConstructorRan = 1; }
        ~LifetimeProbe() noexcept { g_State.DestructorRan = 1; }
    };

    LifetimeProbe g_LifetimeProbe{};

    [[nodiscard]] Uint64 ReadStackPointer() noexcept {
        Uint64 value{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(value));
        return value;
    }
}

int main() noexcept {
    using namespace Zos::User::FirstProgram;

    if (g_State.ConstructorRan != 1) return 10;
    if (g_BssProbe != 0) return 11;

    unsigned char expected[8]{ 0, 1, 2, 3, 4, 5, 6, 7 };
    unsigned char copied[8]{};
    if (memset(copied, 0xA5, sizeof(copied)) != copied || copied[7] != 0xA5)
        return 12;
    if (memcpy(copied, expected, sizeof(copied)) != copied ||
        memcmp(copied, expected, sizeof(copied)) != 0)
        return 13;
    if (memmove(copied + 1, copied, 7) != copied + 1 ||
        copied[0] != 0 || copied[1] != 0 || copied[7] != 6)
        return 14;
    if (memmove(copied, copied + 1, 7) != copied ||
        copied[0] != 0 || copied[6] != 6)
        return 15;

    g_BssProbe = 1;
    g_State.BssZeroVerified = 1;
    g_State.Marker = EntryMarker;
    g_State.Progress = g_State.Progress + 1;

    register Uint64 preserved __asm__("r12") = PreservedRegisterMarker;
    __asm__ volatile("" : "+r"(preserved));

    g_State.StackBeforeSystemCall = ReadStackPointer();
    g_State.ProcessId = Zos::User::Runtime::GetProcessId();
    g_State.StackAfterSystemCall = ReadStackPointer();

    __asm__ volatile("" : "+r"(preserved));
    g_State.PreservedRegister = preserved;

    for (;;) {
        g_State.Progress = g_State.Progress + 1;
        if (g_State.ExitRequest != 0) return TestExitStatus;
        __asm__ volatile("pause");
    }
}