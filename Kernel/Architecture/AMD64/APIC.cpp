#include <Kernel/Architecture/AMD64/APIC.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    namespace {
        using Memory::PhysicalAddress;
        using Memory::Uint32;
        using Memory::Uint64;

        inline constexpr Uint32 ApicBaseMsr{ 0x1Bu };

        inline constexpr Uint64 ApicBaseBootstrapProcessor{ 1ULL << 8 };
        inline constexpr Uint64 ApicBaseX2ApicEnable{ 1ULL << 10 };
        inline constexpr Uint64 ApicBaseGlobalEnable{ 1ULL << 11 };
        inline constexpr Uint64 ApicBaseAddressMask{ 0x000FFFFFFFFFF000ULL };

        inline constexpr Uint32 IdRegisterOffset{ 0x020u };
        inline constexpr Uint32 TaskPriorityRegisterOffset{ 0x080u };
        inline constexpr Uint32 EndOfInterruptRegisterOffset{ 0x0B0u };
        inline constexpr Uint32 SpuriousVectorRegisterOffset{ 0x0F0u };

        inline constexpr Uint32 X2ApicMsrBase{ 0x800u };

        inline constexpr Uint32 SpuriousVectorMask{ 0xFFu };
        inline constexpr Uint32 SoftwareEnableBit{ 1u << 8 };

        [[nodiscard]] constexpr Uint32 X2ApicMsrForOffset(Uint32 offset) noexcept {
            return X2ApicMsrBase + (offset >> 4);
        }

        [[nodiscard]] constexpr PhysicalAddress ApicPhysicalBase(Uint64 apic_base) noexcept {
            return PhysicalAddress(apic_base & ApicBaseAddressMask);
        }
    }

    LocalApicInitializationError LocalApic::Initialize(const CpuInfo& cpu, const Platform::ACPI::Madt& madt, InterruptManager& interrupts, Memory::VirtualAddressAllocator& mmio_addresses, PageMap& page_map) noexcept {
        if (m_Initialized) return LocalApicInitializationError::AlreadyInitialized;

        if (!cpu.IsInitialized() || !madt.IsInitialized() || !interrupts.IsInitialized() ||
            !mmio_addresses.IsInitialized() || !mmio_addresses.IsMetadataPromoted() ||
            !page_map.IsInitialized() || !page_map.IsActive())
            return LocalApicInitializationError::InvalidDependency;

        if (!cpu.Features().Apic)
            return LocalApicInitializationError::ApicUnsupported;

        Uint64 apic_base = ReadMsr(ApicBaseMsr);

        bool globally_enabled = (apic_base & ApicBaseGlobalEnable) != 0;
        bool x2apic_enabled = (apic_base & ApicBaseX2ApicEnable) != 0;

        if (x2apic_enabled && !globally_enabled)
            return LocalApicInitializationError::InvalidApicBaseState;

        if ((apic_base & ApicBaseBootstrapProcessor) == 0)
            return LocalApicInitializationError::NotBootstrapProcessor;

        if (cpu.Features().X2Apic) {
            /*
             * The architectural transition from disabled state to x2APIC
             * mode is two-stage: enter xAPIC mode first, then set EXTD.
             */
            if (!globally_enabled) {
                WriteMsr(ApicBaseMsr, (apic_base | ApicBaseGlobalEnable) & ~ApicBaseX2ApicEnable);
                apic_base = ReadMsr(ApicBaseMsr);

                globally_enabled = (apic_base & ApicBaseGlobalEnable) != 0;
                x2apic_enabled = (apic_base & ApicBaseX2ApicEnable) != 0;

                if (!globally_enabled || x2apic_enabled)
                    return LocalApicInitializationError::ModeTransitionFailed;
            }

            if (!x2apic_enabled) {
                WriteMsr(ApicBaseMsr, apic_base | ApicBaseGlobalEnable | ApicBaseX2ApicEnable);
                apic_base = ReadMsr(ApicBaseMsr);

                globally_enabled = (apic_base & ApicBaseGlobalEnable) != 0;
                x2apic_enabled = (apic_base & ApicBaseX2ApicEnable) != 0;

                if (!globally_enabled || !x2apic_enabled)
                    return LocalApicInitializationError::ModeTransitionFailed;
            }

            m_Mode = LocalApicMode::X2Apic;
            m_PhysicalBase = ApicPhysicalBase(apic_base);
        } else {
            if (x2apic_enabled)
                return LocalApicInitializationError::X2ApicCapabilityMismatch;

            if (!globally_enabled) {
                WriteMsr(ApicBaseMsr, (apic_base | ApicBaseGlobalEnable) & ~ApicBaseX2ApicEnable);
                apic_base = ReadMsr(ApicBaseMsr);

                globally_enabled = (apic_base & ApicBaseGlobalEnable) != 0;
                x2apic_enabled = (apic_base & ApicBaseX2ApicEnable) != 0;

                if (!globally_enabled || x2apic_enabled)
                    return LocalApicInitializationError::ModeTransitionFailed;
            }

            m_Mode = LocalApicMode::XApic;
            m_PhysicalBase = ApicPhysicalBase(apic_base);

            if (m_PhysicalBase.IsNull() || m_PhysicalBase != madt.LocalApicAddress())
                return LocalApicInitializationError::LocalApicAddressMismatch;

            const Memory::MmioMappingError mapping_error = m_MmioMapping.Initialize(
                mmio_addresses,
                page_map,
                m_PhysicalBase,
                1
            );

            if (mapping_error != Memory::MmioMappingError::Success)
                return LocalApicInitializationError::MmioMappingFailed;
        }

        if (!cpu.HasBootstrapApicId())
            return LocalApicInitializationError::ProcessorIdentityUnavailable;

        const Uint32 id_register = ReadRegister(IdRegisterOffset);
        m_Id = m_Mode == LocalApicMode::X2Apic ? id_register : id_register >> 24;

        if (m_Id != cpu.BootstrapApicId())
            return LocalApicInitializationError::ProcessorIdentityMismatch;

        if (!MadtContainsEnabledProcessor(madt, m_Id))
            return LocalApicInitializationError::ProcessorNotInMadt;

        if (!interrupts.RegisterHandler(LocalApicSpuriousVector, &HandleSpuriousInterrupt, "Local APIC spurious interrupt"))
            return LocalApicInitializationError::SpuriousHandlerRegistrationFailed;

        /*
         * Begin from an unrestricted task priority. Individual interrupt
         * sources remain disabled/routed elsewhere until later milestones.
         */
        WriteRegister(TaskPriorityRegisterOffset, 0);
        if ((ReadRegister(TaskPriorityRegisterOffset) & 0xFFu) != 0)
            return LocalApicInitializationError::RegisterValidationFailed;

        Uint32 spurious = ReadRegister(SpuriousVectorRegisterOffset);
        spurious &= ~SpuriousVectorMask;
        spurious |= LocalApicSpuriousVector;
        spurious |= SoftwareEnableBit;

        WriteRegister(SpuriousVectorRegisterOffset, spurious);

        const Uint32 configured_spurious = ReadRegister(SpuriousVectorRegisterOffset);
        if ((configured_spurious & SpuriousVectorMask) != LocalApicSpuriousVector ||
            (configured_spurious & SoftwareEnableBit) == 0)
            return LocalApicInitializationError::RegisterValidationFailed;

        m_Initialized = true;
        Print();

        return LocalApicInitializationError::Success;
    }

    Uint32 LocalApic::ReadRegister(Uint32 offset) const noexcept {
        if (m_Mode == LocalApicMode::X2Apic)
            return static_cast<Uint32>(ReadMsr(X2ApicMsrForOffset(offset)));

        const auto address = m_MmioMapping.Base() + offset;
        return *reinterpret_cast<volatile const Uint32*>(address.Value());
    }

    void LocalApic::WriteRegister(Uint32 offset, Uint32 value) noexcept {
        if (m_Mode == LocalApicMode::X2Apic) {
            WriteMsr(X2ApicMsrForOffset(offset), value);
            return;
        }

        const auto address = m_MmioMapping.Base() + offset;
        *reinterpret_cast<volatile Uint32*>(address.Value()) = value;
    }

    void LocalApic::EndOfInterrupt() noexcept {
        if (!m_Initialized) return;
        WriteRegister(EndOfInterruptRegisterOffset, 0);
    }

    bool LocalApic::MadtContainsEnabledProcessor(const Platform::ACPI::Madt& madt, Uint32 apic_id) noexcept {
        const Platform::ACPI::MadtStatistics& statistics = madt.Statistics();

        for (Memory::Uint64 i = 0; i < statistics.ProcessorLocalX2ApicCount; i++) {
            const Platform::ACPI::ProcessorLocalX2ApicEntry* processor = madt.ProcessorLocalX2Apic(i);
            if (processor == nullptr) continue;

            if (Platform::ACPI::IsProcessorEnabled(processor->Flags) &&
                processor->X2ApicId == apic_id)
                return true;
        }

        for (Memory::Uint64 i = 0; i < statistics.ProcessorLocalApicCount; i++) {
            const Platform::ACPI::ProcessorLocalApicEntry* processor = madt.ProcessorLocalApic(i);
            if (processor == nullptr) continue;

            if (Platform::ACPI::IsProcessorEnabled(processor->Flags) &&
                static_cast<Uint32>(processor->ApicId) == apic_id)
                return true;
        }

        return false;
    }

    void LocalApic::HandleSpuriousInterrupt(InterruptContext&) noexcept {
        /*
         * A true Local APIC spurious interrupt does not set an ISR bit,
         * so software must not issue an EOI for this vector.
         */
    }

    void LocalApic::Print() const noexcept {
        Diagnostics::Write("[zOS/APIC] Local APIC mode: ");
        Diagnostics::Write(ModeName(m_Mode));
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/APIC] Physical base: ");
        Diagnostics::WriteHex(m_PhysicalBase.Value());
        Diagnostics::Write("\n");

        if (m_Mode == LocalApicMode::XApic) {
            Diagnostics::Write("[zOS/APIC] MMIO virtual base: ");
            Diagnostics::WriteHex(m_MmioMapping.Base().Value());
            Diagnostics::Write("\n");
        }

        Diagnostics::Write("[zOS/APIC] BSP APIC ID: ");
        Diagnostics::WriteDecimal(m_Id);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/APIC] Spurious vector: ");
        Diagnostics::WriteHex(LocalApicSpuriousVector);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/APIC] Local APIC initialized.\n");
    }

    const char* LocalApic::ModeName(LocalApicMode mode) noexcept {
        switch (mode) {
        case LocalApicMode::XApic: return "xAPIC";
        case LocalApicMode::X2Apic: return "x2APIC";
        default: return "unknown";
        }
    }

    const char* LocalApic::Describe(LocalApicInitializationError error) noexcept {
        switch (error) {
        case LocalApicInitializationError::Success: return "success";
        case LocalApicInitializationError::AlreadyInitialized: return "Local APIC is already initialized";
        case LocalApicInitializationError::InvalidDependency: return "Local APIC initialization dependencies are invalid";
        case LocalApicInitializationError::ApicUnsupported: return "processor does not support the architectural Local APIC";
        case LocalApicInitializationError::InvalidApicBaseState: return "APIC Base MSR contains an invalid mode state";
        case LocalApicInitializationError::NotBootstrapProcessor: return "Local APIC initialization is not executing on the bootstrap processor";
        case LocalApicInitializationError::X2ApicCapabilityMismatch: return "APIC Base MSR reports x2APIC mode without CPUID x2APIC support";
        case LocalApicInitializationError::ModeTransitionFailed: return "failed to enter the requested Local APIC mode";
        case LocalApicInitializationError::LocalApicAddressMismatch: return "MADT and APIC Base MSR disagree on the Local APIC physical address";
        case LocalApicInitializationError::MmioMappingFailed: return "failed to establish the xAPIC MMIO mapping";
        case LocalApicInitializationError::ProcessorIdentityUnavailable: return "bootstrap processor APIC identity is unavailable";
        case LocalApicInitializationError::ProcessorIdentityMismatch: return "Local APIC ID does not match the CPUID bootstrap processor ID";
        case LocalApicInitializationError::ProcessorNotInMadt: return "Local APIC ID is not an enabled processor in the MADT";
        case LocalApicInitializationError::SpuriousHandlerRegistrationFailed: return "failed to register the Local APIC spurious interrupt handler";
        case LocalApicInitializationError::RegisterValidationFailed: return "Local APIC register programming did not validate";
        default: return "unknown Local APIC initialization error";
        }
    }
}