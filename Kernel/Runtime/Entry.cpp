#include <Kernel/Runtime/Entry.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Runtime {
    [[noreturn]] void Enter() noexcept {
        Diagnostics::Write("[zOS/Kernel] Permanent kernel runtime entered.\n");

        for (;;) __asm__ volatile("hlt");
    }
}
