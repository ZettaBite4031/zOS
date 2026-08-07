#pragma once

namespace Zos::Boot::ELF {
    using Uint8 = unsigned char;
    using Uint16 = unsigned short;
    using Uint32 = unsigned int;
    using Uint64 = unsigned long long;

    inline constexpr Uint8 Magic0{ 0x7F };
    inline constexpr Uint8 Magic1{ 'E' };
    inline constexpr Uint8 Magic2{ 'L' };
    inline constexpr Uint8 Magic3{ 'F' };
    inline constexpr Uint8 Class64{ 2 };
    inline constexpr Uint8 LittleEndian{ 1 };
    inline constexpr Uint8 CurrentVersion{ 1 };

    enum class Type : Uint16 {
        Executable = 2,
    };

    enum class Machine : Uint16 {
        AMD64 = 62,
    };

    enum class ProgramType : Uint32 {
        Null = 0,
        Load = 1,
    };

    enum ProgramFlags : Uint32 {
        Execute = 1,
        Write = 2,
        Read = 4,
    };

    struct Header {
        Uint8 Identification[16];
        Type TypeValue;
        Machine MachineValue;
        Uint32 Version;
        Uint64 Entry;
        Uint64 ProgramHeaderOffset;
        Uint64 SectionHeaderOffset;
        Uint32 Flags;
        Uint16 HeaderSize;
        Uint16 ProgramHeaderEntrySize;
        Uint16 ProgramHeaderCount;
        Uint16 SectionHeaderEntrySize;
        Uint16 SectionHeaderCount;
        Uint16 SectionNameStringTableIndex;
    };

    struct ProgramHeader {
        ProgramType TypeValue;
        Uint32 Flags;
        Uint64 Offset;
        Uint64 VirtAddress;
        Uint64 PhysAddress;
        Uint64 FileSize;
        Uint64 MemorySize;
        Uint64 Alignment;
    };

    static_assert(sizeof(Header) == 64);
    static_assert(sizeof(ProgramHeader) == 56);
}