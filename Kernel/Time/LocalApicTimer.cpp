#include <Kernel/Time/LocalApicTimer.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Synchronization/Timer.hpp>

namespace Zos::Kernel::Time {
    namespace {
        using Memory::MaximumValue;
        using Memory::Uint32;
        using Memory::Uint64;

        inline constexpr Uint64 NanosecondsPerSecond{ 1'000'000'000ULL };

        inline constexpr Uint64 CalibrationWindowNanoseconds{ 10'000'000ULL };
        inline constexpr Uint64 CalibrationSampleCount{ 5 };
        inline constexpr Uint64 MaximumCalibrationSpreadPercent{ 2 };

        inline constexpr Uint64 TargetInterruptRateHz{ 100 };
        inline constexpr Uint64 DeliveryTestInterruptCount{ 8 };
        inline constexpr Uint64 DeliveryTestTimeoutNanoseconds{ 500'000'000ULL };

        [[nodiscard]] Uint64 ReadRflags() noexcept {
            Uint64 flags{};

            __asm__ volatile(
                "pushfq\n\t"
                "popq %0"
                : "=r"(flags)
                :
                : "memory"
            );

            return flags;
        }

        inline constexpr Uint64 RflagsInterruptEnable{ 1ULL << 9 };
    }

    LocalApicTimer* LocalApicTimer::s_ActiveTimer{};

    LocalApicTimerInitializationError LocalApicTimer::Initialize(Architecture::AMD64::LocalApic& apic, Architecture::AMD64::InterruptManager& interrupts, 
        const MonotonicClock& clock, const Architecture::AMD64::ProcessorLocal& processor) noexcept {

        if (m_Initialized) return LocalApicTimerInitializationError::AlreadyInitialized;
        if (!apic.IsInitialized() || !interrupts.IsInitialized() || !clock.IsInitialized() || !processor.IsInitialized())
            return LocalApicTimerInitializationError::InvalidDependency;

        if (s_ActiveTimer != nullptr) return LocalApicTimerInitializationError::ActiveInstanceExists;
        if ((ReadRflags() & RflagsInterruptEnable) != 0)
            return LocalApicTimerInitializationError::InvalidDependency;

        m_Apic = &apic;
        m_Clock = &clock;
        m_Processor = &processor;

        if (!Calibrate()) return LocalApicTimerInitializationError::CalibrationFailed;

        const Uint64 reload_count = m_FrequencyHz / TargetInterruptRateHz;
        if (reload_count == 0 || reload_count > 0xFFFFFFFFULL)
            return LocalApicTimerInitializationError::InvalidReloadCount;

        m_ReloadCount = static_cast<Uint32>(reload_count);

        /*
         * Leave the periodic source stopped and masked. The delivery test
         * will unmask it, load the initial count, then perform the first STI.
         */
        if (!m_Apic->ConfigureTimer(Architecture::AMD64::LocalApicTimerVector, Architecture::AMD64::LocalApicTimerMode::Periodic,
                Architecture::AMD64::LocalApicTimerDivide::By16, true))
            return LocalApicTimerInitializationError::TimerConfigurationFailed;

        m_Apic->SetTimerInitialCount(0);

        if (!interrupts.RegisterFixedHandler(Architecture::AMD64::LocalApicTimerVector,
                &HandleInterrupt, "Local APIC timer", Architecture::AMD64::InterruptAcknowledgement::LocalApicEoiBeforeHandler))
            return LocalApicTimerInitializationError::HandlerRegistrationFailed;

        s_ActiveTimer = this;
        m_Initialized = true;

        Print();

        return LocalApicTimerInitializationError::Success;
    }

    bool LocalApicTimer::Calibrate() noexcept {
        if (m_Apic == nullptr || m_Clock == nullptr)
            return false;

        if (!m_Apic->ConfigureTimer(Architecture::AMD64::LocalApicTimerVector, Architecture::AMD64::LocalApicTimerMode::OneShot,
                Architecture::AMD64::LocalApicTimerDivide::By16, true))
            return false;

        Uint64 minimum_frequency = MaximumValue;
        Uint64 maximum_frequency{};
        Uint64 frequency_sum{};

        for (Uint64 sample = 0; sample < CalibrationSampleCount; sample++) {
            m_Apic->SetTimerInitialCount(0xFFFFFFFFu);

            const Uint64 start_nanoseconds = m_Clock->Nanoseconds();
            Uint64 current_nanoseconds{};

            do {
                __asm__ volatile("pause");
                current_nanoseconds = m_Clock->Nanoseconds();
            } while (current_nanoseconds - start_nanoseconds < CalibrationWindowNanoseconds);

            const Uint32 current_count = m_Apic->TimerCurrentCount();
            m_Apic->SetTimerInitialCount(0);

            const Uint64 elapsed_nanoseconds = current_nanoseconds - start_nanoseconds;
            const Uint64 elapsed_ticks = 0xFFFFFFFFULL - static_cast<Uint64>(current_count);
            if (elapsed_nanoseconds == 0 || elapsed_ticks == 0)
                return false;

            /*
             * elapsed_ticks is bounded by the 32-bit LAPIC counter, so the
             * multiplication cannot overflow 64 bits.
             */
            const Uint64 sample_frequency = (elapsed_ticks * NanosecondsPerSecond) / elapsed_nanoseconds;
            if (sample_frequency == 0) return false;

            if (sample_frequency < minimum_frequency) minimum_frequency = sample_frequency;
            if (sample_frequency > maximum_frequency) maximum_frequency = sample_frequency;

            if (sample_frequency > MaximumValue - frequency_sum) return false;

            frequency_sum += sample_frequency;
        }

        const Uint64 average_frequency = frequency_sum / CalibrationSampleCount;
        if (average_frequency == 0 || maximum_frequency < minimum_frequency)
            return false;

        const Uint64 spread = maximum_frequency - minimum_frequency;
        const Uint64 maximum_spread = (average_frequency / 100) * MaximumCalibrationSpreadPercent;
        if (spread > maximum_spread) return false;

        m_FrequencyHz = average_frequency;
        return true;
    }

    bool LocalApicTimer::EnableAndVerifyInterruptDelivery() noexcept {
        if (!m_Initialized || m_Apic == nullptr || m_Clock == nullptr) return false;
        if ((ReadRflags() & RflagsInterruptEnable) != 0) return false;

        m_InterruptCount = 0;
        m_FirstInterruptNanoseconds = 0;
        m_LastInterruptNanoseconds = 0;
        m_ObservedRateHz = 0;
        m_ProcessorLocalObserved = false;
        m_UserModeObserved = false;

        if (!m_Apic->SetTimerMasked(false)) return false;
        const Uint64 test_start = m_Clock->Nanoseconds();
        m_Apic->SetTimerInitialCount(m_ReloadCount);

        /*
         * All externally routable legacy sources remain quiesced. From this
         * point forward the periodic LAPIC timer is the first intentionally
         * enabled maskable interrupt source in zOS.
         */
        __asm__ volatile("sti" ::: "memory");
        while (m_InterruptCount < DeliveryTestInterruptCount) {
            __asm__ volatile("pause");
            if (m_Clock->Nanoseconds() - test_start > DeliveryTestTimeoutNanoseconds) {
                __asm__ volatile("cli" ::: "memory");
                (void)m_Apic->SetTimerMasked(true);
                m_Apic->SetTimerInitialCount(0);
                return false;
            }
        }

        /*
         * Freeze observations while validating them. The timer continues
         * counting, but maskable delivery is briefly disabled.
         */
        __asm__ volatile("cli" ::: "memory");

        const Uint64 observed_count = m_InterruptCount;
        const Uint64 first_nanoseconds = m_FirstInterruptNanoseconds;
        const Uint64 last_nanoseconds = m_LastInterruptNanoseconds;

        if (observed_count < 2 || first_nanoseconds == 0 || last_nanoseconds <= first_nanoseconds) {
            (void)m_Apic->SetTimerMasked(true);
            m_Apic->SetTimerInitialCount(0);
            return false;
        }

        const Uint64 observed_intervals = observed_count - 1;
        const Uint64 observed_duration = last_nanoseconds - first_nanoseconds;

        if (observed_intervals > MaximumValue / NanosecondsPerSecond) {
            (void)m_Apic->SetTimerMasked(true);
            m_Apic->SetTimerInitialCount(0);
            return false;
        }

        m_ObservedRateHz = (observed_intervals * NanosecondsPerSecond) / observed_duration;

        /*
         * Virtualization can introduce scheduling jitter. This is deliberately
         * a delivery/cadence sanity check, not a precision benchmark.
         */
        const Uint64 minimum_rate = TargetInterruptRateHz / 2;
        const Uint64 maximum_rate = TargetInterruptRateHz + TargetInterruptRateHz / 2;

        if (m_ObservedRateHz < minimum_rate || m_ObservedRateHz > maximum_rate) {
            (void)m_Apic->SetTimerMasked(true);
            m_Apic->SetTimerInitialCount(0);
            return false;
        }

        /*
         * Leave the periodic timer and IF enabled. EnterRuntime() can now use
         * HLT as the kernel's idle loop and be awakened by the timer.
         */
        __asm__ volatile("sti" ::: "memory");

        Diagnostics::Write("[zOS/Timer] Observed rate: ");
        Diagnostics::WriteDecimal(m_ObservedRateHz);
        Diagnostics::Write(" Hz\n");

        Diagnostics::Write("[zOS/Timer] Interrupt delivery verified.\n");

        return true;
    }

    void LocalApicTimer::HandleInterrupt(Architecture::AMD64::InterruptContext& context) noexcept {
        LocalApicTimer* timer = s_ActiveTimer;
        if (timer == nullptr || timer->m_Clock == nullptr) return;

        if (context.FromUserMode()) timer->m_UserModeObserved = true;

        Architecture::AMD64::ProcessorLocal* current = Architecture::AMD64::CurrentProcessor();
        if (timer->m_Processor != nullptr) {
            if (current == timer->m_Processor && current->Self == current &&
                current->IsInitialized() && current->ApicId == timer->m_Processor->ApicId)
                timer->m_ProcessorLocalObserved = true;
        }

        const Uint64 now = timer->m_Clock->Nanoseconds();
        const Uint64 count = timer->m_InterruptCount;
        if (count == 0) timer->m_FirstInterruptNanoseconds = now;

        timer->m_LastInterruptNanoseconds = now;
        timer->m_InterruptCount = count + 1;

        /*
         * Waitable timers share the existing LAPIC cadence. Expired timers
         * make their blocked threads Ready before the scheduler decides
         * whether this tick should preempt the current thread.
         */
        if (current != nullptr && current->LocalScheduler != nullptr && current->LocalScheduler->IsInitialized())
            Synchronization::Timer::Poll(now, *current->LocalScheduler);

        /*
        * Timer delivery verification occurs before preemption is enabled,
        * so the same interrupt source serves both bring-up and scheduling.
        */
        if (current != nullptr && current->LocalScheduler != nullptr && current->LocalScheduler->PreemptionEnabled()) {
            const auto error = current->LocalScheduler->Preempt();
            if (error != Scheduling::SchedulerPreemptionError::Success)
                Diagnostics::Fatal("Scheduler", Scheduling::Scheduler::Describe(error));
        }
    }

    void LocalApicTimer::Print() const noexcept {
        Diagnostics::Write("[zOS/Timer] Local APIC timer mode: periodic\n");
        Diagnostics::Write("[zOS/Timer] Divide ratio: 16\n");

        Diagnostics::Write("[zOS/Timer] Calibrated frequency: ");
        Diagnostics::WriteDecimal(m_FrequencyHz);
        Diagnostics::Write(" Hz\n");

        Diagnostics::Write("[zOS/Timer] Interrupt vector: ");
        Diagnostics::WriteHex(Architecture::AMD64::LocalApicTimerVector);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Timer] Target rate: ");
        Diagnostics::WriteDecimal(TargetInterruptRateHz);
        Diagnostics::Write(" Hz\n");

        Diagnostics::Write("[zOS/Timer] Reload count: ");
        Diagnostics::WriteDecimal(m_ReloadCount);
        Diagnostics::Write("\n");
    }

    const char* LocalApicTimer::Describe(LocalApicTimerInitializationError error) noexcept {
        switch (error) {
        case LocalApicTimerInitializationError::Success: return "success";
        case LocalApicTimerInitializationError::AlreadyInitialized: return "Local APIC timer is already initialized";
        case LocalApicTimerInitializationError::InvalidDependency: return "Local APIC timer dependencies or interrupt state are invalid";
        case LocalApicTimerInitializationError::ActiveInstanceExists: return "another Local APIC timer instance is already active";
        case LocalApicTimerInitializationError::CalibrationFailed: return "failed to calibrate the Local APIC timer";
        case LocalApicTimerInitializationError::InvalidReloadCount: return "Local APIC timer calibration produced an invalid periodic reload count";
        case LocalApicTimerInitializationError::TimerConfigurationFailed: return "failed to configure the Local APIC timer registers";
        case LocalApicTimerInitializationError::HandlerRegistrationFailed: return "failed to register the Local APIC timer interrupt handler";
        default: return "unknown Local APIC timer initialization error";
        }
    }
}
