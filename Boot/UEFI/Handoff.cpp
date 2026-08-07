#include <Boot/Protocol.hpp>

#include <Boot/UEFI/UEFI.hpp>

extern "C" [[noreturn]] __attribute__((naked)) void TransferControl(Zos::Boot::UEFI::PhysicalAddress, Zos::Boot::UEFI::PhysicalAddress, const Zos::Boot::BootEnvironment_V1*) noexcept {
    __asm__ volatile(
        "cli\n\t"
        "cld\n\t"
        "movq %rdx, %rsp\n\t"
        "andq $-16, %rsp\n\t"
        "xorq %rbp, %rbp\n\t"
        "movq %r8, %rdi\n\t"
        "callq *%rcx\n\t"
        "1:\n\t"
        "hlt\n\t"
        "jmp 1b\n\t"
    );
}