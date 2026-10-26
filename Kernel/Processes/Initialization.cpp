#include <Kernel/Processes/Initialization.hpp>
#include <Kernel/Processes/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Objects/SelfTest.hpp>

namespace Zos::Kernel::Processes {
    namespace {
        inline constexpr ProcessId KernelProcessId{ 0 };
    }

    void Initialize(State& processes, Memory::State& memory) noexcept {
        /*
         * Processes are the first permanent consumers of the object model.
         * Verify its generic identity/reference/handle invariants before
         * publishing the kernel process.
         */
        Objects::SelfTest::VerifyFoundation();

        const auto error = processes.KernelProcess.InitializeKernel(KernelProcessId, memory.KernelPageMap);

        if (error != ProcessInitializationError::Success)
            Diagnostics::Fatal("Process", Process::Describe(error));

        SelfTest::VerifyKernelProcess(processes.KernelProcess, memory.KernelPageMap);
    }
}
