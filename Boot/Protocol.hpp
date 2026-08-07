#pragma once

namespace Zos::Boot {
    using Uint8 = unsigned char;
    using Uint32 = unsigned int;
    using Uint64 = unsigned long long;

    constexpr Uint64 MakeSignature(char byte0, char byte1, char byte2, char byte3, char byte4, char byte5, char byte6, char byte7) noexcept {
        return static_cast<Uint64>(static_cast<Uint8>(byte0))
            | (static_cast<Uint64>(static_cast<Uint8>(byte1)) << 8)
            | (static_cast<Uint64>(static_cast<Uint8>(byte2)) << 16)
            | (static_cast<Uint64>(static_cast<Uint8>(byte3)) << 24)
            | (static_cast<Uint64>(static_cast<Uint8>(byte4)) << 32)
            | (static_cast<Uint64>(static_cast<Uint8>(byte5)) << 40)
            | (static_cast<Uint64>(static_cast<Uint8>(byte6)) << 48)
            | (static_cast<Uint64>(static_cast<Uint8>(byte7)) << 56);
    }

    inline constexpr Uint64 EnvironmentSignature{ MakeSignature('z', 'O', 'S', 'B', 'O', 'O', 'T', '\0') };

    inline constexpr Uint32 ProtocolVersion{ 1 };

    struct PhysicalRange {
        Uint64 Base;
        Uint64 Size;
    };

    struct BootEnvironment_V1 {
        Uint64 Signature;
        Uint32 Version;
        Uint32 Size;

        Uint64 KernelEntryPoint;
        PhysicalRange KernelImage;
        PhysicalRange KernelStack;
        PhysicalRange EnvironmentStorage;
        PhysicalRange MemoryMapStorage;

        Uint64 MemoryMapSize;
        Uint64 MemoryMapDescriptorSize;
        Uint32 MemoryDescriptorVersion;
        Uint32 Reserved;

        Uint64 FirmwareSystemTable;
    };

    static_assert(sizeof(PhysicalRange) == 16);
    static_assert(sizeof(BootEnvironment_V1) == 120);
    static_assert(__builtin_offsetof(BootEnvironment_V1, KernelEntryPoint) == 16);
    static_assert(__builtin_offsetof(BootEnvironment_V1, KernelImage) == 24);
    static_assert(__builtin_offsetof(BootEnvironment_V1, MemoryMapStorage) == 72);
    static_assert(__builtin_offsetof(BootEnvironment_V1, MemoryMapSize) == 88);
    static_assert(__builtin_offsetof(BootEnvironment_V1, FirmwareSystemTable) == 112);
}