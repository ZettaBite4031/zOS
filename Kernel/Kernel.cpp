namespace {
    // This is deliberately QEMU-specific bring-up output. It will be replaced
    // by the kernel logging subsystem after the loader can transfer control.
    void WriteDebugCharacter(char value) noexcept {
        constexpr unsigned short DebugPort = 0xE9;
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(DebugPort));
    }

    void WriteDebug(const char* message) noexcept {
        while (*message != '\0')
        {
            WriteDebugCharacter(*message);
            ++message;
        }
    }
}

extern "C" [[noreturn]] __attribute__((section(".text.KernelMain"))) void KernelMain() noexcept {
    WriteDebug("[zOS/Kernel] Kernel entry reached\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}