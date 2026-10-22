#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    inline constexpr Memory::Uint64 RflagsInterruptEnable{ 1ULL << 9 };

    [[nodiscard]] inline Memory::Uint64 ReadProcessorFlags() noexcept {
        Memory::Uint64 value{};
        __asm__ volatile("pushfq\n\tpopq %0" : "=r"(value) :: "memory");
        return value;
    }

    [[nodiscard]] inline bool InterruptsEnabled() noexcept {
        return (ReadProcessorFlags() & RflagsInterruptEnable) != 0;
    }

    // Masks local interrupts only. Not a lock: SMP callers need synchronization.
    class ScopedInterruptDisable final {
    public:
        ScopedInterruptDisable() noexcept : m_WasEnabled(InterruptsEnabled()) {
            __asm__ volatile("cli" ::: "memory");
        }
        ScopedInterruptDisable(const ScopedInterruptDisable&) = delete;
        ScopedInterruptDisable& operator=(const ScopedInterruptDisable&) = delete;
        ~ScopedInterruptDisable() noexcept {
            if (m_WasEnabled) __asm__ volatile("sti" ::: "memory");
        }
    private:
        bool m_WasEnabled{};
    };
}
