#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Platform::ACPI {
    using Memory::Uint8;
    using Memory::Uint32;
    using Memory::Uint64;

    class TableSignature final {
    public:
        constexpr TableSignature() noexcept = default;
        constexpr TableSignature(char byte0, char byte1, char byte2, char byte3) noexcept : m_Bytes{ byte0, byte1, byte2, byte3 } {}

        [[nodiscard]] constexpr char operator[](Uint64 index) const noexcept { return m_Bytes[index]; }

    private:
        char m_Bytes[4]{};
    };

    inline constexpr TableSignature XsdtSignature{ 'X', 'S', 'D', 'T' };

    struct __attribute__((packed)) RootSystemDescriptionPointer final {
        char Signature[8];
        Uint8 Checksum;
        char OemId[6];
        Uint8 Revision;
        Uint32 RsdtAddress;
        Uint32 Length;
        Uint64 XsdtAddress;
        Uint8 ExtendedChecksum;
        Uint8 Reserved[3];
    };

    struct __attribute__((packed)) DescriptionHeader final {
        char Signature[4];
        Uint32 Length;
        Uint8 Revision;
        Uint8 Checksum;
        char OemId[6];
        char OemTableId[8];
        Uint32 OemRevision;
        Uint32 CreatorId;
        Uint32 CreatorRevision;
    };

    static_assert(sizeof(RootSystemDescriptionPointer) == 36);
    static_assert(sizeof(DescriptionHeader) == 36);

}
