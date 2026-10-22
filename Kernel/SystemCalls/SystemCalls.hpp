#pragma once

#include <ABI/SystemCalls.hpp>
#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Processes {
    class Process;
}

namespace Zos::Kernel::SystemCalls {
    using Number = ABI::SystemCallNumber;
    using NativeSystemCall = ABI::NativeSystemCall;

    inline constexpr Memory::Uint64 InvalidSystemCallResult{ ABI::InvalidSystemCallResult };

    /*
     * Architecture-neutral request presented to the native zOS syscall layer.
     *
     * Native AMD64 ABI:
     *   RAX              syscall number / return value
     *   RDI, RSI, RDX,
     *   R10, R8, R9      arguments 0..5
     *   RCX, R11         architecturally clobbered by SYSCALL/SYSRET
     *   other GPRs       preserved
     *
     * InvalidSystemCallResult is returned for an unknown native call number.
     * The native layer itself does not depend on the architectural register frame.
     */
    struct Request final {
        Number CallNumber{};
        Memory::Uint64 Arguments[6]{};
    };

    [[nodiscard]] Memory::Uint64 DispatchNative(
        const Request& request,
        Processes::Process& process
    ) noexcept;
}
