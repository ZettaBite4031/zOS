#pragma once

#include <Kernel/Time/State.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class CpuInfo;
    class LocalApic;
    class InterruptManager;
    struct ProcessorLocal;
}

namespace Zos::Kernel::Platform {
    class Hpet;
}

namespace Zos::Kernel::Time {
    void InitializeClock(State& time, const Architecture::AMD64::CpuInfo& cpu, const Platform::Hpet& hpet) noexcept;

    void InitializeSchedulerTimer(State& time, Architecture::AMD64::LocalApic& apic, Architecture::AMD64::InterruptManager& interrupts, const Architecture::AMD64::ProcessorLocal& processor) noexcept;
}
