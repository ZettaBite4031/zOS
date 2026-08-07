#pragma once

namespace Zos::Boot::UEFI {
    using Boolean = unsigned char;
    using Char16 = char16_t;
    using Handle = void*;
    using Status = unsigned long long;
    using Uint32 = unsigned int;
    using Uint64 = unsigned long long;
    using UintN = unsigned long long;

    inline constexpr Status Success{ 0 };

    struct TableHeader {
        Uint64 Signature;
        Uint32 Revision;
        Uint32 HeaderSize;
        Uint32 CRC32;
        Uint32 Reserved;
    };

    struct SimpleTextOutputProtocol;

    using TextReset = Status(*)(SimpleTextOutputProtocol* self, Boolean extended_verification);
    using TextOutputString = Status(*)(SimpleTextOutputProtocol* self, Char16* string);

    struct SimpleTextOutputProtocol {
        TextReset Reset;
        TextOutputString OutputString;
        void* TestString;
        void* QueryMode;
        void* SetMode;
        void* SetAttribute;
        void* ClearScreen;
        void* SetCursorPosition;
        void* EnableCursor;
        void* Mode;
    };

    struct SystemTable {
        TableHeader Header;
        Char16* FirmwareVendor;
        Uint32 FirmwareRevision;
        Handle ConsoleInputHandle;
        void* ConsoleInput;
        Handle ConsoleOutputHandle;
        SimpleTextOutputProtocol* ConsoleOutput;
        Handle StandardErrorHandle;
        SimpleTextOutputProtocol* StandardError;
        void* RuntimeServices;
        void* BootServices;
        UintN ConfigurationTableCount;
        void* ConfigurationTables;
    };

    static_assert(sizeof(TableHeader) == 24);
    static_assert(sizeof(SimpleTextOutputProtocol) == 80);
    static_assert(sizeof(SystemTable) == 120);
    static_assert(__builtin_offsetof(SystemTable, ConsoleOutput) == 64);
}