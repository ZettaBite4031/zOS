#pragma once

#include <Kernel/Memory/Memory.hpp>
#include <Kernel/Platform/ACPI/Tables.hpp>

namespace Zos::Kernel::Platform::ACPI {
    enum class InitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidRootPointer,
        AddressUnavailable,
        InvalidRsdpSignature,
        InvalidRsdpChecksum,
        UnsupportedRevision,
        InvalidRsdpLength,
        MissingXsdt,
        InvalidXsdtSignature,
        InvalidXsdtLength,
        InvalidXsdtChecksum,
        InvalidTable,
    };

    class TableDirectory final {
    public:
        constexpr TableDirectory() noexcept = default;
        TableDirectory(const TableDirectory&) = delete;
        TableDirectory& operator=(const TableDirectory&) = delete;

        [[nodiscard]] InitializationError Initialize(Memory::PhysicalAddress rsdp_address) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint8 Revision() const noexcept { return m_Revision; }
        [[nodiscard]] Memory::Uint64 TableCount() const noexcept { return m_TableCount; }
        [[nodiscard]] Memory::PhysicalAddress RsdpAddress() const noexcept { return m_RsdpAddress; }
        [[nodiscard]] Memory::PhysicalAddress XsdtAddress() const noexcept { return m_XsdtAddress; }

        [[nodiscard]] const DescriptionHeader* RootTable() const noexcept;
        [[nodiscard]] const DescriptionHeader* Table(Memory::Uint64 index) const noexcept;
        [[nodiscard]] const DescriptionHeader* FindTable(TableSignature signature, Memory::Uint64 occurrence = 0) const noexcept;

        [[nodiscard]] static const char* Describe(InitializationError error) noexcept;

    private:
        [[nodiscard]] static const Memory::Uint8* PhysicalPointer(Memory::PhysicalAddress address, Memory::Uint64 length) noexcept;
        [[nodiscard]] static bool ChecksumValid(const Memory::Uint8* bytes, Memory::Uint64 length) noexcept;
        [[nodiscard]] static bool SignatureMatches(const char* bytes, TableSignature signature) noexcept;
        [[nodiscard]] static Memory::Uint64 ReadUint64(const Memory::Uint8* bytes) noexcept;
        [[nodiscard]] static bool ValidateTable(Memory::PhysicalAddress address, const DescriptionHeader*& table) noexcept;

        [[nodiscard]] Memory::PhysicalAddress TableAddress(Memory::Uint64 index) const noexcept;

        Memory::PhysicalAddress m_RsdpAddress{};
        Memory::PhysicalAddress m_XsdtAddress{};
        Memory::Uint64 m_TableCount{};
        Memory::Uint8 m_Revision{};
        bool m_Initialized{};
    };
}
