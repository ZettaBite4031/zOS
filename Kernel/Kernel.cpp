#include <Boot/Protocol.hpp>

namespace {
    using namespace Zos;

    void WriteDebugChar(char value) noexcept {
        constexpr unsigned short DebugPort = 0xE9;
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(DebugPort));
    }

    void WriteDebug(const char* msg) noexcept {
        while (*msg != '\0') {
            WriteDebugChar(*msg);
            msg++;
        }
    }

    void WriteHex(Boot::Uint64 value) noexcept {
        static constexpr char Digits[] = "0123456789ABCDEF";
        WriteDebug("0x");

        bool emittedDigit = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto digit = static_cast<Boot::Uint8>((value >> shift) & 0xF);
            if (digit != 0 || emittedDigit || shift == 0) {
                WriteDebugChar(Digits[digit]);
                emittedDigit = true;
            }
        }
    }

    void WriteDecimal(Boot::Uint64 value) noexcept {
        char buffer[21];
        Boot::Uint64 position = sizeof(buffer);

        do {
            const auto digit = static_cast<Boot::Uint8>(value % 10);
            buffer[--position] = static_cast<char>('0' + digit);
            value /= 10;
        } while (value != 0);

        while (position < sizeof(buffer)) {
            WriteDebugChar(buffer[position]);
            ++position;
        }
    }

    [[noreturn]] void Halt() noexcept {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    [[noreturn]] void RejectBootEnvironment(const char* reason) noexcept {
        WriteDebug("[zOS/Kernel] FATAL: Invalid boot environment: ");
        WriteDebug(reason);
        WriteDebug(".\n");
        Halt();
    }

    void PrintSignature(Boot::Uint64 signature) {
        for (Boot::Uint32 i = 0; i < 8; i++) {
            Boot::Uint8 byte = static_cast<Boot::Uint8>((signature >> (i * 8)) & 0xFF);
            if (byte == '\0') break;
            WriteDebugChar(static_cast<char>(byte));
        }

    }
}

extern "C" [[noreturn]] __attribute__((section(".text.KernelMain"))) void KernelMain(const Zos::Boot::BootEnvironment_V1* environment) noexcept {
    using namespace Zos::Boot;
    
    WriteDebug("\n\n[zOS/Kernel]=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
    WriteDebug("[zOS/Kernel]           Kernel entry reached\n");
    WriteDebug("[zOS/Kernel]=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");

    if (environment == nullptr)
        RejectBootEnvironment("pointer is null");

    if (environment->Signature != EnvironmentSignature) 
        RejectBootEnvironment("signature mismatch");

    if (environment->Version != ProtocolVersion)
        RejectBootEnvironment("unsupported protocol version");

    if (environment->Size < sizeof(BootEnvironment_V1))
        RejectBootEnvironment("structure is too small");

    if (environment->KernelImage.Size == 0) 
        RejectBootEnvironment("kernel image range is empty");

    if (environment->KernelStack.Size == 0) 
        RejectBootEnvironment("kernel stack range is empty");

    if (environment->MemoryMapSize == 0 ||
        environment->MemoryMapDescriptorSize == 0 ||
        environment->MemoryMapSize > environment->MemoryMapStorage.Size ||
        (environment->MemoryMapSize % environment->MemoryMapDescriptorSize) != 0) 
        RejectBootEnvironment("memory map metadata is inconsistent");
    
    WriteDebug("[zOS/Kernel] Boot protocol signature: ");
    PrintSignature(environment->Signature);
    WriteDebug("\n");

    WriteDebug("[zOS/Kernel] Boot protocol version: ");
    WriteDecimal(environment->Version);
    WriteDebug("\n");

    WriteDebug("[zOS/Kernel] Kernel image: ");
    WriteHex(environment->KernelImage.Base);
    WriteDebug(" + ");
    WriteDecimal(environment->KernelImage.Size);
    WriteDebug(" bytes\n");

    WriteDebug("[zOS/Kernel] Kernel stack: ");
    WriteHex(environment->KernelStack.Base);
    WriteDebug(" + ");
    WriteDecimal(environment->KernelStack.Size);
    WriteDebug(" bytes\n");

    WriteDebug("[zOS/Kernel] UEFI memory map: ");
    WriteDecimal(environment->MemoryMapSize);
    WriteDebug(" bytes, descriptor size ");
    WriteDecimal(environment->MemoryMapDescriptorSize);
    WriteDebug("\n");

    WriteDebug("[zOS/Kernel] Firmware handoff validated.\n");

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}