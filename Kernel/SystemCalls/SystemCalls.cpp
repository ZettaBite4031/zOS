#include <Kernel/SystemCalls/SystemCalls.hpp>

#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel::SystemCalls {
    Memory::Uint64 DispatchNative(const Request& request, const Processes::Process& process) noexcept {
        if (!process.IsInitialized() || process.State() != Processes::ProcessState::Active)
            return InvalidSystemCallResult;

        switch (static_cast<NativeSystemCall>(request.CallNumber)) {
        case NativeSystemCall::GetProcessId:
            return process.Id();
        default:
            return InvalidSystemCallResult;
        }
    }
}
