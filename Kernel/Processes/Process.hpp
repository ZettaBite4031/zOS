#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Scheduling {
    class Thread;
}

namespace Zos::Kernel::Architecture::AMD64 {
    struct InterruptContext;
}

namespace Zos::Kernel::Processes {
    using ProcessId = Memory::Uint64;

    enum class ProcessState : Memory::Uint32 {
        Active,
        Terminating,
        Terminated,
    };

    enum class ProcessInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        AddressSpaceInitializationFailed,
        KernelMappingInstallFailed,
    };

    struct ProcessFaultRecord final {
        bool Valid{};
        Memory::Uint64 ThreadId{};
        Memory::Uint64 Vector{};
        Memory::Uint64 ErrorCode{};
        Memory::Uint64 FaultAddress{};
        Memory::Uint64 InstructionPointer{};
    };

    class Process final {
    public:
        constexpr Process() noexcept = default;

        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        /*
         * PID 0 borrows the already-active kernel page map.
         */
        [[nodiscard]] ProcessInitializationError InitializeKernel(ProcessId id, Architecture::AMD64::PageMap& kernel_page_map) noexcept;

        /*
         * Creates an independently rooted process address space.
         *
         * Permanent upper-half kernel branches are shared with the kernel
         * page map while required lower-half kernel mappings are reproduced
         * privately. User mappings therefore remain process-private.
         */
        [[nodiscard]] ProcessInitializationError InitializeIsolated(ProcessId id, Memory::PhysicalMemoryManager& physical_memory, const Architecture::AMD64::PageMap& kernel_page_map) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] ProcessId Id() const noexcept { return m_Id; }
        [[nodiscard]] ProcessState State() const noexcept { return m_State; }
        [[nodiscard]] Memory::Uint64 ThreadCount() const noexcept { return m_ThreadCount; }

        [[nodiscard]] Architecture::AMD64::PageMap* AddressSpace() noexcept { return m_PageMap; }
        [[nodiscard]] const Architecture::AMD64::PageMap* AddressSpace() const noexcept { return m_PageMap; }
        [[nodiscard]] bool OwnsAddressSpace() const noexcept { return m_OwnsPageMap; }
        [[nodiscard]] const ProcessFaultRecord& FaultRecord() const noexcept { return m_FaultRecord; }

        [[nodiscard]] bool BeginUserFaultTermination(Memory::Uint64 thread_id, Memory::Uint64 vector,
            Memory::Uint64 error_code, Memory::Uint64 fault_address, Memory::Uint64 instruction_pointer) noexcept;

        [[nodiscard]] bool OwnsThread(const Scheduling::Thread& thread) const noexcept;

        [[nodiscard]] static const char* StateName(ProcessState state) noexcept;

        [[nodiscard]] static const char* Describe(ProcessInitializationError error) noexcept;

    private:
        friend class ::Zos::Kernel::Scheduling::Thread;

        void NotifyThreadTerminated(Scheduling::Thread& thread) noexcept;
        void AttachThread(Scheduling::Thread& thread) noexcept;
        void Print() const noexcept;

        ProcessId m_Id{};
        ProcessState m_State{ ProcessState::Terminated };

        Architecture::AMD64::PageMap m_OwnedPageMap{};
        Architecture::AMD64::PageMap* m_PageMap{};

        Scheduling::Thread* m_ThreadHead{};
        Scheduling::Thread* m_ThreadTail{};
        Memory::Uint64 m_ThreadCount{};
        ProcessFaultRecord m_FaultRecord{};

        bool m_OwnsPageMap{};
        bool m_Initialized{};
    };

    [[noreturn]] void HandleCurrentUserException(Architecture::AMD64::InterruptContext& context, Memory::Uint64 fault_address) noexcept;
}