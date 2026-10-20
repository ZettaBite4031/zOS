#include <Kernel/Platform/ACPI/MADT.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Platform::ACPI {
    namespace {
        void WriteProcessorState(Uint32 flags) noexcept {
            if (IsProcessorEnabled(flags)) {
                Diagnostics::Write("enabled");
                return;
            }

            if (IsProcessorOnlineCapable(flags)) {
                Diagnostics::Write("online-capable");
                return;
            }

            Diagnostics::Write("disabled");
        }
    }

    MadtInitializationError Madt::Initialize(const TableDirectory& tables) noexcept {
        if (m_Initialized) return MadtInitializationError::AlreadyInitialized;
        if (!tables.IsInitialized()) return MadtInitializationError::AcpiUnavailable;

        const DescriptionHeader* description = tables.FindTable(MadtSignature);
        if (description == nullptr) return MadtInitializationError::TableNotFound;
        if (description->Length < sizeof(MadtHeader)) return MadtInitializationError::TableTooSmall;

        const auto* table = reinterpret_cast<const MadtHeader*>(description);
        const auto* bytes = reinterpret_cast<const Uint8*>(table);
        const Uint64 table_length = table->Header.Length;

        MadtStatistics statistics{};
        Memory::PhysicalAddress local_apic_address{ table->LocalApicAddress };
        bool local_apic_address_overridden{};

        Uint64 offset = sizeof(MadtHeader);
        while (offset < table_length) {
            if (table_length - offset < sizeof(MadtEntryHeader)) 
                return MadtInitializationError::InvalidEntry;

            const auto* entry = reinterpret_cast<const MadtEntryHeader*>(bytes + offset);
            if (entry->Length < sizeof(MadtEntryHeader) || static_cast<Uint64>(entry->Length) > table_length - offset)
                return MadtInitializationError::InvalidEntry;

            switch (entry->Type) {
            case MadtEntryType::ProcessorLocalApic:
                if (entry->Length < sizeof(ProcessorLocalApicEntry)) 
                    return MadtInitializationError::InvalidEntry;
                
                statistics.ProcessorLocalApicCount++;
                break;
            
            case MadtEntryType::IoApic:
                if (entry->Length < sizeof(IoApicEntry))
                    return MadtInitializationError::InvalidEntry;

                statistics.IoApicCount++;
                break;

            case MadtEntryType::InterruptSourceOverride:
                if (entry->Length < sizeof(InterruptSourceOverrideEntry))
                    return MadtInitializationError::InvalidEntry;

                statistics.InterruptSourceOverrideCount++;
                break;

            case MadtEntryType::LocalApicAddressOverride: {
                if (entry->Length < sizeof(LocalApicAddressOverrideEntry))
                    return MadtInitializationError::InvalidEntry;

                if (local_apic_address_overridden)
                    return MadtInitializationError::MultipleLocalApicAddressOverrides;

                const auto* override_entry = reinterpret_cast<const LocalApicAddressOverrideEntry*>(entry);

                if (override_entry->Reserved != 0 || override_entry->LocalApicAddress == 0)
                    return MadtInitializationError::InvalidEntry;

                local_apic_address = Memory::PhysicalAddress(override_entry->LocalApicAddress);
                local_apic_address_overridden = true;
                break;
            }

            case MadtEntryType::ProcessorLocalX2Apic: {
                if (entry->Length < sizeof(ProcessorLocalX2ApicEntry))
                    return MadtInitializationError::InvalidEntry;

                const auto* processor = reinterpret_cast<const ProcessorLocalX2ApicEntry*>(entry);
                if (processor->Reserved != 0)
                    return MadtInitializationError::InvalidEntry;

                statistics.ProcessorLocalX2ApicCount++;
                break;
            }

            default:
                statistics.UnhandledEntryCount++;
                break;
            }

            statistics.EntryCount++;
            offset += entry->Length;
        }

        if (offset != table_length)
            return MadtInitializationError::InvalidEntry;

        if (local_apic_address.IsNull())
            return MadtInitializationError::InvalidLocalApicAddress;

        /*
         * Publish the validated view only after the entire variable
         * record stream has been proven structurally sound.
         */
        m_Table = table;
        m_LocalApicAddress = local_apic_address;
        m_Statistics = statistics;
        m_LocalApicAddressOverridden = local_apic_address_overridden;
        m_Initialized = true;

        PrintTopology();
        return MadtInitializationError::Success;
    }

    const MadtEntryHeader* Madt::Entry(Uint64 index) const noexcept {
        if (!m_Initialized || index >= m_Statistics.EntryCount) return nullptr;

        const auto* bytes = reinterpret_cast<const Uint8*>(m_Table);
        const Uint64 table_length = m_Table->Header.Length;

        Uint64 current{};
        Uint64 offset = sizeof(MadtHeader);
        while (offset < table_length) {
            if (table_length - offset < sizeof(MadtEntryHeader)) return nullptr;

            const auto* entry = reinterpret_cast<const MadtEntryHeader*>(bytes + offset);
            if (entry->Length < sizeof(MadtEntryHeader) || static_cast<Uint64>(entry->Length) > table_length - offset)
                return nullptr;

            if (current == index) return entry;

            current++;
            offset += entry->Length;
        }

        return nullptr;
    }

    const MadtEntryHeader* Madt::FindEntry(MadtEntryType type, Uint64 occurrence) const noexcept {
        if (!m_Initialized) return nullptr;

        const auto* bytes = reinterpret_cast<const Uint8*>(m_Table);
        const Uint64 table_length = m_Table->Header.Length;

        Uint64 match{};
        Uint64 offset = sizeof(MadtHeader);
        while (offset < table_length) {
            if (table_length - offset < sizeof(MadtEntryHeader)) return nullptr;

            const auto* entry = reinterpret_cast<const MadtEntryHeader*>(bytes + offset);
            if (entry->Length < sizeof(MadtEntryHeader) || static_cast<Uint64>(entry->Length) > table_length - offset)
                return nullptr;
            
            if (entry->Type == type) {
                if (match == occurrence) 
                    return entry;
                match++;
            }

            offset += entry->Length;
        }

        return nullptr;
    }

    const ProcessorLocalApicEntry* Madt::ProcessorLocalApic(Uint64 occurrence) const noexcept {
        const MadtEntryHeader* entry = FindEntry(MadtEntryType::ProcessorLocalApic, occurrence);
        if (entry == nullptr || entry->Length < sizeof(ProcessorLocalApicEntry))
            return nullptr;

        return reinterpret_cast<const ProcessorLocalApicEntry*>(entry);
    }

    const ProcessorLocalX2ApicEntry* Madt::ProcessorLocalX2Apic(Uint64 occurrence) const noexcept {
        const MadtEntryHeader* entry = FindEntry(MadtEntryType::ProcessorLocalX2Apic, occurrence);
        if (entry == nullptr || entry->Length < sizeof(ProcessorLocalX2ApicEntry))
            return nullptr;

        return reinterpret_cast<const ProcessorLocalX2ApicEntry*>(entry);
    }

    const IoApicEntry* Madt::IoApic(Uint64 occurrence) const noexcept {
        const MadtEntryHeader* entry = FindEntry(MadtEntryType::IoApic, occurrence);
        if (entry == nullptr || entry->Length < sizeof(IoApicEntry))
            return nullptr;

        return reinterpret_cast<const IoApicEntry*>(entry);
    }

    const InterruptSourceOverrideEntry* Madt::InterruptSourceOverride(Uint64 occurrence) const noexcept {
        const MadtEntryHeader* entry = FindEntry(MadtEntryType::InterruptSourceOverride, occurrence);
        if (entry == nullptr || entry->Length < sizeof(InterruptSourceOverrideEntry))
            return nullptr;

        return reinterpret_cast<const InterruptSourceOverrideEntry*>(entry);
    }

    void Madt::PrintTopology() const noexcept {
        Diagnostics::Write("[zOS/ACPI] MADT Local APIC base: ");
        Diagnostics::WriteHex(m_LocalApicAddress.Value());

        if (m_LocalApicAddressOverridden)
            Diagnostics::Write(" (overridden)");

        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/ACPI] Legacy 8259 compatibility: ");
        Diagnostics::Write(HasLegacy8259() ? "present\n" : "not present\n");

        const auto* bytes = reinterpret_cast<const Uint8*>(m_Table);
        const Uint64 table_length = m_Table->Header.Length;

        Uint64 offset = sizeof(MadtHeader);

        while (offset < table_length) {
            const auto* entry = reinterpret_cast<const MadtEntryHeader*>(bytes + offset);

            switch (entry->Type) {
            case MadtEntryType::ProcessorLocalApic: {
                const auto* processor = reinterpret_cast<const ProcessorLocalApicEntry*>(entry);

                Diagnostics::Write("[zOS/ACPI] Processor LAPIC: UID ");
                Diagnostics::WriteDecimal(processor->AcpiProcessorUid);
                Diagnostics::Write(", APIC ID ");
                Diagnostics::WriteDecimal(processor->ApicId);
                Diagnostics::Write(", ");
                WriteProcessorState(processor->Flags);
                Diagnostics::Write("\n");
                break;
            }

            case MadtEntryType::ProcessorLocalX2Apic: {
                const auto* processor = reinterpret_cast<const ProcessorLocalX2ApicEntry*>(entry);

                Diagnostics::Write("[zOS/ACPI] Processor x2APIC: UID ");
                Diagnostics::WriteDecimal(processor->AcpiProcessorUid);
                Diagnostics::Write(", APIC ID ");
                Diagnostics::WriteDecimal(processor->X2ApicId);
                Diagnostics::Write(", ");
                WriteProcessorState(processor->Flags);
                Diagnostics::Write("\n");
                break;
            }

            case MadtEntryType::IoApic: {
                const auto* io_apic = reinterpret_cast<const IoApicEntry*>(entry);

                Diagnostics::Write("[zOS/ACPI] I/O APIC: ID ");
                Diagnostics::WriteDecimal(io_apic->IoApicId);
                Diagnostics::Write(", address ");
                Diagnostics::WriteHex(io_apic->IoApicAddress);
                Diagnostics::Write(", GSI base ");
                Diagnostics::WriteDecimal(io_apic->GlobalSystemInterruptBase);
                Diagnostics::Write("\n");
                break;
            }

            case MadtEntryType::InterruptSourceOverride: {
                const auto* override_entry = reinterpret_cast<const InterruptSourceOverrideEntry*>(entry);

                Diagnostics::Write("[zOS/ACPI] IRQ override: bus ");
                Diagnostics::WriteDecimal(override_entry->Bus);
                Diagnostics::Write(", source ");
                Diagnostics::WriteDecimal(override_entry->Source);
                Diagnostics::Write(" -> GSI ");
                Diagnostics::WriteDecimal(override_entry->GlobalSystemInterrupt);
                Diagnostics::Write(", flags ");
                Diagnostics::WriteHex(override_entry->Flags);
                Diagnostics::Write("\n");
                break;
            }

            case MadtEntryType::LocalApicAddressOverride:
                break;

            default:
                Diagnostics::Write("[zOS/ACPI] Unhandled MADT record: type ");
                Diagnostics::WriteDecimal(static_cast<Uint8>(entry->Type));
                Diagnostics::Write(", length ");
                Diagnostics::WriteDecimal(entry->Length);
                Diagnostics::Write("\n");
                break;
            }

            offset += entry->Length;
        }

        Diagnostics::Write("[zOS/ACPI] MADT entries: ");
        Diagnostics::WriteDecimal(m_Statistics.EntryCount);
        Diagnostics::Write(", unhandled: ");
        Diagnostics::WriteDecimal(m_Statistics.UnhandledEntryCount);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/ACPI] MADT topology discovery complete.\n");
    }

    const char* Madt::Describe(MadtInitializationError error) noexcept {
        switch (error) {
        case MadtInitializationError::Success: return "success";
        case MadtInitializationError::AlreadyInitialized: return "MADT is already initialized";
        case MadtInitializationError::AcpiUnavailable: return "ACPI table directory is not initialized";
        case MadtInitializationError::TableNotFound: return "ACPI MADT was not found";
        case MadtInitializationError::TableTooSmall: return "ACPI MADT is smaller than its fixed header";
        case MadtInitializationError::InvalidEntry: return "ACPI MADT contains an invalid entry";
        case MadtInitializationError::MultipleLocalApicAddressOverrides: return "ACPI MADT contains multiple Local APIC address overrides";
        case MadtInitializationError::InvalidLocalApicAddress: return "ACPI MADT does not provide a valid Local APIC address";
        default: return "unknown MADT initialization error";
        }
    }
}
