#include <Boot/Protocol.hpp>

#include <Boot/UEFI/UEFI.hpp>

extern "C" [[noreturn]] __attribute__((naked)) void TransferControl(Zos::Boot::UEFI::PhysicalAddress, Zos::Boot::UEFI::PhysicalAddress, const Zos::Boot::BootEnvironment*) noexcept {
    __asm__ volatile(
        "cli\n\t"
        "cld\n\t"

        /*
         * Microsoft x64 ABI:
         * 
         * RCX - entry point
         * RDX - stack top
         * R8 - BootEnvironment*
         */

         "movq %rdx, %rsp\n\t"
         "andq $-16, %rsp\n\t"

         "xorq %rbp, %rbp\n\t"

         /*
          * Kernel uses SysV AMD64:
          *
          * RDI - first argument
          */
         "movq %r8, %rdi\n\t"

         "callq *%rcx\n\t"

         "1:\n\t"
         "hlt:\n\t"
         "jmp 1b\n\t"
    );
}