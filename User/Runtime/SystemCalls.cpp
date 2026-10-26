#include <User/Runtime/Handles.hpp>
#include <User/Runtime/Process.hpp>
#include <User/Runtime/Thread.hpp>

#include <ABI/SystemCalls.hpp>

namespace Zos::User::Runtime {
    namespace {
        using Uint64 = ABI::SystemCallNumber;

        [[nodiscard]] Uint64 Invoke0(
            ABI::NativeSystemCall call
        ) noexcept {
            register Uint64 result __asm__("rax") =
                static_cast<Uint64>(call);

            __asm__ volatile(
                "syscall"
                : "+a"(result)
                :
                : "rcx", "r11", "cc", "memory"
            );

            return result;
        }

        [[nodiscard]] Uint64 Invoke1(
            ABI::NativeSystemCall call,
            Uint64 arg
        ) noexcept {
            register Uint64 result __asm__("rax") =
                static_cast<Uint64>(call);

            register Uint64 first __asm__("rdi") = arg;

            __asm__ volatile(
                "syscall"
                : "+a"(result)
                : "D"(first)
                : "rcx", "r11", "cc", "memory"
            );

            return result;
        }
    }

    ProcessId GetProcessId() noexcept {
        return Invoke0(
            ABI::NativeSystemCall::GetProcessId
        );
    }

    Handle OpenCurrentProcess() noexcept {
        return Handle(
            Invoke0(
                ABI::NativeSystemCall::OpenCurrentProcess
            )
        );
    }

    ProcessId GetProcessId(Handle process) noexcept {
        return Invoke1(
            ABI::NativeSystemCall::GetProcessIdFromHandle,
            process.Value()
        );
    }

    Handle OpenCurrentThread() noexcept {
        return Handle(
            Invoke0(
                ABI::NativeSystemCall::OpenCurrentThread
            )
        );
    }

    ThreadId GetThreadId(Handle thread) noexcept {
        return Invoke1(
            ABI::NativeSystemCall::GetThreadIdFromHandle,
            thread.Value()
        );
    }

    bool CloseHandle(Handle handle) noexcept {
        return Invoke1(
            ABI::NativeSystemCall::CloseHandle,
            handle.Value()
        ) == 0;
    }

    [[noreturn]] void ExitProcess(int status) noexcept {
        const auto encoded =
            static_cast<Uint64>(
                static_cast<unsigned int>(status)
            );

        (void)Invoke1(
            ABI::NativeSystemCall::ExitProcess,
            encoded
        );

        __builtin_trap();
        __builtin_unreachable();
    }
}
