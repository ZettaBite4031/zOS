#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>
#include <Kernel/Memory/KernelHeap.hpp>
#include <Kernel/Memory/KernelStack.hpp>
#include <Kernel/Architecture/AMD64/Paging.hpp>

namespace Zos::Kernel::Memory {
    struct State final {
        PhysicalMemoryManager Physical{};
        BootstrapMetadataArena BootstrapMetadata{};
        VirtualAddressAllocator KernelAddresses{};
        VirtualAddressAllocator MmioAddresses{};
        Architecture::AMD64::PageMap KernelPageMap{};
        KernelHeap Heap{};
        KernelStack PrimaryStack{};
    };
}
