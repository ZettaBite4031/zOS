#include <Kernel/Tests/Integration/BootSuite.hpp>

#include <Kernel/Synchronization/SelfTest.hpp>
#include <Kernel/Tests/Integration/FirstCity.hpp>
#include <Kernel/Tests/Integration/NativeUserspace.hpp>
#include <Kernel/Tests/Integration/ObjectHandles.hpp>
#include <Kernel/Tests/Integration/WaitableObjects.hpp>

namespace Zos::Kernel::Tests::Integration {
    void RunBootSuite(KernelRuntime& runtime) noexcept {
        NativeUserspace::Run(runtime);
        FirstCity::Run(runtime);
        ObjectHandles::Run(runtime);

        Synchronization::SelfTest::VerifyFoundation(
            runtime
        );

        WaitableObjects::Run(runtime);
    }
}
