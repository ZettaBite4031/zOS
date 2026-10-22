#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Executables::ELF {
    using Memory::Uint8;
    using Memory::Uint16;
    using Memory::Uint32;
    using Memory::Uint64;

    inline constexpr Uint8 Magic0{ 0x7F };
    inline constexpr Uint8 Magic1{ 'E' };
    inline constexpr Uint8 Magic2{ 'L' };
    inline constexpr Uint8 Magic3{ 'F' };

    inline constexpr Uint8 Class64{ 2 };
    inline constexpr Uint8 LittleEndian{ 1 };
    inline constexpr Uint8 CurrentVersion{ 1 };

    enum class FileType : Uint16 {
        Executable = 2,
    };

    enum class Machine : Uint16 {
        AMD64 = 62,
    };

    enum class ProgramType : Uint32 {
        Null = 0,
        Load = 1,
        Dynamic = 2,
        Interpreter = 3,
        Note = 4,
        ProgramHeader = 6,
    };

    enum ProgramFlag : Uint32 {
        Execute = 1u << 0,
        Write = 1u << 1,
        Read = 1u << 2,
    };

    struct Header final {
        Uint8 Identification[16]{};
        FileType Type{};
        Machine MachineValue{};
        Uint32 Version{};
        Uint64 Entry{};
        Uint64 ProgramHeaderOffset{};
        Uint64 SectionHeaderOffset{};
        Uint32 Flags{};
        Uint16 HeaderSize{};
        Uint16 ProgramHeaderEntrySize{};
        Uint16 ProgramHeaderCount{};
        Uint16 SectionHeaderEntrySize{};
        Uint16 SectionHeaderCount{};
        Uint16 SectionNameIndex{};
    } __attribute__((packed));

    static_assert(sizeof(Header) == 64);

    struct ProgramHeader final {
        ProgramType Type{};
        Uint32 Flags{};
        Uint64 Offset{};
        Uint64 VirtualAddress{};
        Uint64 PhysicalAddress{};
        Uint64 FileSize{};
        Uint64 MemorySize{};
        Uint64 Alignment{};
    } __attribute__((packed));

    static_assert(sizeof(ProgramHeader) == 56);
}
