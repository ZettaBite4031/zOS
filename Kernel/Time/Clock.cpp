#include <Kernel/Time/Clock.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Time {
    namespace {
        using Memory::MaximumValue;
        using Memory::Uint64;

        inline constexpr Uint64 NanosecondsPerSecond{ 1'000'000'000ULL };

        inline constexpr Uint64 CalibrationWindowFemtoseconds{ 10'000'000'000'000ULL };
        inline constexpr Uint64 CalibrationSampleCount{ 5 };

        inline constexpr Uint64 MinimumAcceptedTscFrequency{ 1'000'000ULL };
        inline constexpr Uint64 MaximumAcceptedTscFrequency{ 1'000'000'000'000ULL };

        inline constexpr Uint64 ClockAdvanceProbeReads{ 1'000'000 };

        [[nodiscard]] Uint64 FractionToNanoseconds(Uint64 numerator, Uint64 denominator) noexcept {
            if (denominator == 0 || numerator >= denominator) return 0;

            Uint64 remainder = numerator;
            Uint64 result{};

            /*
             * Produce nine decimal digits in three base-1000 steps.
             * This avoids forming numerator * 1,000,000,000 directly.
             */
            for (Memory::Uint32 group = 0; group < 3; group++) {
                if (remainder > MaximumValue / 1'000ULL) return MaximumValue;

                const Uint64 scaled = remainder * 1'000ULL;
                const Uint64 digits = scaled / denominator;
                remainder = scaled % denominator;

                if (result > (MaximumValue - digits) / 1'000ULL)
                    return MaximumValue;

                result = result * 1'000ULL + digits;
            }

            return result;
        }
    }

    MonotonicClockInitializationError MonotonicClock::Initialize(const Architecture::AMD64::CpuInfo& cpu, const Platform::Hpet& hpet) noexcept {
        if (m_Initialized) return MonotonicClockInitializationError::AlreadyInitialized;
        if (!cpu.IsInitialized() || !hpet.IsInitialized()) return MonotonicClockInitializationError::InvalidDependency;

        m_Hpet = &hpet;

        bool tsc_calibration_attempted = false;
        if (cpu.Features().Tsc && cpu.Features().InvariantTsc) {
            tsc_calibration_attempted = true;
            Uint64 calibrated_frequency{};
            if (CalibrateInvariantTsc(hpet, calibrated_frequency)) {
                m_Source = MonotonicClockSource::InvariantTsc;
                m_FrequencyHz = calibrated_frequency;
            }
        }

        if (m_FrequencyHz == 0) {
            /*
             * A 32-bit HPET counter can wrap in minutes. Until periodic
             * timer service exists to extend it in software, it is not a
             * safe permanent clock source when calls may be arbitrarily far
             * apart.
             */
            if (!hpet.Has64BitCounter())
                return MonotonicClockInitializationError::NoStableClockSource;

            m_Source = MonotonicClockSource::Hpet;
            m_FrequencyHz = hpet.FrequencyHz();

            if (m_FrequencyHz == 0) 
                return MonotonicClockInitializationError::NoStableClockSource;
        }

        m_InitialTicks = CurrentTicks();

        bool advanced = false;
        for (Uint64 attempt = 0; attempt < ClockAdvanceProbeReads; attempt++) {
            if (CurrentTicks() != m_InitialTicks) {
                advanced = true;
                break;
            }
        }

        if (!advanced) return MonotonicClockInitializationError::ClockNotAdvancing;

        m_Initialized = true;
        Print(cpu, tsc_calibration_attempted);

        return MonotonicClockInitializationError::Success;
    }

    bool MonotonicClock::CalibrateInvariantTsc(const Platform::Hpet& hpet, Uint64& frequency_hz) const noexcept {
        frequency_hz = 0;

        const Uint64 period = hpet.ClockPeriodFemtoseconds();
        if (period == 0) return false;

        const Uint64 target_hpet_ticks = (CalibrationWindowFemtoseconds + period - 1) / period;
        if (target_hpet_ticks == 0) return false;

        Uint64 minimum_frequency = MaximumValue;
        Uint64 maximum_frequency{};
        Uint64 frequency_sum{};

        for (Uint64 sample = 0; sample < CalibrationSampleCount; sample++) {
            const Uint64 start_hpet = hpet.ReadCounter();
            const Uint64 start_tsc = Architecture::AMD64::ReadTsc();

            Uint64 end_hpet{};
            Uint64 elapsed_hpet{};

            do {
                end_hpet = hpet.ReadCounter();
                elapsed_hpet = hpet.ElapsedTicks(start_hpet, end_hpet);
            } while (elapsed_hpet < target_hpet_ticks);

            const Uint64 end_tsc = Architecture::AMD64::ReadTsc();
            const Uint64 elapsed_tsc = end_tsc - start_tsc;

            const Uint64 elapsed_nanoseconds = hpet.TicksToNanoseconds(elapsed_hpet);
            if (elapsed_tsc == 0 || elapsed_nanoseconds == 0 ||
                elapsed_nanoseconds == MaximumValue)
                return false;

            /*
             * Compute elapsed_tsc * 1e9 / elapsed_ns without requiring
             * 128-bit compiler-runtime division support.
             */
            const Uint64 whole = elapsed_tsc / elapsed_nanoseconds;
            const Uint64 remainder = elapsed_tsc % elapsed_nanoseconds;

            if (whole > MaximumValue / NanosecondsPerSecond)
                return false;

            const Uint64 whole_hz = whole * NanosecondsPerSecond;

            if (remainder > MaximumValue / NanosecondsPerSecond)
                return false;

            const Uint64 fractional_hz =
                (remainder * NanosecondsPerSecond) / elapsed_nanoseconds;

            if (fractional_hz > MaximumValue - whole_hz)
                return false;

            const Uint64 sample_frequency = whole_hz + fractional_hz;

            if (sample_frequency < MinimumAcceptedTscFrequency ||
                sample_frequency > MaximumAcceptedTscFrequency)
                return false;

            if (sample_frequency < minimum_frequency)
                minimum_frequency = sample_frequency;

            if (sample_frequency > maximum_frequency)
                maximum_frequency = sample_frequency;

            if (sample_frequency > MaximumValue - frequency_sum)
                return false;

            frequency_sum += sample_frequency;
        }

        const Uint64 average_frequency = frequency_sum / CalibrationSampleCount;
        if (average_frequency == 0 || maximum_frequency < minimum_frequency)
            return false;

        const Uint64 spread = maximum_frequency - minimum_frequency;

        /*
         * Require all five 10 ms samples to fit within a 1% band.
         * HPET remains available as the correctness fallback if a VM or
         * platform cannot produce a stable calibration.
         */
        if (spread > average_frequency / 100)
            return false;

        frequency_hz = average_frequency;
        return true;
    }

    Uint64 MonotonicClock::CurrentTicks() const noexcept {
        switch (m_Source) {
        case MonotonicClockSource::InvariantTsc:
            return Architecture::AMD64::ReadTsc();

        case MonotonicClockSource::Hpet:
            return m_Hpet != nullptr ? m_Hpet->ReadCounter() : 0;

        default:
            return 0;
        }
    }

    Uint64 MonotonicClock::Ticks() const noexcept {
        if (!m_Initialized) return 0;

        const Uint64 current = CurrentTicks();

        if (m_Source == MonotonicClockSource::Hpet)
            return m_Hpet->ElapsedTicks(m_InitialTicks, current);

        return current - m_InitialTicks;
    }

    Uint64 MonotonicClock::TscTicksToNanoseconds(Uint64 ticks) const noexcept {
        if (m_FrequencyHz == 0) return 0;

        const Uint64 seconds = ticks / m_FrequencyHz;
        const Uint64 remainder = ticks % m_FrequencyHz;

        if (seconds > MaximumValue / NanosecondsPerSecond)
            return MaximumValue;

        const Uint64 whole_nanoseconds = seconds * NanosecondsPerSecond;
        const Uint64 fractional_nanoseconds =
            FractionToNanoseconds(remainder, m_FrequencyHz);

        if (fractional_nanoseconds == MaximumValue ||
            fractional_nanoseconds > MaximumValue - whole_nanoseconds)
            return MaximumValue;

        return whole_nanoseconds + fractional_nanoseconds;
    }

    Uint64 MonotonicClock::Nanoseconds() const noexcept {
        if (!m_Initialized) return 0;

        const Uint64 ticks = Ticks();

        if (m_Source == MonotonicClockSource::InvariantTsc)
            return TscTicksToNanoseconds(ticks);

        return m_Hpet != nullptr ? m_Hpet->TicksToNanoseconds(ticks) : 0;
    }

    void MonotonicClock::Print(const Architecture::AMD64::CpuInfo& cpu, bool tsc_calibration_attempted) const noexcept {
        Diagnostics::Write("[zOS/Time] Monotonic clock source: ");
        Diagnostics::Write(SourceName(m_Source));
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Time] Clock frequency: ");
        Diagnostics::WriteDecimal(m_FrequencyHz);
        Diagnostics::Write(" Hz\n");

        if (m_Source == MonotonicClockSource::InvariantTsc) {
            Diagnostics::Write("[zOS/Time] TSC reference: HPET, ");
            Diagnostics::WriteDecimal(CalibrationSampleCount);
            Diagnostics::Write(" x 10 ms samples\n");
        } else if (!cpu.Features().InvariantTsc) {
            Diagnostics::Write("[zOS/Time] Invariant TSC unavailable; HPET retained as clock source.\n");
        } else if (tsc_calibration_attempted) {
            Diagnostics::Write("[zOS/Time] Invariant TSC calibration rejected; HPET retained as clock source.\n");
        }

        Diagnostics::Write("[zOS/Time] Monotonic clock initialized.\n");
    }

    const char* MonotonicClock::SourceName(MonotonicClockSource source) noexcept {
        switch (source) {
        case MonotonicClockSource::Hpet: return "HPET";
        case MonotonicClockSource::InvariantTsc: return "invariant TSC";
        default: return "unknown";
        }
    }

    const char* MonotonicClock::Describe(MonotonicClockInitializationError error) noexcept {
        switch (error) {
        case MonotonicClockInitializationError::Success: return "success";
        case MonotonicClockInitializationError::AlreadyInitialized: return "monotonic clock is already initialized";
        case MonotonicClockInitializationError::InvalidDependency: return "monotonic clock dependencies are invalid";
        case MonotonicClockInitializationError::NoStableClockSource: return "no stable monotonic clock source is available";
        case MonotonicClockInitializationError::ClockNotAdvancing: return "selected monotonic clock source is not advancing";
        default: return "unknown monotonic clock initialization error";
        }
    }
}