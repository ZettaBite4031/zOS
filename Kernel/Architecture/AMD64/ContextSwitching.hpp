#pragma once

#include <Kernel/Memory/KernelStack.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    using ContextEntry = void(*)(void* argument) noexcept;

    /*
     * Cooperative kernel-thread context.
     *
     * SwitchThreadContext behaves like an ordinary SysV AMD64 function
     * call from the compiler's point of view, so only the integer
     * callee-saved register set and RSP belong here.
     */
    struct ThreadContext final {
        Memory::Uint64 Rsp{};
        Memory::Uint64 Rbx{};
        Memory::Uint64 Rbp{};
        Memory::Uint64 R12{};
        Memory::Uint64 R13{};
        Memory::Uint64 R14{};
        Memory::Uint64 R15{};
    };

    static_assert(__builtin_offsetof(ThreadContext, Rsp) == 0);
    static_assert(__builtin_offsetof(ThreadContext, Rbx) == 8);
    static_assert(__builtin_offsetof(ThreadContext, Rbp) == 16);
    static_assert(__builtin_offsetof(ThreadContext, R12) == 24);
    static_assert(__builtin_offsetof(ThreadContext, R13) == 32);
    static_assert(__builtin_offsetof(ThreadContext, R14) == 40);
    static_assert(__builtin_offsetof(ThreadContext, R15) == 48);
    static_assert(sizeof(ThreadContext) == 56);

    /*
     * Creates the synthetic first-return frame for a never-run context.
     * The resulting RSP is suitable for SwitchThreadContext().
     */
    [[nodiscard]] bool PrepareThreadContext(ThreadContext& context, const Memory::KernelStack& stack, ContextEntry entry, void* argument) noexcept;

    /*
     * Save the current call-preserved execution state into outgoing,
     * restore incoming, and RET on the incoming stack.
     *
     * The caller is responsible for excluding interrupts while changing
     * scheduler-visible thread ownership around this operation.
     */
    extern "C" void SwitchThreadContext(ThreadContext* outgoing, const ThreadContext* incoming) noexcept;
}
