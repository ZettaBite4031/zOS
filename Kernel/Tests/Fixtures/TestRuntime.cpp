#include <Kernel/Tests/Fixtures/TestRuntime.hpp>

namespace {
    constinit Zos::Kernel::Tests::Fixtures::TestRuntime g_TestRuntime{};
}

namespace Zos::Kernel::Tests::Fixtures {
    TestRuntime& Get() noexcept { return g_TestRuntime; }
}
