namespace {
    using ExitCallback = void(*)();

    inline constexpr unsigned MaximumExitCallbacks{ 64 };
    ExitCallback g_ExitCallbacks[MaximumExitCallbacks]{};
    unsigned g_ExitCallbackCount{};
}

/*
 * Clang's -fno-use-cxa-atexit mode uses atexit for static destructors.
 * This first native runtime is single-threaded; registration and execution
 * therefore need no locking yet. Capacity exhaustion is reported to callers.
 */
extern "C" int atexit(void (*callback)()) noexcept {
    if (callback == nullptr || g_ExitCallbackCount == MaximumExitCallbacks) return -1;
    g_ExitCallbacks[g_ExitCallbackCount++] = callback;
    return 0;
}

namespace Zos::User::Runtime {
    void RunTerminationCallbacks() noexcept {
        while (g_ExitCallbackCount != 0) {
            auto* callback = g_ExitCallbacks[--g_ExitCallbackCount];
            g_ExitCallbacks[g_ExitCallbackCount] = nullptr;
            callback();
        }
    }
}
