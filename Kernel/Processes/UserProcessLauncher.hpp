#pragma once

#include <Kernel/Executables/ExecutableImageView.hpp>
#include <Kernel/Executables/UserExecutableLoader.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Processes {
    enum class UserProcessLaunchState : Memory::Uint32 {
        Empty,
        Preparing,
        Runnable,
        Failed,
    };

    enum class UserProcessLaunchError : Memory::Uint32 {
        Success,
        AlreadyLaunched,
        InvalidDependency,
        ProcessInitializationFailed,
        ImageLoadFailed,
        ThreadInitializationFailed,
        UserModeConfigurationFailed,
        SchedulerRegistrationFailed,
    };

    /*
     * Permanent ownership root for one isolated, single-threaded native
     * process. A launched instance must not be moved or destroyed while its
     * thread can run.
     *
     * Terminated process resources remain retained until zOS gains a safe
     * process/thread reaping protocol. Object references and process-local
     * handles are released independently of that later storage reclamation.
     */
    class UserProcessInstance final {
    public:
        UserProcessInstance() = default;

        UserProcessInstance(const UserProcessInstance&) = delete;
        UserProcessInstance& operator=(const UserProcessInstance&) = delete;

        [[nodiscard]] UserProcessLaunchError Launch(ProcessId process_id, Scheduling::ThreadId thread_id, const Executables::ExecutableImageView& executable, const Executables::UserExecutableLoadOptions& load_options,
            Memory::Uint64 kernel_stack_pages, Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& kernel_addresses, Architecture::AMD64::PageMap& kernel_page_map, Scheduling::Scheduler& scheduler) noexcept;

        [[nodiscard]] bool IsLaunched() const noexcept { return m_State == UserProcessLaunchState::Runnable; }
        [[nodiscard]] UserProcessLaunchState LaunchState() const noexcept { return m_State; }
        [[nodiscard]] UserProcessLaunchError LastLaunchError() const noexcept { return m_LastError; }

        [[nodiscard]] Process& ProcessObject() noexcept { return m_Process; }
        [[nodiscard]] const Process& ProcessObject() const noexcept { return m_Process; }

        [[nodiscard]] Scheduling::Thread& MainThread() noexcept { return m_Thread; }
        [[nodiscard]] const Scheduling::Thread& MainThread() const noexcept { return m_Thread; }

        [[nodiscard]] const Executables::LoadedUserImage& Image() const noexcept { return m_Image; }

        [[nodiscard]] static const char* Describe(UserProcessLaunchError error) noexcept;

    private:
        static void ThreadEntry(void* context) noexcept;

        Process m_Process{};
        Scheduling::Thread m_Thread{};
        Executables::LoadedUserImage m_Image{};
        UserProcessLaunchState m_State{ UserProcessLaunchState::Empty };
        UserProcessLaunchError m_LastError{ UserProcessLaunchError::Success };
    };
}
