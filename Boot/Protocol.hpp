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

    inline constexpr Uint32 ProtocolVersion{ 2 };

    struct PhysicalRange {
        Uint64 Base;
        Uint64 Size;
    };

    /*
     * These values intentionally mirror EFI_MEMORY_TYPE.
     * 
     * The boot protocol owns this representation so that the
     * kernel never depends on the UEFI implementation header.
     */
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

    /*
     * Historical V1 defintion.
     *
     * V1 exposed the UEFI System Table directly to the kernel.
     * Retain the definition so that the protocol's evolution
     * remains explicit and auditable.
     */
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

    /*
     * V2 resolves firmware-specific platform roots in the loader.
     * 
     * The kernel receives the physical address of ACPI RSDP
     * rather than retaining a dependency on EFI_SYSTEM_TABLE
     */
    struct BootEnvironment_V2 final {
        Uint64 Signature{};
        Uint32 Version{};
        Uint32 Size{};

        Uint64 KernelEntryPoint{};
        PhysicalRange KernelImage{};
        PhysicalRange KernelStack{};
        PhysicalRange EnvironmentStorage{};
        PhysicalRange MemoryMapStorage{};

        Uint64 MemoryMapSize{};
        Uint64 MemoryMapDescriptorSize{};
        Uint32 MemoryDescriptorVersion{};
        Uint32 Reserved{};

        Uint64 AcpiRsdp{};
    };

    /*
     * Kernel and loader code should use this alias rather than
     * baking a protocol revision into every subsystem API.
     */
    using BootEnvironment = BootEnvironment_V2;

    static_assert(sizeof(PhysicalRange) == 16);
    static_assert(sizeof(FirmwareMemoryDescriptor) == 40);
    static_assert(__builtin_offsetof(FirmwareMemoryDescriptor, PhysStart) == 8);
    static_assert(__builtin_offsetof(FirmwareMemoryDescriptor, NumPages) == 24);

    static_assert(sizeof(BootEnvironment_V1) == 120);
    static_assert(sizeof(BootEnvironment_V2) == 120);
    static_assert(__builtin_offsetof(BootEnvironment_V2, KernelEntryPoint) == 16);
    static_assert(__builtin_offsetof(BootEnvironment_V2, KernelImage) == 24);
    static_assert(__builtin_offsetof(BootEnvironment_V2, MemoryMapStorage) == 72);
    static_assert(__builtin_offsetof(BootEnvironment_V2, MemoryMapSize) == 88);
    static_assert(__builtin_offsetof(BootEnvironment_V2, AcpiRsdp) == 112);
}