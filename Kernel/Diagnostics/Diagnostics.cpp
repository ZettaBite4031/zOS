#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Diagnostics {
    namespace {
        constexpr Memory::Uint16 DebugPort{ 0xE9 };
        constexpr char HexDigits[]{ "0123456789ABCDEF" };
    }

    void WriteChar(char value) noexcept {
        __asm__ volatile(
            "outb %0, %1"
            :
            : "a"(value),
              "Nd"(DebugPort)
        );
    }

    void Write(const char* msg) noexcept {
        if (msg == nullptr) return;
        while (*msg != '\0') {
            WriteChar(*msg);
            msg++;
        }
    }

    void WriteHex(Memory::Uint64 value) noexcept {
        Write("0x");

        bool emitted_digit = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto digit = static_cast<Memory::Uint8>((value >> shift) & 0x0F);
            if (digit != 0 || emitted_digit || shift == 0) {
                WriteChar(HexDigits[digit]);
                emitted_digit = true;
            }
        }
    }

    void WriteDecimal(Memory::Uint64 value) noexcept {
        char buffer[21]{};
        Memory::Uint64 position{ sizeof(buffer) };

        do {
            const auto digit = static_cast<Memory::Uint8>(value % 10);
            buffer[--position] = static_cast<char>('0' + digit);
            value /= 10;
        } while (value != 0);

        while (position < sizeof(buffer)) {
            WriteChar(buffer[position]);
            position++;
        }
    }

    [[noreturn]] void Halt() noexcept {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    [[noreturn]] void Fatal(const char* subsystem, const char* reason) noexcept {
        Write("[zOS/");
        if (subsystem != nullptr) Write(subsystem);
        else Write("Kernel");
        Write("] FATAL: ");

        if (reason != nullptr) Write(reason);
        else Write("unspecified failure");

        Write(".\n");
        Halt();
    }

    
}