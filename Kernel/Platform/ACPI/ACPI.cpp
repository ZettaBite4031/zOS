#include <Kernel/Platform/ACPI/ACPI.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Memory/Layout.hpp>

namespace Zos::Kernel::Platform::ACPI {
    namespace {
        inline constexpr char RsdpSignature[8]{ 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
        inline constexpr Uint64 LegacyRsdpLength{ 20 };
    }

    const Uint8* TableDirectory::PhysicalPointer(Memory::PhysicalAddress address, Uint64 length) noexcept {
        if (address.IsNull() || length == 0 || address.Value() >= Memory::Layout::DirectMapSize) return nullptr;
        if (length > Memory::Layout::DirectMapSize - address.Value()) return nullptr;

        const Memory::VirtualAddress direct = Memory::Layout::DirectMapAddress(address);
        if (direct.IsNull()) return nullptr;
        return reinterpret_cast<const Uint8*>(direct.Value());
    }

    bool TableDirectory::ChecksumValid(const Uint8* bytes, Uint64 length) noexcept {
        if (bytes == nullptr || length == 0) return false;

        Uint8 sum{};
        for (Uint64 i = 0; i < length; i++) sum = static_cast<Uint8>(sum + bytes[i]);
        return sum == 0;
    }

    bool TableDirectory::SignatureMatches(const char* bytes, TableSignature signature) noexcept {
        if (bytes == nullptr) return false;
        for (Uint64 i = 0; i < 4; i++) if (bytes[i] != signature[i]) return false;
        return true;
    }

    Uint64 TableDirectory::ReadUint64(const Uint8* bytes) noexcept {
        if (bytes == nullptr) return 0;

        Uint64 value{};
        for (Uint64 i = 0; i < sizeof(Uint64); i++) value |= static_cast<Uint64>(bytes[i]) << (i * 8);
        return value;
    }

    bool TableDirectory::ValidateTable(Memory::PhysicalAddress address, const DescriptionHeader*& table) noexcept {
        table = nullptr;

        const Uint8* header_bytes = PhysicalPointer(address, sizeof(DescriptionHeader));
        if (header_bytes == nullptr) return false;

        const auto* header = reinterpret_cast<const DescriptionHeader*>(header_bytes);
        if (header->Length < static_cast<Uint32>(sizeof(DescriptionHeader))) return false;

        const Uint8* table_bytes = PhysicalPointer(address, header->Length);
        if (table_bytes == nullptr || !ChecksumValid(table_bytes, header->Length)) return false;

        table = header;
        return true;
    }

    InitializationError TableDirectory::Initialize(Memory::PhysicalAddress rsdp_address) noexcept {
        if (m_Initialized) return InitializationError::AlreadyInitialized;
        if (rsdp_address.IsNull()) return InitializationError::InvalidRootPointer;

        const Uint8* rsdp_bytes = PhysicalPointer(rsdp_address, sizeof(RootSystemDescriptionPointer));
        if (rsdp_bytes == nullptr) return InitializationError::AddressUnavailable;

        const auto* rsdp = reinterpret_cast<const RootSystemDescriptionPointer*>(rsdp_bytes);
        for (Uint64 i = 0; i < sizeof(RsdpSignature); i++) if (rsdp->Signature[i] != RsdpSignature[i]) return InitializationError::InvalidRsdpSignature;

        if (!ChecksumValid(rsdp_bytes, LegacyRsdpLength)) return InitializationError::InvalidRsdpChecksum;
        if (rsdp->Revision < 2) return InitializationError::UnsupportedRevision;
        if (rsdp->Length < static_cast<Uint32>(sizeof(RootSystemDescriptionPointer))) return InitializationError::InvalidRsdpLength;

        const Uint8* complete_rsdp = PhysicalPointer(rsdp_address, rsdp->Length);
        if (complete_rsdp == nullptr) return InitializationError::AddressUnavailable;
        if (!ChecksumValid(complete_rsdp, rsdp->Length)) return InitializationError::InvalidRsdpChecksum;
        if (rsdp->XsdtAddress == 0) return InitializationError::MissingXsdt;

        const Memory::PhysicalAddress xsdt_address{ rsdp->XsdtAddress };
        const Uint8* xsdt_header_bytes = PhysicalPointer(xsdt_address, sizeof(DescriptionHeader));
        if (xsdt_header_bytes == nullptr) return InitializationError::AddressUnavailable;

        const auto* xsdt = reinterpret_cast<const DescriptionHeader*>(xsdt_header_bytes);
        if (!SignatureMatches(xsdt->Signature, XsdtSignature)) return InitializationError::InvalidXsdtSignature;
        if (xsdt->Length < static_cast<Uint32>(sizeof(DescriptionHeader))) return InitializationError::InvalidXsdtLength;

        const Uint64 entry_bytes = static_cast<Uint64>(xsdt->Length) - sizeof(DescriptionHeader);
        if ((entry_bytes % sizeof(Uint64)) != 0) return InitializationError::InvalidXsdtLength;

        const Uint8* complete_xsdt = PhysicalPointer(xsdt_address, xsdt->Length);
        if (complete_xsdt == nullptr) return InitializationError::AddressUnavailable;
        if (!ChecksumValid(complete_xsdt, xsdt->Length)) return InitializationError::InvalidXsdtChecksum;

        const Uint64 table_count = entry_bytes / sizeof(Uint64);
        const Uint8* entries = complete_xsdt + sizeof(DescriptionHeader);

        for (Uint64 i = 0; i < table_count; i++) {
            const Memory::PhysicalAddress table_address{ ReadUint64(entries + i * sizeof(Uint64)) };
            const DescriptionHeader* table = nullptr;
            if (table_address.IsNull() || !ValidateTable(table_address, table)) return InitializationError::InvalidTable;
        }

        Diagnostics::Write("[zOS/ACPI] RSDP revision ");
        Diagnostics::WriteDecimal(rsdp->Revision);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/ACPI] XSDT: ");
        Diagnostics::WriteHex(xsdt_address.Value());
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/ACPI] Tables:");
        for (Uint64 i = 0; i < table_count; i++) {
            const Memory::PhysicalAddress table_address{ ReadUint64(entries + i * sizeof(Uint64)) };
            const DescriptionHeader* table = nullptr;
            if (!ValidateTable(table_address, table)) return InitializationError::InvalidTable;

            Diagnostics::Write(" ");
            for (Uint64 byte = 0; byte < 4; byte++) Diagnostics::WriteChar(table->Signature[byte]);
        }
        Diagnostics::Write("\n");

        m_RsdpAddress = rsdp_address;
        m_XsdtAddress = xsdt_address;
        m_TableCount = table_count;
        m_Revision = rsdp->Revision;
        m_Initialized = true;
        return InitializationError::Success;
    }

    const DescriptionHeader* TableDirectory::RootTable() const noexcept {
        if (!m_Initialized) return nullptr;

        const DescriptionHeader* table = nullptr;
        if (!ValidateTable(m_XsdtAddress, table) || !SignatureMatches(table->Signature, XsdtSignature)) return nullptr;
        return table;
    }

    Memory::PhysicalAddress TableDirectory::TableAddress(Uint64 index) const noexcept {
        if (!m_Initialized || index >= m_TableCount) return {};

        const DescriptionHeader* xsdt = RootTable();
        if (xsdt == nullptr) return {};

        const auto* entries = reinterpret_cast<const Uint8*>(xsdt) + sizeof(DescriptionHeader);
        return Memory::PhysicalAddress(ReadUint64(entries + index * sizeof(Uint64)));
    }

    const DescriptionHeader* TableDirectory::Table(Uint64 index) const noexcept {
        const Memory::PhysicalAddress address = TableAddress(index);
        if (address.IsNull()) return nullptr;

        const DescriptionHeader* table = nullptr;
        if (!ValidateTable(address, table)) return nullptr;
        return table;
    }

    const DescriptionHeader* TableDirectory::FindTable(TableSignature signature, Uint64 occurrence) const noexcept {
        if (!m_Initialized) return nullptr;

        Uint64 match{};
        for (Uint64 i = 0; i < m_TableCount; i++) {
            const DescriptionHeader* table = Table(i);
            if (table == nullptr || !SignatureMatches(table->Signature, signature)) continue;
            if (match == occurrence) return table;
            match++;
        }
        return nullptr;
    }

    const char* TableDirectory::Describe(InitializationError error) noexcept {
        switch (error) {
        case InitializationError::Success: return "success";
        case InitializationError::AlreadyInitialized: return "ACPI table directory is already initialized";
        case InitializationError::InvalidRootPointer: return "ACPI RSDP physical address is invalid";
        case InitializationError::AddressUnavailable: return "ACPI table lies outside the available direct map";
        case InitializationError::InvalidRsdpSignature: return "ACPI RSDP signature is invalid";
        case InitializationError::InvalidRsdpChecksum: return "ACPI RSDP checksum is invalid";
        case InitializationError::UnsupportedRevision: return "ACPI revision does not provide an XSDT";
        case InitializationError::InvalidRsdpLength: return "ACPI RSDP length is invalid";
        case InitializationError::MissingXsdt: return "ACPI RSDP does not provide an XSDT";
        case InitializationError::InvalidXsdtSignature: return "ACPI XSDT signature is invalid";
        case InitializationError::InvalidXsdtLength: return "ACPI XSDT length is invalid";
        case InitializationError::InvalidXsdtChecksum: return "ACPI XSDT checksum is invalid";
        case InitializationError::InvalidTable: return "ACPI XSDT contains an invalid table";
        default: return "unknown ACPI initialization error";
        }
    }
}
