#pragma once

namespace Zos::Boot {
    using Uint8 = unsigned char;
    using Uint16 = unsigned short;
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

    // These values deliberately mirror EFI_MEMORY_TYPE. Keeping the firmware
    // memory-map ABI in the boot protocol prevents the kernel from depending on
    // the UEFI loader's implementation headers
    enum class FirmwareMemoryType : Uint32 {
        Reserved = 0,
        LoaderCode = 1,
        LoaderData = 2,
        BootServicesCode = 3,
        BootServicesData = 4,
        RuntimeServicesCode = 5,
        RuntimeServicesData = 6,
        ConventionalMemory = 7,
        UnusableMemory = 8,
        AcpiReclaimMemory = 9,
        AcpiMemoryNvs = 10,
        MemoryMappedIo = 11,
        MemoryMappedIoPortSpace = 12,
        PalCode = 13,
        PersistentMemory = 14,
        UnacceptedMemory = 15,
    };

    struct FirmwareMemoryDescriptor {
        FirmwareMemoryType Type;
        Uint32 Reserved;
        Uint64 PhysStart;
        Uint64 VirtStart;
        Uint64 NumPages;
        Uint64 Attribute;
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
    static_assert(sizeof(FirmwareMemoryDescriptor) == 40);
    static_assert(__builtin_offsetof(FirmwareMemoryDescriptor, PhysStart) == 8);
    static_assert(__builtin_offsetof(FirmwareMemoryDescriptor, NumPages) == 24);

    static_assert(sizeof(BootEnvironment_V1) == 120);
    static_assert(__builtin_offsetof(BootEnvironment_V1, KernelEntryPoint) == 16);
    static_assert(__builtin_offsetof(BootEnvironment_V1, KernelImage) == 24);
    static_assert(__builtin_offsetof(BootEnvironment_V1, MemoryMapStorage) == 72);
    static_assert(__builtin_offsetof(BootEnvironment_V1, MemoryMapSize) == 88);
    static_assert(__builtin_offsetof(BootEnvironment_V1, FirmwareSystemTable) == 112);
}