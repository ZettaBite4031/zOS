#include <Kernel/Processes/UserProcessLauncher.hpp>

#include <Kernel/Architecture/AMD64/Instructions.hpp>
#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Processes {
    UserProcessLaunchError UserProcessInstance::Prepare(ProcessId process_id, Scheduling::ThreadId thread_id, const Executables::ExecutableImageView& executable, const Executables::UserExecutableLoadOptions& load_options,
        Memory::Uint64 kernel_stack_pages, Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& kernel_addresses, Architecture::AMD64::PageMap& kernel_page_map) noexcept {
        if (m_State != UserProcessLaunchState::Empty || m_Process.IsInitialized() || m_Thread.IsInitialized() || m_Image.IsLoaded())
            return UserProcessLaunchError::AlreadyLaunched;

        if (process_id == 0 || thread_id == 0 || !executable.IsValid() || kernel_stack_pages == 0 || !physical_memory.IsInitialized() ||
            !kernel_addresses.IsInitialized() || !kernel_page_map.IsInitialized() || !kernel_page_map.IsActive())
            return UserProcessLaunchError::InvalidDependency;

        m_State = UserProcessLaunchState::Preparing;
        const auto process_error = m_Process.InitializeIsolated(process_id, physical_memory, kernel_page_map);
        if (process_error != ProcessInitializationError::Success) {
            m_State = UserProcessLaunchState::Failed;
            return m_LastError = UserProcessLaunchError::ProcessInitializationFailed;
        }

        const auto image_error = Executables::UserExecutableLoader::LoadStaticElf(executable.Data, executable.Size, m_Process, physical_memory, load_options, m_Image);
        if (image_error != Executables::UserExecutableLoadError::Success) {
            m_State = UserProcessLaunchState::Failed;
            return m_LastError = UserProcessLaunchError::ImageLoadFailed;
        }

        const auto thread_error = m_Thread.InitializeKernel(thread_id, m_Process, &ThreadEntry, this, physical_memory, kernel_addresses, kernel_page_map, kernel_stack_pages);
        if (thread_error != Scheduling::ThreadInitializationError::Success) {
            m_State = UserProcessLaunchState::Failed;
            return m_LastError = UserProcessLaunchError::ThreadInitializationFailed;
        }

        const auto user_mode_error = m_Thread.ConfigureUserMode(m_Image.EntryPoint(), m_Image.StackTop());
        if (user_mode_error != Scheduling::ThreadUserModeError::Success) {
            m_State = UserProcessLaunchState::Failed;
            return m_LastError = UserProcessLaunchError::UserModeConfigurationFailed;
        }

        m_State = UserProcessLaunchState::Prepared;
        m_LastError = UserProcessLaunchError::Success;
        return UserProcessLaunchError::Success;
    }

    UserProcessLaunchError UserProcessInstance::Start(Scheduling::Scheduler& scheduler) noexcept {
        if (m_State != UserProcessLaunchState::Prepared || !m_Process.IsInitialized() || !m_Thread.IsInitialized() ||
            !m_Image.IsLoaded() || !m_Thread.UserModeConfigured())
            return UserProcessLaunchError::InvalidDependency;

        if (!scheduler.IsInitialized() || !scheduler.PreemptionEnabled() || !Architecture::AMD64::InterruptsEnabled())
            return UserProcessLaunchError::InvalidDependency;

        Scheduling::SchedulerRegistrationError registration_error{};

        {
            Architecture::AMD64::ScopedInterruptDisable guard{};
            registration_error = scheduler.RegisterThread(m_Thread);
            if (registration_error == Scheduling::SchedulerRegistrationError::Success)
                m_State = UserProcessLaunchState::Runnable;
            else m_State = UserProcessLaunchState::Failed;
        }

        if (registration_error != Scheduling::SchedulerRegistrationError::Success)
            return m_LastError = UserProcessLaunchError::SchedulerRegistrationFailed;

        m_LastError = UserProcessLaunchError::Success;
        return UserProcessLaunchError::Success;
    }

    UserProcessLaunchError UserProcessInstance::Launch(ProcessId process_id, Scheduling::ThreadId thread_id, const Executables::ExecutableImageView& executable, const Executables::UserExecutableLoadOptions& load_options, Memory::Uint64 kernel_stack_pages,
        Memory::PhysicalMemoryManager& physical_memory, Memory::VirtualAddressAllocator& kernel_addresses, Architecture::AMD64::PageMap& kernel_page_map, Scheduling::Scheduler& scheduler) noexcept {
        const UserProcessLaunchError prepare_error = Prepare(process_id, thread_id, executable, load_options, kernel_stack_pages, physical_memory, kernel_addresses, kernel_page_map);
        if (prepare_error != UserProcessLaunchError::Success) return prepare_error;
        return Start(scheduler);
    }

    void UserProcessInstance::ThreadEntry(void* context) noexcept {
        auto* instance = static_cast<UserProcessInstance*>(context);

        if (instance == nullptr || !instance->IsLaunched() || !instance->m_Image.IsLoaded() ||
            !instance->m_Thread.UserModeConfigured() || instance->m_Process.State() != ProcessState::Active)
            Diagnostics::Fatal("Process", "invalid native user launch context");

        auto* processor = Architecture::AMD64::CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread != &instance->m_Thread ||
            instance->m_Thread.OwningProcess() != &instance->m_Process ||
            !instance->m_Process.AddressSpace()->IsActive())
            Diagnostics::Fatal("Process", "native user launch lost thread or CR3 ownership");

        instance->m_Thread.EnterUserMode();
    }

    const char* UserProcessInstance::Describe(UserProcessLaunchError error) noexcept {
        switch (error) {
        case UserProcessLaunchError::Success: return "success";
        case UserProcessLaunchError::AlreadyLaunched: return "process instance already used";
        case UserProcessLaunchError::InvalidDependency: return "invalid process launch dependency";
        case UserProcessLaunchError::ProcessInitializationFailed: return "process address-space initialization failed";
        case UserProcessLaunchError::ImageLoadFailed: return "native ELF image load failed";
        case UserProcessLaunchError::ThreadInitializationFailed: return "process main thread initialization failed";
        case UserProcessLaunchError::UserModeConfigurationFailed: return "process user-mode configuration failed";
        case UserProcessLaunchError::SchedulerRegistrationFailed: return "process scheduling registration failed";
        default: return "unknown process launch error";
        }
    }
}
