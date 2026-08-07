#include <Boot/UEFI/UEFI.hpp>

namespace {
    using namespace Zos::Boot::UEFI;

    Char16 Banner[] = u"\r\nzOS UEFI Loader\r\n";
    Char16 FirmwareStatus[] = u"Firmware application entry verified.\r\n";
    Char16 NextMilestone[] = u"Kernel loading is the next milestone!\r\n";

    // QEMU's debug console is temporary bring-up infrastructure. It gives the
    // first milestone deterministic headless output without becoming part of 
    // the permanent zOS firmware or logging architecture.
    void WriteDebugCharacter(char value) noexcept {
        constexpr unsigned short DebugPort = 0xE9;
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(DebugPort));
    }

    void WriteDebug(const char* msg) noexcept {
        while (*msg != '\0') {
            WriteDebugCharacter(*msg);
            msg++;
        }
    }

    void WriteConsole(SystemTable& st, Char16* msg) noexcept {
        if (st.ConsoleOutput != nullptr && st.ConsoleOutput->OutputString != nullptr)
            st.ConsoleOutput->OutputString(st.ConsoleOutput, msg);
    }
}

extern "C" Zos::Boot::UEFI::Status EfiMain(Zos::Boot::UEFI::Handle ImageHandle, Zos::Boot::UEFI::SystemTable* SystemTable) noexcept {
    static_cast<void>(ImageHandle);

    using namespace Zos::Boot::UEFI;

    if (SystemTable == nullptr) {
        WriteDebug("[zOS/Boot] Invalid UEFI system table.\n");
        return 1;
    }

    WriteConsole(*SystemTable, Banner);
    WriteConsole(*SystemTable, FirmwareStatus);
    WriteConsole(*SystemTable, NextMilestone);

    WriteDebug("[zOS/Boot] UEFI Application entered.\n");
    WriteDebug("[zOS/Boot] Firmware console is available.\n");
    WriteDebug("[zOS/Boot] Kernel loading is not implemented yet!\n");

    return Success;
}