#include <Kernel/Architecture/AMD64/Initialization.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    void Initialize(State& architecture, Memory::State& memory) noexcept {
        const auto cpu_error = architecture.Cpu.Initialize();

        if (cpu_error != CpuInitializationError::Success)
            Diagnostics::Fatal("CPU", CpuInfo::Describe(cpu_error));

        const auto interrupt_error = architecture.Interrupts.Initialize(memory.Physical, memory.KernelAddresses,memory.KernelPageMap, architecture.Descriptors);
        if (interrupt_error != InterruptInitializationError::Success)
            Diagnostics::Fatal("Interrupt", InterruptManager::Describe(interrupt_error));

        Diagnostics::Write("[zOS/Interrupt] GDT, TSS, and IDT established.\n");

        if (!architecture.Interrupts.RunBreakpointSelfTest())
            Diagnostics::Fatal("Interrupt", "INT3 round-trip self-test failed");

        Diagnostics::Write("[zOS/Interrupt] INT3 dispatch and IRETQ self-test passed.\n");
    }
}
