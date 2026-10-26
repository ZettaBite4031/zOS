#pragma once

#include <Kernel/Architecture/AMD64/CPU.hpp>
#include <Kernel/Architecture/AMD64/DescriptorTables.hpp>
#include <Kernel/Architecture/AMD64/Interrupts.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    struct State final {
        CpuInfo Cpu{};
        DescriptorTables Descriptors{};
        InterruptManager Interrupts{};
    };
}
