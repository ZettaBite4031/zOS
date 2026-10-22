#include <Kernel/Processes/Process.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Scheduling/Thread.hpp>

namespace Zos::Kernel::Processes {
    ProcessInitializationError Process::InitializeKernel(ProcessId id) noexcept {
        if (m_Initialized) return ProcessInitializationError::AlreadyInitialized;

        m_Id = id;
        m_State = ProcessState::Active;

        m_ThreadHead = nullptr;
        m_ThreadTail = nullptr;
        m_ThreadCount = 0;

        m_Initialized = true;

        Print();
        return ProcessInitializationError::Success;
    }

    void Process::AttachThread(Scheduling::Thread& thread) noexcept {
        /*
         * Thread initialization guarantees that a fresh Thread reacehs this
         * point exactly once and that the owning Process is already active.
         */
        thread.m_OwningProcess = this;
        thread.m_ProcessNext = nullptr;

        if (m_ThreadTail != nullptr) m_ThreadTail->m_ProcessNext = &thread;
        else m_ThreadHead = &thread;

        m_ThreadTail = &thread;
        m_ThreadCount++;
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
        Diagnostics::Write("[zOS/Process] Kernel process initialized.\n");
        Diagnostics::Write("[zOS/Process] Process ID: ");
        Diagnostics::WriteDecimal(m_Id);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/Process] State: ");
        Diagnostics::Write(StateName(m_State));
        Diagnostics::Write("\n");
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
        default: return "unknown process initialization error";
        }
    }
}
