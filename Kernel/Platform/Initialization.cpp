#include <Kernel/Platform/Initialization.hpp>

#include <Kernel/Architecture/AMD64/LegacyPIC.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Time/Initialization.hpp>

namespace Zos::Kernel::Platform {
    void Initialize(State& platform, Time::State& time, const Boot::Context& boot, Architecture::AMD64::State& architecture, Memory::State& memory) noexcept {
        const auto acpi_error = platform.Acpi.Initialize(boot.AcpiRsdp);
        if (acpi_error != ACPI::InitializationError::Success)
            Diagnostics::Fatal("ACPI", ACPI::TableDirectory::Describe(acpi_error));

        const auto madt_error = platform.Madt.Initialize(platform.Acpi);
        if (madt_error != ACPI::MadtInitializationError::Success)
            Diagnostics::Fatal("ACPI", ACPI::Madt::Describe(madt_error));

        const auto legacy_pic_error = Architecture::AMD64::LegacyPic::Quiesce(platform.Madt);
        if (legacy_pic_error != Architecture::AMD64::LegacyPic::QuiesceError::Success)
            Diagnostics::Fatal("Interrupt", Architecture::AMD64::LegacyPic::Describe(legacy_pic_error));

        const auto apic_error = platform.Apic.Initialize(architecture.Cpu, platform.Madt, architecture.Interrupts, memory.MmioAddresses, memory.KernelPageMap);
        if (apic_error != Architecture::AMD64::LocalApicInitializationError::Success)
            Diagnostics::Fatal("APIC", Architecture::AMD64::LocalApic::Describe(apic_error));

        if (!architecture.Interrupts.BindLocalApic(platform.Apic))
            Diagnostics::Fatal("Interrupt", "failed to bind Local APIC acknowledgement");

        if (!architecture.Interrupts.RunVectorPolicySelfTest())
            Diagnostics::Fatal("Interrupt", "interrupt vector policy self-test failed");

        Diagnostics::Write("[zOS/Interrupt] Vector policy and Local APIC acknowledgement established.\n");

        const auto hpet_error = platform.Hpet.Initialize(platform.Acpi, memory.MmioAddresses, memory.KernelPageMap);
        if (hpet_error != HpetInitializationError::Success)
            Diagnostics::Fatal("HPET", Hpet::Describe(hpet_error));

        Time::InitializeClock(time, architecture.Cpu, platform.Hpet);
        const auto processor_error = platform.BootstrapProcessor.Initialize(0, platform.Apic, true);
        if (processor_error != Architecture::AMD64::ProcessorLocalInitializationError::Success)
            Diagnostics::Fatal("CPU", Architecture::AMD64::ProcessorLocal::Describe(processor_error));
    }
}
