#include <Kernel/Platform/HPET.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Platform {
    namespace {
        using Memory::MaximumValue;
        using Memory::PageSize;
        using Memory::PhysicalAddress;
        using Memory::Uint32;
        using Memory::Uint64;
        using Memory::VirtualAddress;

        inline constexpr Memory::Uint8 SystemMemoryAddressSpace{ 0 };

        inline constexpr Uint64 RegisterBlockSize{ 0x400 };

        inline constexpr Uint64 GeneralCapabilitiesOffset{ 0x000 };
        inline constexpr Uint64 GeneralConfigurationOffset{ 0x010 };
        inline constexpr Uint64 MainCounterOffset{ 0x0F0 };

        inline constexpr Uint64 TimerConfigurationBase{ 0x100 };
        inline constexpr Uint64 TimerRegisterStride{ 0x20 };

        inline constexpr Uint64 GeneralEnableBit{ 1ULL << 0 };
        inline constexpr Uint64 LegacyReplacementRouteBit{ 1ULL << 1 };

        inline constexpr Uint64 TimerInterruptEnableBit{ 1ULL << 2 };
        inline constexpr Uint64 TimerFsbEnableBit{ 1ULL << 14 };
        inline constexpr Uint64 TimerReservedBits31To16{ 0x00000000FFFF0000ULL };

        inline constexpr Uint64 Counter64BitCapability{ 1ULL << 13 };

        inline constexpr Uint32 MaximumClockPeriodFemtoseconds{ 100'000'000u };
        inline constexpr Uint64 FemtosecondsPerSecond{ 1'000'000'000'000'000ULL };
        inline constexpr Uint64 FemtosecondsPerNanosecond{ 1'000'000ULL };

        inline constexpr Uint64 CounterAdvanceProbeReads{ 1'000'000 };

        [[nodiscard]] Uint64 SaturatingAdd(Uint64 left, Uint64 right) noexcept {
            if (right > MaximumValue - left) return MaximumValue;
            return left + right;
        }

        [[nodiscard]] Uint64 SaturatingMultiply(Uint64 left, Uint64 right) noexcept {
            if (left == 0 || right == 0) return 0;
            if (left > MaximumValue / right) return MaximumValue;
            return left * right;
        }
    }

    HpetInitializationError Hpet::Initialize(const ACPI::TableDirectory& tables, Memory::VirtualAddressAllocator& mmio_addresses, Architecture::AMD64::PageMap& page_map) noexcept {
        if (m_Initialized) return HpetInitializationError::AlreadyInitialized;

        if (!tables.IsInitialized() || !mmio_addresses.IsInitialized() || !mmio_addresses.IsMetadataPromoted() ||
            !page_map.IsInitialized() || !page_map.IsActive())
            return HpetInitializationError::InvalidDependency;

        const ACPI::DescriptionHeader* header = tables.FindTable(ACPI::HpetSignature);
        if (header == nullptr) return HpetInitializationError::TableTooSmall;
        if (header->Length < sizeof(ACPI::HpetDescriptionTable)) 
            return HpetInitializationError::TableTooSmall;

        const auto* table = reinterpret_cast<const ACPI::HpetDescriptionTable*>(header);
        if (table->BaseAddress.AddressSpaceId != SystemMemoryAddressSpace)
            return HpetInitializationError::UnsupportedAddressSpace;

        const Uint64 physical_base = table->BaseAddress.Address;
        if (physical_base == 0 || (physical_base & (sizeof(Uint64) - 1)) != 0) 
            return HpetInitializationError::InvalidBaseAddress;

        const Uint64 aligned_base = physical_base & ~(PageSize - 1);
        const Uint64 page_offset = physical_base - aligned_base;
        if (page_offset > MaximumValue - RegisterBlockSize)
            return HpetInitializationError::InvalidBaseAddress;

        const Uint64 mapped_bytes = page_offset + RegisterBlockSize;
        if (mapped_bytes > MaximumValue - (PageSize - 1))
            return HpetInitializationError::InvalidBaseAddress;

        const Uint64 page_count = (mapped_bytes + PageSize - 1) / PageSize;
        const Memory::MmioMappingError mapping_error = m_MmioMapping.Initialize(mmio_addresses, page_map, PhysicalAddress(aligned_base), page_count);
        if (mapping_error != Memory::MmioMappingError::Success)
            return HpetInitializationError::MappingFailed;

        m_PhysicalBase = PhysicalAddress(physical_base);
        m_RegisterBase = m_MmioMapping.Base() + page_offset;
        
        const Uint64 capabilities = ReadRegister(GeneralCapabilitiesOffset);
        if (static_cast<Uint32>(capabilities) != table->EventTimerBlockId) 
            return HpetInitializationError::CapabilitiesMismatch;

        const Uint32 revision = static_cast<Uint32>(capabilities & 0xFFu);
        if (revision == 0)
            return HpetInitializationError::CapabilitiesMismatch;

        m_TimerCount = static_cast<Uint32>((capabilities >> 8) & 0x1Fu) + 1;
        if (m_TimerCount == 0 || m_TimerCount > 32)
            return HpetInitializationError::InvalidTimerCount;

        m_Counter64Bit = (capabilities & Counter64BitCapability) != 0;
        m_ClockPeriodFemtoseconds = static_cast<Uint32>(capabilities >> 32);

        if (m_ClockPeriodFemtoseconds == 0 || m_ClockPeriodFemtoseconds > MaximumClockPeriodFemtoseconds)
            return HpetInitializationError::InvalidClockPeriod;

        /*
         * Step 6 uses only the free-running main counter. Disable every
         * comparator's interrupt-delivery mechanisms before enabling the
         * counter so firmware state cannot unexpectedly inject an IRQ.
         */
        for (Uint32 timer = 0; timer < m_TimerCount; timer++) {
            const Uint64 offset = TimerConfigurationBase + static_cast<Uint64>(timer) * TimerRegisterStride;
            Uint64 configuration = ReadRegister(offset);

            configuration &= ~TimerInterruptEnableBit;
            configuration &= ~TimerFsbEnableBit;
            configuration &= ~TimerReservedBits31To16;

            WriteRegister(offset, configuration);

            const Uint64 verified = ReadRegister(offset);
            if ((verified & (TimerInterruptEnableBit | TimerFsbEnableBit)) != 0)
                return HpetInitializationError::ConfigurationFailed;
        }

        Uint64 general_configuration = ReadRegister(GeneralConfigurationOffset);
        general_configuration &= ~LegacyReplacementRouteBit;
        general_configuration |= GeneralEnableBit;
        WriteRegister(GeneralConfigurationOffset, general_configuration);

        const Uint64 verified_configuration = ReadRegister(GeneralConfigurationOffset);
        if ((verified_configuration & GeneralEnableBit) == 0 ||
            (verified_configuration & LegacyReplacementRouteBit) != 0)
            return HpetInitializationError::ConfigurationFailed;

        const Uint64 first_counter = ReadCounter();
        bool advanced = false;

        for (Uint64 attempt = 0; attempt < CounterAdvanceProbeReads; attempt++) {
            if (ReadCounter() != first_counter) {
                advanced = true;
                break;
            }
        }

        if (!advanced)
            return HpetInitializationError::CounterNotAdvancing;

        m_Initialized = true;
        Print();

        return HpetInitializationError::Success;
    }

    Uint64 Hpet::ReadRegister(Uint64 offset) const noexcept {
        const VirtualAddress address = m_RegisterBase + offset;
        return *reinterpret_cast<volatile const Uint64*>(address.Value());
    }

    void Hpet::WriteRegister(Uint64 offset, Uint64 value) noexcept {
        const VirtualAddress address = m_RegisterBase + offset;
        *reinterpret_cast<volatile Uint64*>(address.Value()) = value;
    }

    Uint64 Hpet::ReadCounter() const noexcept {
        if (!m_Initialized && m_RegisterBase.IsNull()) return 0;

        const Uint64 value = ReadRegister(MainCounterOffset);
        return m_Counter64Bit ? value : static_cast<Uint32>(value);
    }

    Uint64 Hpet::ElapsedTicks(Uint64 start, Uint64 end) const noexcept {
        if (m_Counter64Bit) return end - start;

        const Uint32 start32 = static_cast<Uint32>(start);
        const Uint32 end32 = static_cast<Uint32>(end);
        return static_cast<Uint32>(end32 - start32);
    }

    Uint64 Hpet::TicksToNanoseconds(Uint64 ticks) const noexcept {
        if (m_ClockPeriodFemtoseconds == 0) return 0;

        const Uint64 whole_nanoseconds_per_tick =
            m_ClockPeriodFemtoseconds / FemtosecondsPerNanosecond;

        const Uint64 remainder_femtoseconds_per_tick =
            m_ClockPeriodFemtoseconds % FemtosecondsPerNanosecond;

        Uint64 nanoseconds = SaturatingMultiply(ticks, whole_nanoseconds_per_tick);
        if (nanoseconds == MaximumValue) return MaximumValue;

        const Uint64 whole_groups = ticks / FemtosecondsPerNanosecond;
        const Uint64 remaining_ticks = ticks % FemtosecondsPerNanosecond;

        const Uint64 grouped_fraction =
            SaturatingMultiply(whole_groups, remainder_femtoseconds_per_tick);

        if (grouped_fraction == MaximumValue) return MaximumValue;

        const Uint64 tail_fraction =
            (remaining_ticks * remainder_femtoseconds_per_tick) / FemtosecondsPerNanosecond;

        nanoseconds = SaturatingAdd(nanoseconds, grouped_fraction);
        if (nanoseconds == MaximumValue) return MaximumValue;

        return SaturatingAdd(nanoseconds, tail_fraction);
    }

    Uint64 Hpet::FrequencyHz() const noexcept {
        if (m_ClockPeriodFemtoseconds == 0) return 0;
        return FemtosecondsPerSecond / m_ClockPeriodFemtoseconds;
    }

    void Hpet::Print() const noexcept {
        Diagnostics::Write("[zOS/HPET] Physical base: ");
        Diagnostics::WriteHex(m_PhysicalBase.Value());
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/HPET] MMIO virtual base: ");
        Diagnostics::WriteHex(m_RegisterBase.Value());
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/HPET] Main counter: ");
        Diagnostics::Write(m_Counter64Bit ? "64-bit\n" : "32-bit\n");

        Diagnostics::Write("[zOS/HPET] Clock period: ");
        Diagnostics::WriteDecimal(m_ClockPeriodFemtoseconds);
        Diagnostics::Write(" fs\n");

        Diagnostics::Write("[zOS/HPET] Frequency: ");
        Diagnostics::WriteDecimal(FrequencyHz());
        Diagnostics::Write(" Hz\n");

        Diagnostics::Write("[zOS/HPET] Comparators: ");
        Diagnostics::WriteDecimal(m_TimerCount);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/HPET] Main counter running.\n");
    }

    const char* Hpet::Describe(HpetInitializationError error) noexcept {
        switch (error) {
        case HpetInitializationError::Success: return "success";
        case HpetInitializationError::AlreadyInitialized: return "HPET is already initialized";
        case HpetInitializationError::InvalidDependency: return "HPET initialization dependencies are invalid";
        case HpetInitializationError::TableNotFound: return "ACPI does not provide an HPET description table";
        case HpetInitializationError::TableTooSmall: return "ACPI HPET description table is too small";
        case HpetInitializationError::UnsupportedAddressSpace: return "HPET is not exposed through system memory";
        case HpetInitializationError::InvalidBaseAddress: return "HPET register block address is invalid";
        case HpetInitializationError::MappingFailed: return "failed to map the HPET register block";
        case HpetInitializationError::CapabilitiesMismatch: return "HPET hardware capabilities do not match the ACPI description";
        case HpetInitializationError::InvalidClockPeriod: return "HPET reports an invalid main-counter period";
        case HpetInitializationError::InvalidTimerCount: return "HPET reports an invalid comparator count";
        case HpetInitializationError::ConfigurationFailed: return "failed to establish safe HPET counter configuration";
        case HpetInitializationError::CounterNotAdvancing: return "HPET main counter is not advancing";
        default: return "unknown HPET initialization error";
        }
    }
}