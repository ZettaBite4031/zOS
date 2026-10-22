#pragma once

#include <Kernel/Executables/UserExecutableLoader.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Processes/UserProcessLauncher.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Tests::Fixtures {
    // Boot-time verification resources are not part of permanent kernel services.
    // These objects are intentionally retained while the scheduler can reference them.
    struct TestRuntime final {
        Executables::LoadedUserImage FirstUserImage{};
        Processes::Process FirstUserProcess{};
        Scheduling::Thread FirstUserThread{};
        Scheduling::Thread FirstWorkerThread{};
        Processes::UserProcessInstance CityExit{};
        Processes::UserProcessInstance CityFault{};
        Processes::UserProcessInstance CitySurvivor{};
    };

    [[nodiscard]] TestRuntime& Get() noexcept;
}
