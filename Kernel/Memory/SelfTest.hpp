#pragma once

#include <Kernel/Memory/State.hpp>

namespace Zos::Kernel::Memory::SelfTest {
    void VerifyPhysicalAllocator(State& memory) noexcept;
    void VerifyVirtualMemory(State& memory) noexcept;
    void VerifyBootMemoryReclamation(State& memory) noexcept;
    void VerifyKernelHeap(State& memory) noexcept;
    void VerifyCxxAllocation(State& memory) noexcept;
    void VerifyPermanentMemory(State& memory) noexcept;
}
