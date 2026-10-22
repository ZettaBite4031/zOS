#pragma once

#include <Kernel/Architecture/AMD64/CPU.hpp>
#include <Kernel/Architecture/AMD64/Interrupts.hpp>
#include <Kernel/Memory/MMIO.hpp>
#include <Kernel/Platform/ACPI/MADT.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    enum class LocalApicMode : Memory::Uint32 {
        XApic,
        X2Apic,
    };

    enum class LocalApicInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        ApicUnsupported,
        InvalidApicBaseState,
        NotBootstrapProcessor,
        X2ApicCapabilityMismatch,
        ModeTransitionFailed,
        LocalApicAddressMismatch,
        MmioMappingFailed,
        ProcessorIdentityUnavailable,
        ProcessorIdentityMismatch,
        ProcessorNotInMadt,
        SpuriousHandlerRegistrationFailed,
        RegisterValidationFailed,
    };

    enum class LocalApicTimerMode : Memory::Uint32 {
        OneShot,
        Periodic,
    };

    enum class LocalApicTimerDivide : Memory::Uint32 {
        By1,
        By2,
        By4,
        By8,
        By16,
        By32,
        By64,
        By128,
    };

    class LocalApic final {
    public:
        constexpr LocalApic() noexcept = default;

        LocalApic(const LocalApic&) = delete;
        LocalApic& operator=(const LocalApic&) = delete;

        [[nodiscard]] LocalApicInitializationError Initialize(
            const CpuInfo& cpu,
            const Platform::ACPI::Madt& madt,
            InterruptManager& interrupts,
            Memory::VirtualAddressAllocator& mmio_addresses,
            PageMap& page_map
        ) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] LocalApicMode Mode() const noexcept { return m_Mode; }
        [[nodiscard]] Memory::Uint32 Id() const noexcept { return m_Id; }
        [[nodiscard]] Memory::PhysicalAddress PhysicalBase() const noexcept { return m_PhysicalBase; }
        [[nodiscard]] Memory::VirtualAddress MmioBase() const noexcept { return m_MmioMapping.Base(); }

        void EndOfInterrupt() noexcept;

        [[nodiscard]] bool ConfigureTimer(Memory::Uint8 vector, LocalApicTimerMode mode, LocalApicTimerDivide divide, bool masked) noexcept;

        [[nodiscard]] bool SetTimerMasked(bool masked) noexcept;

        void SetTimerInitialCount(Memory::Uint32 count) noexcept;

        [[nodiscard]] Memory::Uint32 TimerCurrentCount() const noexcept;

        [[nodiscard]] static const char* ModeName(LocalApicMode mode) noexcept;
        [[nodiscard]] static const char* Describe(LocalApicInitializationError error) noexcept;

    private:
        [[nodiscard]] Memory::Uint32 ReadRegister(Memory::Uint32 offset) const noexcept;
        void WriteRegister(Memory::Uint32 offset, Memory::Uint32 value) noexcept;

        [[nodiscard]] static bool MadtContainsEnabledProcessor(const Platform::ACPI::Madt& madt, Memory::Uint32 apic_id) noexcept;
        static void HandleSpuriousInterrupt(InterruptContext&) noexcept;

        void Print() const noexcept;

        Memory::MmioMapping m_MmioMapping{};
        Memory::PhysicalAddress m_PhysicalBase{};

        LocalApicMode m_Mode{ LocalApicMode::XApic };
        Memory::Uint32 m_Id{};

        bool m_Initialized{};
    };
}