#include <User/ObjectTest/Contract.hpp>

#include <User/Runtime/Handles.hpp>
#include <User/Runtime/Process.hpp>
#include <User/Runtime/Thread.hpp>

namespace {
    __attribute__((section(".zos_state"), used))
    volatile Zos::User::ObjectTest::State g_State{};
}

int main() noexcept {
    using namespace Zos::User;
    using namespace Zos::User::ObjectTest;
    using namespace Zos::User::Runtime;

    const ProcessId direct_process_id = GetProcessId();

    const Handle process = OpenCurrentProcess();
    if (!process.IsValid()) return 10;

    const Handle thread = OpenCurrentThread();
    if (!thread.IsValid()) return 11;

    const ProcessId process_id = GetProcessId(process);
    if (process_id == InvalidProcessId || process_id != direct_process_id)
        return 12;

    const ThreadId thread_id = GetThreadId(thread);
    if (thread_id == InvalidThreadId) return 13;
    if (GetProcessId(thread) != InvalidProcessId)
        return 14;

    g_State.WrongTypeRejected = 1;

    g_State.ProcessId = process_id;
    g_State.ThreadId = thread_id;
    g_State.FirstProcessHandle = process.Value();
    g_State.FirstThreadHandle = thread.Value();

    if (!CloseHandle(process)) return 15;
    if (GetProcessId(process) != InvalidProcessId)
        return 16;

    g_State.StaleHandleRejected = 1;

    const Handle reopened_process = OpenCurrentProcess();
    if (!reopened_process.IsValid() || reopened_process == process ||
        GetProcessId(reopened_process) != process_id)
        return 17;

    g_State.ReopenedProcessHandle = reopened_process.Value();
    if (!CloseHandle(reopened_process) || !CloseHandle(thread))
        return 18;

    g_State.HandlesClosed = 1;
    g_State.Marker = EntryMarker;
    g_State.Passed = 1;

    return TestExitStatus;
}
