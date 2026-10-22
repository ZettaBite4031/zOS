#pragma once

#include <Kernel/Architecture/AMD64/APIC.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Time/Clock.hpp>

namespace Zos::Kernel::Time {
    enum class LocalApicTimerInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        ActiveInstanceExists,
        CalibrationFailed,
        InvalidReloadCount,
        TimerConfigurationFailed,
        HandlerRegistrationFailed,
    };

    class LocalApicTimer final {
    public:
        constexpr LocalApicTimer() noexcept = default;

        LocalApicTimer(const LocalApicTimer&) = delete;
        LocalApicTimer& operator=(const LocalApicTimer&) = delete;

        [[nodiscard]] LocalApicTimerInitializationError Initialize(Architecture::AMD64::LocalApic& apic, Architecture::AMD64::InterruptManager& interrupts, 
            const MonotonicClock& clock, const Architecture::AMD64::ProcessorLocal& processor) noexcept;

        /*
         * Performs the first controlled STI in zOS, proves repeated timer
         * delivery and centralized LAPIC EOI, then leaves interrupts enabled
         * with the periodic timer running.
         */
        [[nodiscard]] bool EnableAndVerifyInterruptDelivery() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint64 FrequencyHz() const noexcept { return m_FrequencyHz; }
        [[nodiscard]] Memory::Uint32 ReloadCount() const noexcept { return m_ReloadCount; }
        [[nodiscard]] Memory::Uint64 InterruptCount() const noexcept { return m_InterruptCount; }
        [[nodiscard]] Memory::Uint64 ObservedRateHz() const noexcept { return m_ObservedRateHz; }
        [[nodiscard]] bool ProcessorLocalObservedInInterrupt() const noexcept { return m_ProcessorLocalObserved; }
        [[nodiscard]] bool UserModeObservedInInterrupt() const noexcept { return m_UserModeObserved; }

        [[nodiscard]] static const char* Describe(LocalApicTimerInitializationError error) noexcept;

    private:
        [[nodiscard]] bool Calibrate() noexcept;

        static void HandleInterrupt(Architecture::AMD64::InterruptContext&) noexcept;

        void Print() const noexcept;

        Architecture::AMD64::LocalApic* m_Apic{};
        const MonotonicClock* m_Clock{};
        const Architecture::AMD64::ProcessorLocal* m_Processor{};

        Memory::Uint64 m_FrequencyHz{};
        Memory::Uint64 m_ObservedRateHz{};
        Memory::Uint32 m_ReloadCount{};

        volatile Memory::Uint64 m_InterruptCount{};
        volatile Memory::Uint64 m_FirstInterruptNanoseconds{};
        volatile Memory::Uint64 m_LastInterruptNanoseconds{};
        volatile bool m_ProcessorLocalObserved{};
        volatile bool m_UserModeObserved{};

        bool m_Initialized{};

        static LocalApicTimer* s_ActiveTimer;
    };
}