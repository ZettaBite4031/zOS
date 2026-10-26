#include <Kernel/Scheduling/SystemCalls.hpp>

#include <ABI/Handles.hpp>
#include <ABI/SystemCalls.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Objects/HandleTable.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>

namespace Zos::Kernel::Scheduling::SystemCalls {
    Memory::Uint64 OpenCurrentThread(::Zos::Kernel::SystemCalls::Context& context) noexcept {
        Objects::Handle handle{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Open(context.CurrentThread, ThreadQuery, handle);

        return error == Objects::HandleError::Success ? handle.Value() : ABI::InvalidHandleValue;
    }

    Memory::Uint64 GetThreadIdFromHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Thread> thread{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), ThreadQuery, thread);

        return error == Objects::HandleError::Success ? thread->Id() : ABI::InvalidSystemCallResult;
    }
}
