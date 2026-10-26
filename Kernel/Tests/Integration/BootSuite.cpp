#include <Kernel/Tests/Integration/BootSuite.hpp>

#include <Kernel/Tests/Integration/FirstCity.hpp>
#include <Kernel/Tests/Integration/NativeUserspace.hpp>

namespace Zos::Kernel::Tests::Integration {
    void RunBootSuite(KernelRuntime& runtime) noexcept {
        NativeUserspace::Run(runtime);
        FirstCity::Run(runtime);
    }
}
