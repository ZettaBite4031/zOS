#include <Kernel/Architecture/AMD64/Interrupts.hpp>

extern "C" void InterruptDispatch(Zos::Kernel::Architecture::AMD64::InterruptContext* context) noexcept;

/*
 * All 256 runtime-generated vector trampolines converge here.
 * 
 * On entry the stack has already been normalized to:
 * 
 *      Vector
 *      ErrorCode
 *      RIP
 *      CS
 *      RFLAGS
 *      [old RSP]
 *      [old SS]
 * 
 * The final two fields only exist when the processor performed
 * a stack switch
 */
__asm__(
    ".pushsection .text.InterruptCommonEntry,\"ax\",@progbits\n"
    ".global InterruptCommonEntry\n"
    ".type InterruptCommonEntry,@function\n"

    "InterruptCommonEntry:\n"

    /*
     * The SysV ABI requires DF to be clear.
     *
     * IRETQ restores the interrupted RFLAGS, including DF.
     */
    "   cld\n"

    /*
     * Keep this order synchronized with InterruptContext !!!
     */
    "   pushq %rax\n"
    "   pushq %rbx\n"
    "   pushq %rcx\n"
    "   pushq %rdx\n"
    "   pushq %rbp\n"
    "   pushq %rdi\n"
    "   pushq %rsi\n"

    "   pushq %r8\n"
    "   pushq %r9\n"
    "   pushq %r10\n"
    "   pushq %r11\n"
    "   pushq %r12\n"
    "   pushq %r13\n"
    "   pushq %r14\n"
    "   pushq %r15\n"

    "   testb $3, 144(%rsp)\n"
    "   jz 1f\n"
    "   swapgs\n"
    "   movw $0x10, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   movw %ax, %ss\n"
    "1:\n"

    /*
     * R12 now contains the exact InterruptContext pointer.
     *
     * The interrupted R12 value is already safely stored in
     * the register frame above.
     * 
     * R12 is callee-saved under the SysV ABI, which allows 
     * recovery of the original interrupt-frame stack pointer
     * even after aligning RSP for the C++ call.
     */
    "   movq %rsp, %r12\n"

    /*
     * First SysV argument:
     *      InterruptContext* context
     */
    "   movq %rsp, %rdi\n"

    /*
     * Hardware interrupt entry does not give us a SysV call-site
     * alignment guarantee, so alignment is done explicitly.
     */
    "   andq $-16, %rsp\n"
    "   call InterruptDispatch\n"

    /*
     * Return to the exact register image.
     */
    "   movq %r12, %rsp\n"

    "   testb $3, 144(%rsp)\n"
    "   jz 2f\n"
    "   movw $0x2B, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   swapgs\n"
    "2:\n"

    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"

    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbp\n"
    "    popq %rdx\n"
    "    popq %rcx\n"
    "    popq %rbx\n"
    "    popq %rax\n"

    /*
     * Discard our normalized fields:
     *
     *      Vector
     *      ErrorCode
     * IRETQ consumes the processor frame.
     */
    "   addq $16, %rsp\n"
    "iretq\n"

    ".size InterruptCommonEntry, .-InterruptCommonEntry\n"
    ".popsection\n"
);

