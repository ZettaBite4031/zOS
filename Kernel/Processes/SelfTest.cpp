#include <Kernel/Processes/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Objects/Object.hpp>

namespace Zos::Kernel::Processes::SelfTest {
    namespace {
        inline constexpr Memory::VirtualAddress ProbeAddress{ 0x0000010000000000ULL };
        inline constexpr Memory::Uint64 ProbeValue{ 0x5A4F535041434531ULL };
    }

    void VerifyKernelProcess(Process& process, Architecture::AMD64::PageMap& kernel_page_map) noexcept {
        const auto& object = static_cast<const Objects::Object&>(process);

        if (!process.IsInitialized() || process.Id() != 0 || process.State() != ProcessState::Active ||
            process.ThreadCount() != 0 || process.OwnsAddressSpace() || process.AddressSpace() != &kernel_page_map ||
            !object.IsObjectInitialized() || object.Type() != Objects::ObjectType::Process || object.Id() == 0 ||
            object.ReferenceCount() != 1 || !process.Handles().IsInitialized() || process.Handles().ActiveCount() != 0 ||
            !process.Handles().Validate())
            Diagnostics::Fatal("Process", "kernel process initial state is invalid");

        Diagnostics::Write("[zOS/Process] Kernel process object and handle table verified.\n");
    }

    void VerifyIsolatedAddressSpace(Process& process, Memory::State& memory) noexcept {
        using namespace Memory;

        auto* process_space = process.AddressSpace();

        const auto& object = static_cast<const Objects::Object&>(process);

        if (!process.IsInitialized() || !process.OwnsAddressSpace() || process.State() != ProcessState::Active ||
            process_space == nullptr || process_space->RootTable() == memory.KernelPageMap.RootTable() ||
            !object.IsObjectInitialized() || object.Type() != Objects::ObjectType::Process || object.Id() == 0 ||
            object.ReferenceCount() != 1 || !process.Handles().IsInitialized() || process.Handles().ActiveCount() != 0 ||
            !process.Handles().Validate())
            Diagnostics::Fatal("Process", "isolated process ownership state is invalid");

        if (memory.KernelPageMap.IsMapped(ProbeAddress) || process_space->IsMapped(ProbeAddress))
            Diagnostics::Fatal("Process", "private test address is not initially isolated");

        PhysicalAllocation private_page{};

        const auto allocation_error = memory.Physical.AllocatePage(private_page);
        if (allocation_error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Process", PhysicalMemoryManager::Describe(allocation_error));

        const VirtualAddress direct = Layout::DirectMapAddress(private_page.Base());
        if (direct.IsNull())
            Diagnostics::Fatal("Process", "private test page is outside the direct map");

        auto* direct_value = reinterpret_cast<volatile Uint64*>(direct.Value());

        *direct_value = ProbeValue;

        const MappingOptions user_private{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::User,
            .Cache = CachePolicy::WriteBack,
        };

        const auto mapping_error = process_space->MapPage(ProbeAddress, private_page.Base(), user_private);
        if (mapping_error != Architecture::AMD64::MappingError::Success)
            Diagnostics::Fatal("Process", Architecture::AMD64::PageMap::Describe(mapping_error));

        const auto translation = process_space->Translate(ProbeAddress);
        if (!translation.Mapped || translation.Physical != private_page.Base() ||
            !HasAccess(translation.Options.Access, PageAccess::Read) ||
            !HasAccess(translation.Options.Access, PageAccess::Write) ||
            !HasAccess(translation.Options.Access, PageAccess::User) ||
            HasAccess(translation.Options.Access, PageAccess::Execute) ||
            HasAccess(translation.Options.Access, PageAccess::Global) ||
            memory.KernelPageMap.IsMapped(ProbeAddress) || *direct_value != ProbeValue)
            Diagnostics::Fatal("Process", "private user mapping failed isolation validation");

        if (process_space->UnmapPage(ProbeAddress ) !=Architecture::AMD64::MappingError::Success)
            Diagnostics::Fatal("Process", "failed to unmap process isolation probe");

        if (memory.Physical.Release(private_page) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Process", "failed to release process isolation probe");

        Diagnostics::Write("[zOS/Process] Independent process address space established.\n");
        Diagnostics::Write("[zOS/Process] Private user mapping isolation verified.\n");
    }
}
