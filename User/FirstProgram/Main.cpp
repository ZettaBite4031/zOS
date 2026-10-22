#include <ABI/SystemCalls.hpp>
#include <User/FirstProgram/Contract.hpp>

namespace {
    using Zos::User::FirstProgram::Uint64;

    __attribute__((section(".zos_state"), used))
    volatile Zos::User::FirstProgram::State g_State{};

    volatile Uint64 g_BssProbe;

    [[nodiscard]] Uint64 ReadStackPointer() noexcept {
        Uint64 value{};
        __asm__ volatile("movq %%rsp, %0" : "=r"(value));
        return value;
    }

    [[nodiscard]] Uint64 GetProcessId(Uint64& preserved_register) noexcept {
        register Uint64 result __asm__("rax") = static_cast<Uint64>(Zos::ABI::NativeSystemCall::GetProcessId);
        register Uint64 preserved __asm__("r12") = preserved_register;

        __asm__ volatile(
            "syscall"
            : "+a"(result), "+r"(preserved)
            :
            : "rcx", "r11", "memory"
        );

        preserved_register = preserved;
        return result;
    }
}

extern "C" [[noreturn]] void _start() noexcept {
    using namespace Zos::User::FirstProgram;

    if (g_BssProbe != 0)
        __builtin_trap();

    g_BssProbe = 1;
    g_State.BssZeroVerified = 1;
    g_State.Marker = EntryMarker;
    g_State.Progress = g_State.Progress + 1;

    Uint64 preserved_register = PreservedRegisterMarker;
    g_State.StackBeforeSystemCall = ReadStackPointer();
    g_State.ProcessId = GetProcessId(preserved_register);
    g_State.StackAfterSystemCall = ReadStackPointer();
    g_State.PreservedRegister = preserved_register;

    for (;;) {
        g_State.Progress = g_State.Progress + 1;

        if (g_State.FaultRequest != 0) {
            const auto* fault = reinterpret_cast<volatile const Uint64*>(StackGuardAddress);
            const Uint64 value = *fault;
            (void)value;

            /* The guard-page read must never succeed. */
            __builtin_trap();
        }

        __asm__ volatile("pause");
    }
}
