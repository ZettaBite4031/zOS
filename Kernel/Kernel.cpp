#include <Boot/Protocol.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Initialization/Initialization.hpp>

namespace {
    /*
     * Kernel-lifetime ownership is established before KernelMain
     * executes and requires no dynamic initialization.
     */
    constinit Zos::Kernel::KernelRuntime g_KernelRuntime{};
}

namespace Zos::Kernel {
    KernelRuntime& GetKernelRuntime() noexcept {
        return g_KernelRuntime;
    }
}

extern "C" [[noreturn]] __attribute__((section(".text.KernelMain"))) void KernelMain(const Zos::Boot::BootEnvironment* environment) noexcept {
    using namespace Zos;

    Kernel::Diagnostics::Write(
        "\n\n"
        "[zOS/Kernel]"
        "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
        "[zOS/Kernel]           Kernel entry reached\n"
        "[zOS/Kernel]"
        "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
    );

    if (environment == nullptr) 
        Kernel::Diagnostics::Fatal("Startup", "boot environment pointer is null");
    
    Kernel::KernelRuntime& runtime = Kernel::GetKernelRuntime();
    Kernel::Initialization::Bootstrap(*environment, runtime);
    Kernel::Initialization::EnterRuntime(runtime);
}