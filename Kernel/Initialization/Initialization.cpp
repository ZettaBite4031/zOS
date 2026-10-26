#include <Kernel/Initialization/Initialization.hpp>
#include <Kernel/Kernel.hpp>

#include <Kernel/Architecture/AMD64/Initialization.hpp>
#include <Kernel/Boot/Initialization.hpp>
#include <Kernel/Memory/Initialization.hpp>
#include <Kernel/Platform/Initialization.hpp>
#include <Kernel/Processes/Initialization.hpp>
#include <Kernel/Runtime/Entry.hpp>
#include <Kernel/Scheduling/Initialization.hpp>
#include <Kernel/Tests/Integration/BootSuite.hpp>
#include <Kernel/Userspace/Initialization.hpp>

namespace Zos::Kernel::Initialization {
    namespace {
        [[noreturn]] void ContinueBootstrap(void* context) noexcept {
            auto& runtime = *static_cast<KernelRuntime*>(context);

            Memory::InitializePermanent(runtime.Memory, runtime.Boot);
            Platform::Initialize(runtime.Platform, runtime.Time, runtime.Boot, runtime.Architecture, runtime.Memory);
            Processes::Initialize(runtime.Processes, runtime.Memory);
            Scheduling::Initialize(runtime);
            Userspace::Initialize(runtime.Userspace, runtime.Architecture);

            Tests::Integration::RunBootSuite(runtime);
            Runtime::Enter();
        }
    }

    [[noreturn]] void Bootstrap(const ::Zos::Boot::BootEnvironment& environment, KernelRuntime& runtime) noexcept {
        Boot::Initialize(runtime.Boot, environment);
        Memory::InitializeBootstrap(runtime.Memory, environment);
        Architecture::AMD64::Initialize(runtime.Architecture, runtime.Memory);

        Memory::PreparePermanentRuntime(runtime.Memory, runtime.Architecture.Interrupts);
        Memory::SwitchToPermanentStack(runtime.Memory, &ContinueBootstrap, &runtime);
    }
}
