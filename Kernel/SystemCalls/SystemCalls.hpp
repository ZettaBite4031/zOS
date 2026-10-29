#pragma once

#include <ABI/SystemCalls.hpp>
#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Processes {
    class Process;
}

namespace Zos::Kernel::Scheduling {
    class Scheduler;
    class Thread;
}

namespace Zos::Kernel::SystemCalls {
    using Number = ABI::SystemCallNumber;
    using NativeSystemCall = ABI::NativeSystemCall;

    inline constexpr Memory::Uint64 InvalidSystemCallResult{ ABI::InvalidSystemCallResult };

    struct Request final {
        Number CallNumber{};
        Memory::Uint64 Arguments[6]{};
    };

    /*
     * Architecture-neutral execution identity presented to native syscall
     * implementations. Subsystems receive exactly the scheduler/process/thread
     * context that a syscall may act on, without reaching through KernelRuntime.
     */
    struct Context final {
        Processes::Process& CurrentProcess;
        Scheduling::Thread& CurrentThread;
        Scheduling::Scheduler& CurrentScheduler;
    };

    [[nodiscard]] Memory::Uint64 DispatchNative(const Request& request, Context& context) noexcept;
}
