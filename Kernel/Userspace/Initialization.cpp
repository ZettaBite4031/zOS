#include <Kernel/Userspace/Initialization.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel::Userspace {
    void Initialize(State& userspace, Architecture::AMD64::State& architecture) noexcept {
        const auto syscall_error = userspace.SystemCalls.Initialize(architecture.Cpu, architecture.Descriptors);

        if (syscall_error != Architecture::AMD64::SystemCallInitializationError::Success)
            Diagnostics::Fatal("Syscall", Architecture::AMD64::SystemCallEntry::Describe(syscall_error));

        if (!architecture.Interrupts.BindUserFaultHandler(&Processes::HandleCurrentUserFault) ||
            !userspace.SystemCalls.BindUserFaultHandler(&Processes::HandleCurrentUserFault))
            Diagnostics::Fatal("User", "failed to bind native user fault containment");
    }
}
