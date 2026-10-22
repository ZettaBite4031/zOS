#include <Kernel/Architecture/AMD64/Paging.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    using namespace Memory;
    inline constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };

    bool PageMap::Validate() const noexcept {
        if (!IsInitialized() || m_PhysicalMemory == nullptr || !m_PhysicalMemory->IsInitialized() || m_RootTable.IsNull() || !m_RootTable.IsPageAligned() ||
            (m_RootTable.Value() & ~AddressMask) != 0)
            return false;

        if (m_KernelPageMap != nullptr) {
            if (m_KernelPageMap == this || !m_KernelPageMap->IsInitialized() || !m_KernelPageMap->IsMetadataPromoted())
                return false;

            const Entry* root = TablePointer(m_RootTable);
            const Entry* kernel_root = m_KernelPageMap->TablePointer(m_KernelPageMap->m_RootTable);
            if (root == nullptr || kernel_root == nullptr) return false;
            for (Uint64 index = 256; index < EntriesPerTable; index++)
                if (root[index] != kernel_root[index]) return false;
        }


        if (m_MetadataPromoted) {
            if (m_BootstrapMetadata != nullptr || m_PermanentMetadata.Head == nullptr || m_PermanentMetadata.Tail == nullptr) return false;
        } else {
            if (m_BootstrapMetadata == nullptr || !m_BootstrapMetadata->IsInitialized() || m_PermanentMetadata.Head != nullptr || m_PermanentMetadata.Tail != nullptr) return false;
        }

        const TableRecord* slow = m_TableRecords;
        const TableRecord* fast = m_TableRecords;
        while (fast != nullptr && fast->Next != nullptr) {
            slow = slow->Next;
            fast = fast->Next->Next;
            if (slow == fast) return false;
        }

        slow = m_RecycledTableRecords;
        fast = m_RecycledTableRecords;
        while (fast != nullptr && fast->NextFree != nullptr) {
            slow = slow->NextFree;
            fast = fast->NextFree->NextFree;
            if (slow == fast) return false;
        }

        Uint64 active_records = 0;
        bool root_found = false;
        const TableRecord* previous = nullptr;

        for (const TableRecord* record = m_TableRecords; record != nullptr; record = record->Next) {
            if (record->Previous != previous || record->Address.IsNull() || !record->Address.IsPageAligned() || (record->Address.Value() & ~AddressMask) != 0 ||
                !record->Ownership.IsValid() || record->Ownership.Base() != record->Address || record->Ownership.PageCount() != 1)
                return false;

            if (m_MetadataPromoted && !PermanentMetadataContains(record)) return false;
            if (record->Address == m_RootTable) root_found = true;

            for (const TableRecord* other = record->Next; other != nullptr; other = other->Next)
                if (other->Address == record->Address) return false;

            previous = record;
            if (active_records == MaximumValue) return false;
            active_records++;
        }

        if (!root_found || active_records != m_Statistics.TablePages) return false;

        Uint64 recycled_records = 0;
        for (const TableRecord* record = m_RecycledTableRecords; record != nullptr; record = record->NextFree) {
            if (!record->Address.IsNull() || record->Ownership.IsValid() || record->Previous != nullptr || record->Next != nullptr) return false;
            if (m_MetadataPromoted && !PermanentMetadataContains(record)) return false;

            for (const TableRecord* active = m_TableRecords; active != nullptr; active = active->Next)
                if (active == record) return false;

            if (recycled_records == MaximumValue) return false;
            recycled_records++;
        }

        if (!m_MetadataPromoted) return true;

        const MetadataPage* slow_page = m_PermanentMetadata.Head;
        const MetadataPage* fast_page = m_PermanentMetadata.Head;
        while (fast_page != nullptr && fast_page->Next != nullptr) {
            slow_page = slow_page->Next;
            fast_page = fast_page->Next->Next;
            if (slow_page == fast_page) return false;
        }

        Uint64 used_records = 0;
        const MetadataPage* last = nullptr;
        for (const MetadataPage* page = m_PermanentMetadata.Head; page != nullptr; page = page->Next) {
            if (!page->Ownership.IsValid() || page->Ownership.PageCount() != 1 || !Layout::IsDirectMappable(page->Ownership.Base()) ||
                Layout::DirectMapAddress(page->Ownership.Base()).Value() != reinterpret_cast<Uint64>(page) || page->UsedRecords > MetadataRecordsPerPage)
                return false;

            if (used_records > MaximumValue - page->UsedRecords) return false;
            used_records += page->UsedRecords;
            last = page;
        }

        if (last != m_PermanentMetadata.Tail || active_records > MaximumValue - recycled_records) return false;
        return used_records == active_records + recycled_records;
    }

    const char* PageMap::Describe(PageMapInitializationError error) noexcept {
        switch (error) {
        case PageMapInitializationError::Success: return "success";
        case PageMapInitializationError::AlreadyInitialized: return "page map is already initialized";
        case PageMapInitializationError::InvalidDependency: return "page map dependencies are not initialized";
        case PageMapInitializationError::PhysicalAllocationFailed: return "failed to allocate a physical page-table page";
        case PageMapInitializationError::MetadataAllocationFailed: return "failed to retain page-table ownership metadata";
        case PageMapInitializationError::PhysicalAddressUnsupported: return "page-table physical address cannot be encoded";
        default: return "unknown page map initialization error";
        }
    }

    const char* PageMap::Describe(PageMapMetadataPromotionError error) noexcept {
        switch (error) {
        case PageMapMetadataPromotionError::Success: return "success";
        case PageMapMetadataPromotionError::NotInitialized: return "page map is not initialized";
        case PageMapMetadataPromotionError::AlreadyPromoted: return "page-map metadata is already permanent";
        case PageMapMetadataPromotionError::InvalidDependency: return "page-map metadata promotion dependency is invalid";
        case PageMapMetadataPromotionError::CorruptState: return "page-map ownership state is corrupt";
        case PageMapMetadataPromotionError::AllocationFailed: return "failed to allocate permanent page-map metadata";
        case PageMapMetadataPromotionError::ValidationFailed: return "permanent page-map metadata failed validation";
        case PageMapMetadataPromotionError::RollbackFailed: return "failed to release candidate page-map metadata";
        default: return "unknown page-map metadata promotion error";
        }
    }

    const char* PageMap::Describe(PageMapKernelMappingInstallError error) noexcept {
        switch (error) {
        case PageMapKernelMappingInstallError::Success: return "success";
        case PageMapKernelMappingInstallError::NotInitialized: return "destination page map is not runtime initialized";
        case PageMapKernelMappingInstallError::InvalidSource: return "kernel page-map source is invalid";
        case PageMapKernelMappingInstallError::DestinationNotEmpty: return "destination page map is not root-only";
        case PageMapKernelMappingInstallError::UnsupportedLargePage: return "kernel mapping installation encountered a large page";
        case PageMapKernelMappingInstallError::MappingFailed: return "failed to reproduce a lower-half kernel mapping";
        case PageMapKernelMappingInstallError::ValidationFailed: return "installed kernel mappings failed validation";
        default: return "unknown kernel mapping installation error";
        }
    }

    const char* PageMap::Describe(PageMapSwitchError error) noexcept {
        switch (error) {
        case PageMapSwitchError::Success: return "success";
        case PageMapSwitchError::NotInitialized: return "page map is not initialized";
        case PageMapSwitchError::InvalidState: return "page map is not ready for runtime activation";
        case PageMapSwitchError::InterruptsEnabled: return "page-map switch requires maskable interrupts to be disabled";
        case PageMapSwitchError::RootTableMismatch: return "CR3 does not match the requested page-map root";
        default: return "unknown page-map switch error";
        }
    }

    const char* PageMap::Describe(MappingError error) noexcept {
        switch (error) {
        case MappingError::Success: return "success";
        case MappingError::NotInitialized: return "page map is not initialized";
        case MappingError::InvalidVirtualAddress: return "virtual address is invalid for a 4 KiB mapping";
        case MappingError::InvalidPhysicalAddress: return "physical address is invalid for a 4 KiB mapping";
        case MappingError::InvalidPermissions: return "mapping permissions violate the page-map policy";
        case MappingError::AlreadyMapped: return "virtual page is already mapped";
        case MappingError::NotMapped: return "virtual page is not mapped";
        case MappingError::PhysicalAllocationFailed: return "failed to allocate an intermediate page table";
        case MappingError::MetadataAllocationFailed: return "failed to retain intermediate page-table ownership";
        case MappingError::UnsupportedLargePage: return "encountered a large-page entry in a 4 KiB-only page map";
        case MappingError::CorruptPageTable: return "page-table structure is inconsistent";
        default: return "unknown page mapping error";
        }
    }

    const char* PageMap::Describe(PageMapActivationError error) noexcept {
        switch (error) {
        case PageMapActivationError::Success: return "success";
        case PageMapActivationError::NotInitialized: return "page map is not initialized";
        case PageMapActivationError::AlreadyActive: return "page map is already active";
        case PageMapActivationError::ExecuteDisableUnsupported: return "execute-disable paging is unsupported";
        case PageMapActivationError::UnsupportedPagingMode: return "active paging mode is unsupported";
        case PageMapActivationError::InterruptsEnabled: return "interrupts must be disabled during page-map activation";
        case PageMapActivationError::DirectMapUnavailable: return "required direct-map coverage is unavailable";
        case PageMapActivationError::ProtectionEnableFailed: return "failed to enable paging protections";
        case PageMapActivationError::RootTableMismatch: return "active CR3 does not match the page-map root";
        }
        return "unknown page-map activation error";
    }
}
