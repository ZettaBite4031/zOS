#include <Kernel/Architecture/AMD64/ContextSwitching.hpp>

extern "C" void ContextEntryTrampoline() noexcept;

__asm__(
    ".pushsection .text.ContextSwitch,\"ax\",@progbits\n"

    ".global SwitchThreadContext\n"
    ".type SwitchThreadContext,@function\n"
    "SwitchThreadContext:\n"
    "    movq %rsp, 0(%rdi)\n"
    "    movq %rbx, 8(%rdi)\n"
    "    movq %rbp, 16(%rdi)\n"
    "    movq %r12, 24(%rdi)\n"
    "    movq %r13, 32(%rdi)\n"
    "    movq %r14, 40(%rdi)\n"
    "    movq %r15, 48(%rdi)\n"

    "    movq 8(%rsi), %rbx\n"
    "    movq 16(%rsi), %rbp\n"
    "    movq 24(%rsi), %r12\n"
    "    movq 32(%rsi), %r13\n"
    "    movq 40(%rsi), %r14\n"
    "    movq 48(%rsi), %r15\n"
    "    movq 0(%rsi), %rsp\n"
    "    ret\n"
    ".size SwitchThreadContext, .-SwitchThreadContext\n"

    /*
     * PrepareThreadContext places this three-word startup image on a
     * fresh stack:
     *
     *     initial RSP -> ContextEntryTrampoline
     *                    ContextEntry
     *                    argument
     *
     * SwitchThreadContext's RET consumes the first word. The two POPs
     * below consume the remaining metadata and leave RSP 16-byte aligned
     * immediately before CALL, satisfying the SysV AMD64 ABI.
     */
    ".global ContextEntryTrampoline\n"
    ".type ContextEntryTrampoline,@function\n"
    "ContextEntryTrampoline:\n"
    "    popq %rax\n"
    "    popq %rdi\n"
    "    call *%rax\n"

    /*
     * A context entry is not permitted to return through the synthetic
     * frame. Scheduling::Thread handles normal thread-entry return first;
     * this is a final architectural guard.
     */
    "    ud2\n"
    ".size ContextEntryTrampoline, .-ContextEntryTrampoline\n"

    ".popsection\n"
);

namespace Zos::Kernel::Architecture::AMD64 {
    bool PrepareThreadContext(ThreadContext& context, const Memory::KernelStack& stack, ContextEntry entry, void* argument) noexcept {
        if (!stack.IsInitialized() || entry == nullptr) return false;

        const Memory::Uint64 bottom = stack.Bottom().Value();
        const Memory::Uint64 top = stack.Top().Value();
        if (bottom == 0 || top <= bottom || (top & 0xFULL) != 0 || top - bottom < 3 * sizeof(Memory::Uint64))
            return false;

        const Memory::Uint64 initial_rsp = top - 3 * sizeof(Memory::Uint64);
        if (!stack.Contains(Memory::VirtualAddress(initial_rsp))) return false;

        auto* startup = reinterpret_cast<Memory::Uint64*>(initial_rsp);

        startup[0] = reinterpret_cast<Memory::Uint64>(&ContextEntryTrampoline);
        startup[1] = reinterpret_cast<Memory::Uint64>(entry);
        startup[2] = reinterpret_cast<Memory::Uint64>(argument);

        context = {};
        context.Rsp = initial_rsp;

        return true;
    }
}