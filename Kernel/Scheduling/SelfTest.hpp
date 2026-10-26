#pragma once

#include <Kernel/Kernel.hpp>

namespace Zos::Kernel::Scheduling::SelfTest {
    void PrepareWorker(KernelRuntime& runtime) noexcept;
    void VerifyCooperativeSwitch(KernelRuntime& runtime) noexcept;
    void RegisterWorker(KernelRuntime& runtime) noexcept;
    void VerifyRoundRobin(KernelRuntime& runtime) noexcept;
    void VerifyPreemption(KernelRuntime& runtime) noexcept;
}
