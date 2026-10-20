#pragma once

#include <Kernel/Platform/ACPI/ACPI.hpp>

namespace Zos::Kernel::Platform::ACPI {
    using Memory::Uint8;
    using Memory::Uint16;
    using Memory::Uint32;
    using Memory::Uint64;

    inline constexpr TableSignature MadtSignature{ 'A', 'P', 'I', 'C' };

    inline constexpr Uint32 MadtPcatCompatibilityFlag{ 1u << 0 };
    inline constexpr Uint32 ProcessorEnabledFlag{ 1u << 0 };
    inline constexpr Uint32 ProcessorOnlineCapableFlag{ 1u << 1 };

    enum class MadtEntryType : Uint8 {
        ProcessorLocalApic = 0,
        IoApic = 1,
        InterruptSourceOverride = 2,
        LocalApicAddressOverride = 5,
        ProcessorLocalX2Apic = 9,
    };

    struct __attribute__((packed)) MadtHeader final {
        DescriptionHeader Header;
        Uint32 LocalApicAddress;
        Uint32 Flags;
    };

    struct __attribute__((packed)) MadtEntryHeader final {
        MadtEntryType Type;
        Uint8 Length;
    };

    struct __attribute__((packed)) ProcessorLocalApicEntry final {
        MadtEntryHeader Header;
        Uint8 AcpiProcessorUid;
        Uint8 ApicId;
        Uint32 Flags;
    };

    struct __attribute__((packed)) IoApicEntry final {
        MadtEntryHeader Header;
        Uint8 IoApicId;
        Uint8 Reserved;
        Uint32 IoApicAddress;
        Uint32 GlobalSystemInterruptBase;
    };

    struct __attribute__((packed)) InterruptSourceOverrideEntry final {
        MadtEntryHeader Header;
        Uint8 Bus;
        Uint8 Source;
        Uint32 GlobalSystemInterrupt;
        Uint16 Flags;
    };

    struct __attribute__((packed)) LocalApicAddressOverrideEntry final {
        MadtEntryHeader Header;
        Uint16 Reserved;
        Uint64 LocalApicAddress;
    };

    struct __attribute__((packed)) ProcessorLocalX2ApicEntry final {
        MadtEntryHeader Header;
        Uint16 Reserved;
        Uint32 X2ApicId;
        Uint32 Flags;
        Uint32 AcpiProcessorUid;
    };

    struct MadtStatistics final {
        Uint64 EntryCount{};
        Uint64 ProcessorLocalApicCount{};
        Uint64 ProcessorLocalX2ApicCount{};
        Uint64 IoApicCount{};
        Uint64 InterruptSourceOverrideCount{};
        Uint64 UnhandledEntryCount{};
    };

    enum class MadtInitializationError : Uint32 {
        Success,
        AlreadyInitialized,
        AcpiUnavailable,
        TableNotFound,
        TableTooSmall,
        InvalidEntry,
        MultipleLocalApicAddressOverrides,
        InvalidLocalApicAddress,
    };

    class Madt final {
    public:
        constexpr Madt() noexcept = default;
        Madt(const Madt&) = delete;
        Madt& operator=(const Madt&) = delete;

        [[nodiscard]] MadtInitializationError Initialize(const TableDirectory& tables) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }

        [[nodiscard]] Memory::PhysicalAddress LocalApicAddress() const noexcept {
            return m_LocalApicAddress;
        }

        [[nodiscard]] bool LocalApicAddressOverridden() const noexcept {
            return m_Initialized && m_LocalApicAddressOverridden;
        }

        [[nodiscard]] bool HasLegacy8259() const noexcept {
            return m_Initialized && (m_Table->Flags & MadtPcatCompatibilityFlag) != 0;
        }

        [[nodiscard]] const MadtStatistics& Statistics() const noexcept {
            return m_Statistics;
        }

        [[nodiscard]] const MadtEntryHeader* Entry(Uint64 index) const noexcept;

        [[nodiscard]] const ProcessorLocalApicEntry* ProcessorLocalApic(Uint64 occurrence = 0) const noexcept;
        [[nodiscard]] const ProcessorLocalX2ApicEntry* ProcessorLocalX2Apic(Uint64 occurrence = 0) const noexcept;
        [[nodiscard]] const IoApicEntry* IoApic(Uint64 occurrence = 0) const noexcept;
        [[nodiscard]] const InterruptSourceOverrideEntry* InterruptSourceOverride(Uint64 occurrence = 0) const noexcept;

        [[nodiscard]] static const char* Describe(MadtInitializationError error) noexcept;

    private:
        [[nodiscard]] const MadtEntryHeader* FindEntry(MadtEntryType type, Uint64 occurrence) const noexcept;

        void PrintTopology() const noexcept;

        const MadtHeader* m_Table{};
        Memory::PhysicalAddress m_LocalApicAddress{};
        MadtStatistics m_Statistics{};
        bool m_LocalApicAddressOverridden{};
        bool m_Initialized{};
    };

    [[nodiscard]] constexpr bool IsProcessorEnabled(Uint32 flags) noexcept {
        return (flags & ProcessorEnabledFlag) != 0;
    }

    [[nodiscard]] constexpr bool IsProcessorOnlineCapable(Uint32 flags) noexcept {
        return (flags & ProcessorOnlineCapableFlag) != 0;
    }

    static_assert(sizeof(MadtHeader) == 44);
    static_assert(sizeof(MadtEntryHeader) == 2);
    static_assert(sizeof(ProcessorLocalApicEntry) == 8);
    static_assert(sizeof(IoApicEntry) == 12);
    static_assert(sizeof(InterruptSourceOverrideEntry) == 10);
    static_assert(sizeof(LocalApicAddressOverrideEntry) == 12);
    static_assert(sizeof(ProcessorLocalX2ApicEntry) == 16);
}