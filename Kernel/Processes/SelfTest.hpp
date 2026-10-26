#pragma once

#include <Kernel/Memory/State.hpp>
#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel::Processes::SelfTest {
    void VerifyKernelProcess(Process& process, Architecture::AMD64::PageMap& kernel_page_map) noexcept;

    /*
     * Verifies isolated address-space ownership on an already-created process.
     * The caller owns the process and therefore controls its lifetime.
     */
    void VerifyIsolatedAddressSpace(Process& process, Memory::State& memory) noexcept;
}
