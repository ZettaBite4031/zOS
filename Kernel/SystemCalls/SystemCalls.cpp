#include <Kernel/SystemCalls/SystemCalls.hpp>

#include <Kernel/Objects/SystemCalls.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Processes/SystemCalls.hpp>
#include <Kernel/Scheduling/SystemCalls.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::SystemCalls {
    Memory::Uint64 DispatchNative(const Request& request, Context& context) noexcept {
        if (!context.CurrentProcess.IsInitialized() || context.CurrentProcess.State() != Processes::ProcessState::Active ||
            !context.CurrentThread.IsInitialized() || context.CurrentThread.OwningProcess() != &context.CurrentProcess)
            return InvalidSystemCallResult;

        switch (static_cast<NativeSystemCall>(request.CallNumber)) {
        case NativeSystemCall::GetProcessId: return Processes::SystemCalls::GetProcessId(context);
        case NativeSystemCall::ExitProcess: Processes::SystemCalls::ExitProcess(context, request.Arguments[0]);
        case NativeSystemCall::OpenCurrentProcess: return Processes::SystemCalls::OpenCurrentProcess(context);
        case NativeSystemCall::OpenCurrentThread: return Scheduling::SystemCalls::OpenCurrentThread(context);
        case NativeSystemCall::CloseHandle: return Objects::SystemCalls::CloseHandle(context, request.Arguments[0]);
        case NativeSystemCall::GetProcessIdFromHandle: return Processes::SystemCalls::GetProcessIdFromHandle(context, request.Arguments[0]);
        case NativeSystemCall::GetThreadIdFromHandle: return Scheduling::SystemCalls::GetThreadIdFromHandle(context, request.Arguments[0]);
        default: return InvalidSystemCallResult;
        }
    }
}
