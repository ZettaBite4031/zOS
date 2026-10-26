#include <Kernel/Objects/SystemCalls.hpp>

#include <ABI/SystemCalls.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>

namespace Zos::Kernel::Objects::SystemCalls {
    Memory::Uint64 CloseHandle(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        const Handle handle = Handle::FromValue(raw_handle);

        const HandleError error = context.CurrentProcess.Handles().Close(handle);

        return error == HandleError::Success ? 0 : ABI::InvalidSystemCallResult;
    }
}
