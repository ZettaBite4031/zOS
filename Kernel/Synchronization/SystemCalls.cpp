#include <Kernel/Synchronization/SystemCalls.hpp>

#include <ABI/SystemCalls.hpp>
#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Objects/HandleTable.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/SystemCalls/SystemCalls.hpp>
#include <Kernel/Synchronization/Event.hpp>
#include <Kernel/Synchronization/Mutex.hpp>
#include <Kernel/Synchronization/Semaphore.hpp>
#include <Kernel/Synchronization/Timer.hpp>

namespace Zos::Kernel::Synchronization::SystemCalls {
    namespace {
        [[nodiscard]] Memory::Uint64 Result(SynchronizationError error) noexcept {
            return error == SynchronizationError::Success ? 0 : ABI::InvalidSystemCallResult;
        }
    }

    Memory::Uint64 WaitForObject(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Objects::Object> object{};

        const Objects::HandleError handle_error = context.CurrentProcess.Handles().AcquireAny(Objects::Handle::FromValue(raw_handle), WaitRight, object);
        if (handle_error != Objects::HandleError::Success) return ABI::InvalidSystemCallResult;

        switch (object->Type()) {
        case Objects::ObjectType::Event: return Result(static_cast<Event&>(*object).Wait(context.CurrentScheduler));
        case Objects::ObjectType::Semaphore: return Result(static_cast<Semaphore&>(*object).Wait(context.CurrentScheduler));
        case Objects::ObjectType::Mutex: return Result(static_cast<Mutex&>(*object).Wait(context.CurrentScheduler));
        case Objects::ObjectType::Timer: return Result(static_cast<Timer&>(*object).Wait(context.CurrentScheduler));
        default: return ABI::InvalidSystemCallResult;
        }
    }

    Memory::Uint64 SignalEvent(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Event> event{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), EventSignalRight, event);
        if (error != Objects::HandleError::Success) return ABI::InvalidSystemCallResult;
        return Result(event->Signal(context.CurrentScheduler));
    }

    Memory::Uint64 ResetEvent(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Event> event{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), EventResetRight, event);
        if (error != Objects::HandleError::Success) return ABI::InvalidSystemCallResult;
        return Result(event->Reset());
    }

    Memory::Uint64 ReleaseSemaphore(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle, Memory::Uint64 count) noexcept {
        Objects::ObjectReference<Semaphore> semaphore{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), SemaphoreReleaseRight, semaphore);
        if (error != Objects::HandleError::Success) return ABI::InvalidSystemCallResult;
        return Result(semaphore->Release(context.CurrentScheduler, count));
    }

    Memory::Uint64 ReleaseMutex(::Zos::Kernel::SystemCalls::Context& context, Memory::Uint64 raw_handle) noexcept {
        Objects::ObjectReference<Mutex> mutex{};

        const Objects::HandleError error = context.CurrentProcess.Handles().Acquire(Objects::Handle::FromValue(raw_handle), MutexReleaseRight, mutex);
        if (error != Objects::HandleError::Success) return ABI::InvalidSystemCallResult;
        return Result(mutex->Release(context.CurrentScheduler));
    }
}
