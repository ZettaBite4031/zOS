#include <Kernel/Processes/Process.hpp>

#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Scheduling/Thread.hpp>
#include <Kernel/Scheduling/Scheduler.hpp>

namespace Zos::Kernel::Processes {
    ProcessInitializationError Process::InitializeKernel(ProcessId id, Architecture::AMD64::PageMap& kernel_page_map) noexcept {
        if (m_Initialized || m_PageMap != nullptr) return ProcessInitializationError::AlreadyInitialized;
        if (!kernel_page_map.IsInitialized() || !kernel_page_map.IsMetadataPromoted() || !kernel_page_map.IsActive())
            return ProcessInitializationError::InvalidDependency;

        m_Id = id;
        m_State = ProcessState::Active;

        m_PageMap = &kernel_page_map;
        m_OwnsPageMap = false;

        m_ThreadHead = nullptr;
        m_ThreadTail = nullptr;
        m_ThreadCount = 0;

        m_Initialized = true;

        m_FaultRecord = {};
        m_ExitRecord = {};

        Print();
        return ProcessInitializationError::Success;
    }

    ProcessInitializationError Process::InitializeIsolated(ProcessId id, Memory::PhysicalMemoryManager& physical_memory, const Architecture::AMD64::PageMap& kernel_page_map) noexcept {
        if (m_Initialized || m_PageMap != nullptr || m_OwnedPageMap.IsInitialized())
            return ProcessInitializationError::AlreadyInitialized;

        if (!physical_memory.IsInitialized() || !physical_memory.IsMetadataAccessPromoted() ||
            !kernel_page_map.IsInitialized() || !kernel_page_map.IsMetadataPromoted())
            return ProcessInitializationError::InvalidDependency;

        const auto initialization_error = m_OwnedPageMap.InitializeRuntime(physical_memory);

        if (initialization_error != Architecture::AMD64::PageMapInitializationError::Success)
            return ProcessInitializationError::AddressSpaceInitializationFailed;

        const auto install_error = m_OwnedPageMap.InstallKernelMappingsFrom(kernel_page_map);
        if (install_error != Architecture::AMD64::PageMapKernelMappingInstallError::Success)
            return ProcessInitializationError::KernelMappingInstallFailed;

        if (!m_OwnedPageMap.Validate() || m_OwnedPageMap.RootTable() == kernel_page_map.RootTable())
            return ProcessInitializationError::KernelMappingInstallFailed;

        m_Id = id;
        m_State = ProcessState::Active;

        m_PageMap = &m_OwnedPageMap;
        m_OwnsPageMap = true;

        m_ThreadHead = nullptr;
        m_ThreadTail = nullptr;
        m_ThreadCount = 0;

        m_Initialized = true;

        m_FaultRecord = {};
        m_ExitRecord = {};

        Print();
        return ProcessInitializationError::Success;
    }

    void Process::NotifyThreadTerminated(Scheduling::Thread& thread) noexcept {
        if (!m_Initialized || thread.m_OwningProcess != this)
            Diagnostics::Fatal("Process", "terminated thread does not belong to its process");

        /*
        * Step 16 has one user thread, but make the state transition depend on
        * membership rather than hardcoding that fact. A future multi-thread
        * process remains Terminating until every member has stopped.
        */
        if (m_State != ProcessState::Terminating) return;
        for (Scheduling::Thread* current = m_ThreadHead; current != nullptr; current = current->m_ProcessNext) {
            if (current->m_State != Scheduling::ThreadState::Terminated) return;
        }

        m_State = ProcessState::Terminated;
    }

    void Process::AttachThread(Scheduling::Thread& thread) noexcept {
        /*
         * Thread initialization guarantees that a fresh Thread reaches this
         * point exactly once and that the owning Process is already active.
         */
        thread.m_OwningProcess = this;
        thread.m_ProcessNext = nullptr;

        if (m_ThreadTail != nullptr) m_ThreadTail->m_ProcessNext = &thread;
        else m_ThreadHead = &thread;

        m_ThreadTail = &thread;
        m_ThreadCount++;
    }

    bool Process::BeginUserFaultTermination(Memory::Uint64 thread_id, const UserFault& fault) noexcept {
        if (!m_Initialized || !m_OwnsPageMap || m_State != ProcessState::Active || m_FaultRecord.Valid || m_ExitRecord.Valid)
            return false;

        m_FaultRecord = {
            .Valid = true,
            .ThreadId = thread_id,
            .Fault = fault,
        };

        m_State = ProcessState::Terminating;
        return true;
    }

    bool Process::BeginNormalExit(Memory::Uint64 thread_id, Memory::Uint32 status) noexcept {
        if (!m_Initialized || !m_OwnsPageMap || m_State != ProcessState::Active ||
            m_ThreadCount != 1 || m_ExitRecord.Valid || m_FaultRecord.Valid)
            return false;

        m_ExitRecord = {
            .Valid = true,
            .ThreadId = thread_id,
            .Status = status,
        };

        m_State = ProcessState::Terminating;
        return true;
    }

    bool Process::OwnsThread(const Scheduling::Thread& thread) const noexcept {
        if (!m_Initialized || thread.m_OwningProcess != this) return false;

        const Scheduling::Thread* current = m_ThreadHead;
        while (current != nullptr) {
            if (current == &thread) return true;
            current = current->m_ProcessNext;
        }

        return false;
    }

    void Process::Print() const noexcept {
        Diagnostics::Write(m_OwnsPageMap ? "[zOS/Process] Isolated process initialized.\n" : "[zOS/Process] Kernel process initialized.\n");

        Diagnostics::Write("[zOS/Process] Process ID: ");
        Diagnostics::WriteDecimal(m_Id);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Process] State: ");
        Diagnostics::Write(StateName(m_State));
        Diagnostics::Write("\n");

        if (m_PageMap != nullptr) {
            Diagnostics::Write("[zOS/Process] Address-space ownership: ");
            Diagnostics::Write(m_OwnsPageMap ? "owned\n" : "borrowed\n");
            Diagnostics::Write("[zOS/Process] CR3 root: ");
            Diagnostics::WriteHex(m_PageMap->RootTable().Value());
            Diagnostics::Write("\n");
        }
    }

    const char* Process::StateName(ProcessState state) noexcept {
        switch (state) {
        case ProcessState::Active: return "active";
        case ProcessState::Terminating: return "terminating";
        case ProcessState::Terminated: return "terminated";
        default: return "unknown";
        }
    }

    const char* Process::Describe(ProcessInitializationError error) noexcept {
        switch (error) {
        case ProcessInitializationError::Success: return "success";
        case ProcessInitializationError::AlreadyInitialized: return "process is already initialized";
        case ProcessInitializationError::InvalidDependency: return "process initialization dependency is invalid";
        case ProcessInitializationError::AddressSpaceInitializationFailed: return "failed to initialize the process address space";
        case ProcessInitializationError::KernelMappingInstallFailed: return "failed to install permanent kernel mappings into the process";
        default: return "unknown process initialization error";
        }
    }

    [[noreturn]] void ExitCurrentUserProcess(Memory::Uint64 status) noexcept {
        using Architecture::AMD64::CurrentProcessor;

        Architecture::AMD64::ProcessorLocal* processor = CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread == nullptr || processor->LocalScheduler == nullptr)
            Diagnostics::Fatal("Process", "process exit arrived without scheduler ownership");

        Scheduling::Thread* thread = processor->CurrentThread;
        Process* process = thread->OwningProcess();

        if (process == nullptr || !process->IsInitialized() || !process->OwnsAddressSpace() || 
            !process->OwnsThread(*thread) || !thread->UserModeConfigured() || process->State() != ProcessState::Active)
            Diagnostics::Fatal("Process", "invalid user process exit context");

        if (!process->BeginNormalExit(thread->Id(), static_cast<Memory::Uint32>(status)))
            Diagnostics::Fatal("Process", "failed to begin normal process termination");

        Diagnostics::Write("[zOS/Process] Native process exited. PID: ");
        Diagnostics::WriteDecimal(process->Id());
        Diagnostics::Write(", status: ");
        Diagnostics::WriteDecimal(process->ExitRecord().Status);
        Diagnostics::Write("\n");

        processor->LocalScheduler->TerminateCurrent();
        
        __builtin_unreachable();
    }

    [[noreturn]] void HandleCurrentUserFault(const UserFault& fault) noexcept {
        using Architecture::AMD64::CurrentProcessor;

        Architecture::AMD64::ProcessorLocal* processor = CurrentProcessor();
        if (processor == nullptr || processor->CurrentThread == nullptr || processor->LocalScheduler == nullptr)
            Diagnostics::Fatal("User", "user fault arrived without scheduler ownership");

        Scheduling::Thread* thread = processor->CurrentThread;
        Process* process = thread->OwningProcess();
        if (process == nullptr || !process->OwnsAddressSpace() || process->State() != ProcessState::Active)
            Diagnostics::Fatal("User", "faulting user thread has invalid process ownership");

        if (!process->BeginUserFaultTermination(thread->Id(), fault))
            Diagnostics::Fatal("User", "failed to begin user-process fault termination");

        const char* fault_name = "unknown";
        switch (fault.Kind) {
        case UserFaultKind::InvalidOpcode: fault_name = "invalid opcode"; break;
        case UserFaultKind::GeneralProtection: fault_name = "general protection"; break;
        case UserFaultKind::PageFault: fault_name = "page fault"; break;
        case UserFaultKind::InvalidSystemCallState: fault_name = "invalid syscall return state"; break;
        }

        Diagnostics::Write("[zOS/User] Fatal user fault contained.\n");
        Diagnostics::Write("[zOS/User] Process ID: ");
        Diagnostics::WriteDecimal(process->Id());
        Diagnostics::Write(", thread ID: ");
        Diagnostics::WriteDecimal(thread->Id());
        Diagnostics::Write(", fault: ");
        Diagnostics::Write(fault_name);
        Diagnostics::Write("\n");

        /*
         * Success never returns. The faulting architecture frame is abandoned
         * on the terminated thread's kernel stack rather than returning to
         * userspace.
         */
        processor->LocalScheduler->TerminateCurrent();

        __builtin_unreachable();
    }
}
