#include <User/Runtime/Process.hpp>

#include <ABI/SystemCalls.hpp>

namespace Zos::User::Runtime {
    namespace {
        using Uint64 = ABI::SystemCallNumber;

        [[nodiscard]] Uint64 Invoke0(ABI::NativeSystemCall call) noexcept {
            register Uint64 result __asm__("rax") = static_cast<Uint64>(call);
            __asm__ volatile("syscall" : "+a"(result) :: "rcx", "r11", "cc", "memory");
            return result;
        }

        [[nodiscard]] Uint64 Invoke1(ABI::NativeSystemCall call, Uint64 arg) noexcept {
            register Uint64 result __asm__("rax") = static_cast<Uint64>(call);
            register Uint64 first __asm__("rdi") = arg;
            __asm__ volatile("syscall" : "+a"(result) : "D"(first) : "rcx", "r11", "cc", "memory");
            return result;
        }
    }

    ProcessId GetProcessId() noexcept {
        return Invoke0(ABI::NativeSystemCall::GetProcessId);
    }

    [[noreturn]] void ExitProcess(int status) noexcept {
        // Preserve the 32-bit two's-complement exit value across the native ABI.
        const auto encoded = static_cast<Uint64>(static_cast<unsigned int>(status));
        (void)Invoke1(ABI::NativeSystemCall::ExitProcess, encoded);

        // Exiting cannot return. A broken kernel implementation must fail visibly.
        __builtin_trap();
        __builtin_unreachable();
    }
}
