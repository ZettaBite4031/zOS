#include <Kernel/Time/Initialization.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Time {
    void InitializeClock(State& time, const Architecture::AMD64::CpuInfo& cpu, const Platform::Hpet& hpet) noexcept {
        const auto error = time.Clock.Initialize(cpu, hpet);
        if (error != MonotonicClockInitializationError::Success)
            Diagnostics::Fatal("Time", MonotonicClock::Describe(error));
    }

    void InitializeSchedulerTimer(State& time, Architecture::AMD64::LocalApic& apic, Architecture::AMD64::InterruptManager& interrupts, const Architecture::AMD64::ProcessorLocal& processor) noexcept {
        const auto error = time.Timer.Initialize(apic, interrupts, time.Clock, processor);
        if (error != LocalApicTimerInitializationError::Success)
            Diagnostics::Fatal("Timer", LocalApicTimer::Describe(error));

        if (!time.Timer.EnableAndVerifyInterruptDelivery())
            Diagnostics::Fatal("Timer", "Local APIC timer interrupt delivery validation failed");

        if (!time.Timer.ProcessorLocalObservedInInterrupt())
            Diagnostics::Fatal("CPU", "processor-local state was not observed in interrupt context");

        Diagnostics::Write("[zOS/CPU] Processor-local interrupt access verified.\n");
    }
}
