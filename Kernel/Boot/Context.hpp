#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Boot {
    struct Context final {
        Memory::PhysicalSpan KernelImage{};
        Memory::PhysicalSpan BootstrapStack{};
        Memory::PhysicalSpan EnvironmentStorage{};
        Memory::PhysicalSpan MemoryMapStorage{};
        Memory::PhysicalAddress AcpiRsdp{};
        bool Initialized{};
    };
}
