#include <User/CityPrograms/Contract.hpp>
#include <User/Runtime/Process.hpp>

#ifndef ZOS_CITY_ROLE
#error "ZOS_CITY_ROLE must identify the City executable"
#endif

#if ZOS_CITY_ROLE < 1 || ZOS_CITY_ROLE > 3
#error "Unsupported City executable role"
#endif

namespace {
    using namespace Zos::User::CityPrograms;

    __attribute__((section(".zos_state"), used))
    volatile State g_State{};
}

int main() noexcept {
    using namespace Zos::User::CityPrograms;

    g_State.Marker = EntryMarker;
    g_State.RoleId = ZOS_CITY_ROLE;
    g_State.ProcessId = Zos::User::Runtime::GetProcessId();
    g_State.Ready = 1;

    for (;;) {
        g_State.Progress = g_State.Progress + 1;
        if (g_State.Arm != 0) {
#if ZOS_CITY_ROLE == 1
            return ExitStatus;
#elif ZOS_CITY_ROLE == 2
            const auto* guard = reinterpret_cast<volatile const Uint64*>(StackGuardAddress);
            const Uint64 value = *guard;
            (void)value;
            __builtin_trap(); // Guard-page access must never complete.
#else
            // The survivor deliberately keeps executing after the others stop.
#endif
        }
        __asm__ volatile("pause");
    }
}