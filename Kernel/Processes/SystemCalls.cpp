#include <Kernel/Processes/SystemCalls.hpp>

#include <ABI/Handles.hpp>
#include <ABI/SystemCalls.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Objects/HandleTable.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>

namespace Zos::Kernel::Processes::SystemCalls {
    Memory::Uint64 GetProcessId(::Zos::Kernel::SystemCalls::Context& context) noexcept {
        return context.CurrentProcess.Id();
    }

    Memory::Uint64 OpenCurrentProcess(::Zos::Kernel::SystemCalls::Context& context) noexcept {
        Objects::Handle handle{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Open(context.CurrentProcess, ProcessQuery, handle );

        return error == Objects::HandleError::Success ? handle.Value() : ABI::InvalidHandleValue;
    }

    Memory::Uint64 GetProcessIdFromHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Process> process{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), ProcessQuery, process);

        return error == Objects::HandleError::Success ? process->Id() : ABI::InvalidSystemCallResult;
    }

    [[noreturn]] void ExitProcess(::Zos::Kernel::SystemCalls::Context&, Memory::Uint64 status) noexcept {
        ExitCurrentUserProcess(status);
    }
}
