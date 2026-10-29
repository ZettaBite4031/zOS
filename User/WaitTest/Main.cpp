#include <User/WaitTest/Contract.hpp>

#include <User/Runtime/Handles.hpp>
#include <User/Runtime/Process.hpp>
#include <User/Runtime/Synchronization.hpp>

namespace {
    __attribute__((section(".zos_state"), used))
    volatile Zos::User::WaitTest::State g_State{};
}

int main() noexcept {
    using namespace Zos::User::Runtime;
    namespace Contract = Zos::User::WaitTest;

    const Handle primary{ g_State.PrimaryHandle };
    const Handle secondary{ g_State.SecondaryHandle };

    if (!primary.IsValid()) return 10;

    g_State.ProcessId = GetProcessId();
    g_State.Entered = 1;

    switch (static_cast<Contract::Role>(g_State.RoleId)) {
    case Contract::Role::EventWaiter:
    case Contract::Role::SemaphoreWaiter:
    case Contract::Role::TimerWaiter:
        if (!WaitForObject(primary)) return 11;
        g_State.Acquired = 1;
        break;

    case Contract::Role::EventSignaler:
        if (!SignalEvent(primary)) return 12;
        g_State.Released = 1;
        break;

    case Contract::Role::SemaphoreReleaser:
        if (!ReleaseSemaphore(primary)) return 13;
        g_State.Released = 1;
        break;

    case Contract::Role::MutexOwner:
        if (!secondary.IsValid()) return 14;
        if (!WaitForObject(primary)) return 15;
        g_State.Acquired = 1;
        if (!WaitForObject(secondary)) return 16;
        if (!ReleaseMutex(primary)) return 17;
        g_State.Released = 1;
        break;

    case Contract::Role::MutexWaiter:
        if (!WaitForObject(primary)) return 18;
        g_State.Acquired = 1;
        if (!ReleaseMutex(primary)) return 19;
        g_State.Released = 1;
        break;

    default: return 20;
    }

    g_State.Completed = 1;
    g_State.Marker = Contract::CompletionMarker;
    return Contract::TestExitStatus;
}
