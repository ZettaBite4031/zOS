#include <Kernel/Architecture/AMD64/UserMode.hpp>

#include <Kernel/Architecture/AMD64/Interrupts.hpp>
#include <Kernel/Architecture/AMD64/Paging.hpp>

extern "C" [[noreturn]] void EnterUserModeAssembly(Zos::Kernel::Memory::Uint64 entry, Zos::Kernel::Memory::Uint64 stack_top) noexcept;

__asm__(
    ".pushsection .text.EnterUserMode,\"ax\",@progbits\n"
    ".global EnterUserModeAssembly\n"
    ".type EnterUserModeAssembly,@function\n"
    "EnterUserModeAssembly:\n"
    "    cli\n"
    "    movw $0x2B, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    pushq $0x2B\n"
    "    pushq %rsi\n"
    "    pushq $0x202\n"
    "    pushq $0x33\n"
    "    pushq %rdi\n"
    "    xorq %rax, %rax\n"
    "    xorq %rbx, %rbx\n"
    "    xorq %rcx, %rcx\n"
    "    xorq %rdx, %rdx\n"
    "    xorq %rbp, %rbp\n"
    "    xorq %rsi, %rsi\n"
    "    xorq %rdi, %rdi\n"
    "    xorq %r8, %r8\n"
    "    xorq %r9, %r9\n"
    "    xorq %r10, %r10\n"
    "    xorq %r11, %r11\n"
    "    xorq %r12, %r12\n"
    "    xorq %r13, %r13\n"
    "    xorq %r14, %r14\n"
    "    xorq %r15, %r15\n"
    "    swapgs\n"
    "    iretq\n"
    "    ud2\n"
    ".size EnterUserModeAssembly, .-EnterUserModeAssembly\n"
    ".popsection\n"
);

namespace Zos::Kernel::Architecture::AMD64 {
    [[noreturn]] void EnterUserMode(Memory::VirtualAddress entry, Memory::VirtualAddress stack_top) noexcept {
        static_assert(UserDataSelector == 0x2B);
        static_assert(UserCodeSelector == 0x33);

        if (entry.IsNull() || stack_top.IsNull() || !PageMap::IsCanonical(entry) || !PageMap::IsCanonical(stack_top) ||
            entry.Value() >= 0x0000800000000000ULL || stack_top.Value() >= 0x0000800000000000ULL || (stack_top.Value() & 0xFULL) != 0)
            __builtin_trap();

        EnterUserModeAssembly(entry.Value(), stack_top.Value());

        __builtin_unreachable();
    }
}