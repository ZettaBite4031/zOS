#pragma once

#include <Kernel/Architecture/AMD64/CPU.hpp>
#include <Kernel/Platform/HPET.hpp>

namespace Zos::Kernel::Time {
    enum class MonotonicClockSource : Memory::Uint32 {
        Hpet,
        InvariantTsc,
    };

    enum class MonotonicClockInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        NoStableClockSource,
        ClockNotAdvancing,  
    };

    class MonotonicClock final {
    public:
        constexpr MonotonicClock() noexcept = default;

        MonotonicClock(const MonotonicClock&) = delete;
        MonotonicClock& operator=(const MonotonicClock&) = delete;

        [[nodiscard]] MonotonicClockInitializationError Initialize(const Architecture::AMD64::CpuInfo& cpu, const Platform::Hpet& hpet) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] MonotonicClockSource Source() const noexcept { return m_Source; }

        [[nodiscard]] Memory::Uint64 FrequencyHz() const noexcept { return m_FrequencyHz; }
        [[nodiscard]] Memory::Uint64 Ticks() const noexcept;

        [[nodiscard]] Memory::Uint64 Nanoseconds() const noexcept;
        [[nodiscard]] Memory::Uint64 Microseconds() const noexcept { return Nanoseconds() / 1'000ULL; }
        [[nodiscard]] Memory::Uint64 Milliseconds() const noexcept { return Nanoseconds() / 1'000'000ULL; }

        [[nodiscard]] static const char* SourceName(MonotonicClockSource source) noexcept;
        [[nodiscard]] static const char* Describe(MonotonicClockInitializationError error) noexcept;

    private:
        [[nodiscard]] bool CalibrateInvariantTsc(const Platform::Hpet& hpet, Memory::Uint64& frequency_hz) const noexcept;
        [[nodiscard]] Memory::Uint64 CurrentTicks() const noexcept;
        [[nodiscard]] Memory::Uint64 TscTicksToNanoseconds(Memory::Uint64 ticks) const noexcept;

        void Print(const Architecture::AMD64::CpuInfo& cpu, bool tsc_calibration_attempted) const noexcept;

        const Platform::Hpet* m_Hpet{};

        MonotonicClockSource m_Source{ MonotonicClockSource::Hpet };
        Memory::Uint64 m_FrequencyHz{};
        Memory::Uint64 m_InitialTicks{};

        bool m_Initialized{};
    };
}