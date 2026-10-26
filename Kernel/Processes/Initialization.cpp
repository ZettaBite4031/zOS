#include <Kernel/Processes/Initialization.hpp>
#include <Kernel/Processes/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Processes {
    namespace {
        inline constexpr ProcessId KernelProcessId{ 0 };
    }

    void Initialize(State& processes, Memory::State& memory) noexcept {
        const auto error = processes.KernelProcess.InitializeKernel(KernelProcessId, memory.KernelPageMap);
        if (error != ProcessInitializationError::Success)
            Diagnostics::Fatal("Process", Process::Describe(error));

        SelfTest::VerifyKernelProcess(processes.KernelProcess, memory.KernelPageMap);
    }
}
