#include <User/Runtime/Process.hpp>

extern "C" {
    using StartupFunction = void(*)();

    extern StartupFunction __init_array_start[];
    extern StartupFunction __init_array_end[];
    extern StartupFunction __fini_array_start[];
    extern StartupFunction __fini_array_end[];

    [[noreturn]] void ZosUserRuntimeEntry() noexcept;
}

int main() noexcept;

namespace Zos::User::Runtime {
    void RunTerminationCallbacks() noexcept;
}

/*
 * EnterUserMode currently supplies an aligned empty user stack, not a C-call
 * return address. Normalize RSP and CALL into the C++ runtime, establishing
 * the SysV AMD64 function-entry stack contract.
 */
__asm__(
    ".pushsection .text.UserStartup,\"ax\",@progbits\n"
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "    xorq %rbp, %rbp\n"
    "    andq $-16, %rsp\n"
    "    call ZosUserRuntimeEntry\n"
    "    ud2\n"
    ".size _start, .-_start\n"
    ".popsection\n"
);

extern "C" [[noreturn]] void ZosUserRuntimeEntry() noexcept {
    for (auto* entry = __init_array_start; entry != __init_array_end; entry++)
        if (*entry != nullptr) (*entry)();

    const int status = main();
    Zos::User::Runtime::RunTerminationCallbacks();

    // The matching static C++ destructors execute in reverse registration order;
    for (auto* entry = __fini_array_end; entry != __fini_array_start;) {
        --entry;
        if (*entry != nullptr) (*entry)();
    }

    Zos::User::Runtime::ExitProcess(status);
}