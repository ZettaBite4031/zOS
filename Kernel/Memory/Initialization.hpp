#pragma once

#include <Boot/Protocol.hpp>
#include <Kernel/Boot/Context.hpp>
#include <Kernel/Memory/State.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class InterruptManager;
}

namespace Zos::Kernel::Memory {
    /*
     * Brings up the PMM, bootstrap metadata, kernel virtual allocators,
     * kernel-owned page tables, and the active kernel address space.
     */
    void InitializeBootstrap(State& memory, const ::Zos::Boot::BootEnvironment& environment) noexcept;

    /*
     * Runs after kernel exception/interrupt entry exists. Reclaims eligible
     * loader memory and prepares the permanent kernel stack.
     */
    void PreparePermanentRuntime(State& memory, const Architecture::AMD64::InterruptManager& interrupts) noexcept;

    /*
     * Transfers execution onto the permanent stack prepared above. This is a
     * memory-owned transition because the old loader stack becomes invalid.
     */
    [[noreturn]] void SwitchToPermanentStack(State& memory, void(*continuation)(void*) noexcept, void* context) noexcept;

    /*
     * Runs only after execution has moved onto PrimaryStack. Releases the
     * loader-provided stack/storage, establishes the permanent heap and C++
     * allocation runtime, promotes metadata, and retires bootstrap metadata.
     */
    void InitializePermanent(State& memory, Boot::Context& boot) noexcept;
}
