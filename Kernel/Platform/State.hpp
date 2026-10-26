#pragma once

#include <Kernel/Architecture/AMD64/APIC.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Platform/ACPI/ACPI.hpp>
#include <Kernel/Platform/ACPI/MADT.hpp>
#include <Kernel/Platform/HPET.hpp>

namespace Zos::Kernel::Platform {
    struct State final {
        ACPI::TableDirectory Acpi{};
        ACPI::Madt Madt{};
        Architecture::AMD64::LocalApic Apic{};
        Hpet Hpet{};
        Architecture::AMD64::ProcessorLocal BootstrapProcessor{};
    };
}
