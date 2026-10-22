#pragma once

#include <Kernel/Executables/EmbeddedUserImages.hpp>
#include <Kernel/Executables/UserExecutableLoader.hpp>
#include <Kernel/Processes/Process.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>
#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>

namespace Zos::Kernel::Processes {
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
     * thread can run. Step 20 intentionally retains terminated resources;
     * reclamation requires a separate safe teardown protocol.
     */
    class UserProcessInstance final {
    public:
        UserProcessInstance() = default;
        UserProcessInstance(const UserProcessInstance&) = delete;
        UserProcessInstance& operator=(const UserProcessInstance&) = delete;

        [[nodiscard]] UserProcessLaunchError Launch(
            ProcessId process_id,
            Scheduling::ThreadId thread_id,
            const Executables::EmbeddedImage& executable,
            const Executables::UserExecutableLoadOptions& load_options,
            Memory::Uint64 kernel_stack_pages,
            Memory::PhysicalMemoryManager& physical_memory,
            Memory::VirtualAddressAllocator& kernel_addresses,
            Architecture::AMD64::PageMap& kernel_page_map,
            Scheduling::Scheduler& scheduler
        ) noexcept;

        [[nodiscard]] bool IsLaunched() const noexcept { return m_Launched; }
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
        bool m_Launched{};
    };
}
