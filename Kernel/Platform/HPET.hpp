#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/MMIO.hpp>
#include <Kernel/Platform/ACPI/ACPI.hpp>

namespace Zos::Kernel::Platform::ACPI {
    inline constexpr TableSignature HpetSignature{ 'H', 'P', 'E', 'T' };

    struct __attribute__((packed)) HpetDescriptionTable final {
        DescriptionHeader Header;
        Memory::Uint32 EventTimerBlockId;
        GenericAddressStructure BaseAddress;
        Memory::Uint8 HpetNumber;
        Memory::Uint16 MinimumClockTick;
        Memory::Uint8 PageProtectionAndOem;
    };

    static_assert(sizeof(HpetDescriptionTable) == 56);
}

namespace Zos::Kernel::Platform {
    enum class HpetInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        TableNotFound,
        TableTooSmall,
        UnsupportedAddressSpace,
        InvalidBaseAddress,
        MappingFailed,
        CapabilitiesMismatch,
        InvalidClockPeriod,
        InvalidTimerCount,
        ConfigurationFailed,
        CounterNotAdvancing,
    };

    class Hpet final {
    public:
        constexpr Hpet() noexcept = default;

        Hpet(const Hpet&) = delete;
        Hpet& operator=(const Hpet&) = delete;

        [[nodiscard]] HpetInitializationError Initialize(
            const ACPI::TableDirectory& tables,
            Memory::VirtualAddressAllocator& mmio_addresses,
            Architecture::AMD64::PageMap& page_map
        ) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }

        [[nodiscard]] Memory::PhysicalAddress PhysicalBase() const noexcept { return m_PhysicalBase; }
        [[nodiscard]] Memory::VirtualAddress RegisterBase() const noexcept { return m_RegisterBase; }

        [[nodiscard]] Memory::Uint32 ClockPeriodFemtoseconds() const noexcept { return m_ClockPeriodFemtoseconds; }
        [[nodiscard]] Memory::Uint64 FrequencyHz() const noexcept;
        [[nodiscard]] Memory::Uint32 TimerCount() const noexcept { return m_TimerCount; }
        [[nodiscard]] bool Has64BitCounter() const noexcept { return m_Counter64Bit; }

        [[nodiscard]] Memory::Uint64 ReadCounter() const noexcept;
        [[nodiscard]] Memory::Uint64 ElapsedTicks(Memory::Uint64 start, Memory::Uint64 end) const noexcept;
        [[nodiscard]] Memory::Uint64 TicksToNanoseconds(Memory::Uint64 ticks) const noexcept;

        [[nodiscard]] static const char* Describe(HpetInitializationError error) noexcept;

    private:
        [[nodiscard]] Memory::Uint64 ReadRegister(Memory::Uint64 offset) const noexcept;
        void WriteRegister(Memory::Uint64 offset, Memory::Uint64 value) noexcept;

        void Print() const noexcept;

        Memory::MmioMapping m_MmioMapping{};
        Memory::PhysicalAddress m_PhysicalBase{};
        Memory::VirtualAddress m_RegisterBase{};

        Memory::Uint32 m_ClockPeriodFemtoseconds{};
        Memory::Uint32 m_TimerCount{};

        bool m_Counter64Bit{};
        bool m_Initialized{};
    };
}
