#include <Kernel/Architecture/AMD64/Paging.hpp>

extern "C" void* memset(void* dst, int v, unsigned long long n) noexcept;

namespace Zos::Kernel::Architecture::AMD64 {
    using namespace Memory;

    Uint64 PageMap::Pml4Index(VirtualAddress address) noexcept {
        return (address.Value() >> 39) & 0x1FF;
    }

    Uint64 PageMap::PdptIndex(VirtualAddress address) noexcept {
        return (address.Value() >> 30) & 0x1FF;
    }

    Uint64 PageMap::PdIndex(VirtualAddress address) noexcept {
        return (address.Value() >> 21) & 0x1FF;
    }

    Uint64 PageMap::PtIndex(VirtualAddress address) noexcept {
        return (address.Value() >> 12) & 0x1FF;
    }

    Uint64 PageMap::PageOffset(VirtualAddress address) noexcept {
        return address.Value() & (PageSize - 1);
    }

    bool PageMap::IsCanonical(VirtualAddress address) noexcept {
        const Uint64 value = address.Value();
        const Uint64 upper = value >> 48;
        const bool sign = ((value >> 47) & 1) != 0;
        return sign ? upper == 0xFFFF : upper == 0;
    }

    bool PageMap::ValidOptions(MappingOptions options) noexcept {
        if (!HasAccess(options.Access, PageAccess::Read)) return false;
        if (HasAccess(options.Access, PageAccess::Write) && HasAccess(options.Access, PageAccess::Execute)) return false;
        if (HasAccess(options.Access, PageAccess::User) && HasAccess(options.Access, PageAccess::Global)) return false;
        return options.Cache == CachePolicy::WriteBack || options.Cache == CachePolicy::Uncached;
    }

    Uint64 PageMap::LeafFlags(MappingOptions options) noexcept {
        Uint64 flags = Present;

        if (HasAccess(options.Access, PageAccess::Write)) flags |= Writable;
        if (HasAccess(options.Access, PageAccess::User)) flags |= User;
        if (HasAccess(options.Access, PageAccess::Global)) flags |= Global;
        if (!HasAccess(options.Access, PageAccess::Execute)) flags |= NoExecute;
        if (options.Cache == CachePolicy::Uncached) flags |= PageWriteThrough | PageCacheDisable;

        return flags;
    }

    MappingOptions PageMap::DecodeOptions(Entry entry) noexcept {
        MappingOptions options{};
        options.Access = PageAccess::Read;

        if ((entry & Writable) != 0) options.Access |= PageAccess::Write;
        if ((entry & NoExecute) == 0) options.Access |= PageAccess::Execute;
        if ((entry & User) != 0) options.Access |= PageAccess::User;
        if ((entry & Global) != 0) options.Access |= PageAccess::Global;

        options.Cache = ((entry & (PageWriteThrough | PageCacheDisable)) != 0)
            ? CachePolicy::Uncached : CachePolicy::WriteBack;

        return options;
    }

    PhysicalAddress PageMap::EntryAddress(Entry entry) noexcept {
        return PhysicalAddress(entry & AddressMask);
    }

    bool PageMap::TableIsEmpty(const Entry* table) noexcept {
        for (Uint64 i = 0; i < EntriesPerTable; i++) 
            if ((table[i] & Present) != 0) return false;
        return true;
    }

    PageMap::Entry* PageMap::BootstrapTablePointer(PhysicalAddress address) const noexcept {
        return reinterpret_cast<Entry*>(address.Value());
    }

    PageMap::TableRecord* PageMap::FindTableRecord(PhysicalAddress address) noexcept {
        for (TableRecord* record = m_TableRecords; record != nullptr; record = record->Next) 
            if (record->Address == address) return record;
        return nullptr;
    }

    const PageMap::TableRecord* PageMap::FindTableRecord(PhysicalAddress address) const noexcept {
        for (const TableRecord* record = m_TableRecords; record != nullptr; record = record->Next) 
            if (record->Address == address) return record;
        return nullptr;
    }

    PageMapInitializationError PageMap::AllocateTable(PhysicalAddress& output) noexcept {
        PhysicalAllocation allocation{};
        const PhysicalAllocationError allocation_error = m_PhysicalMemory->AllocatePage(allocation);
        if (allocation_error != PhysicalAllocationError::Success) return PageMapInitializationError::PhysicalAllocationFailed;

        if ((allocation.Base().Value() & ~AddressMask) != 0) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::PhysicalAddressUnsupported;
        }

        Entry* table = BootstrapTablePointer(allocation.Base());
        if (table == nullptr) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::PhysicalAddressUnsupported;
        }

        memset(table, 0, PageSize);
        const PhysicalAddress address = allocation.Base();

        PhysicalAllocation* ownership = m_Metadata->Retain(static_cast<PhysicalAllocation&&>(allocation));
        if (ownership == nullptr) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::MetadataAllocationFailed;
        }

        void* record_storage = m_Metadata->Allocate(sizeof(TableRecord), alignof(TableRecord));
        if (record_storage == nullptr) {
            (void)m_PhysicalMemory->Release(*ownership);
            return PageMapInitializationError::MetadataAllocationFailed;
        }

        auto* record = static_cast<TableRecord*>(record_storage);
        record->Address = address;
        record->Ownership = ownership;
        record->Previous = nullptr;
        record->Next = m_TableRecords;
        if (m_TableRecords != nullptr) m_TableRecords->Previous = record;
        m_TableRecords = record;

        output = address;
        m_Statistics.TablePages++;
        return PageMapInitializationError::Success;
    }    

    MappingError PageMap::AllocateTableForMapping(PhysicalAddress& output) noexcept {
        const PageMapInitializationError error = AllocateTable(output);
        switch (error) {
        case PageMapInitializationError::Success: return MappingError::Success;
        case PageMapInitializationError::PhysicalAllocationFailed: return MappingError::PhysicalAllocationFailed;
        case PageMapInitializationError::MetadataAllocationFailed: return MappingError::MetadataAllocationFailed;
        default: return MappingError::CorruptPageTable;
        }
    }

    bool PageMap::ReleaseTable(PhysicalAddress address) noexcept {
        if (address == m_RootTable) return false;

        TableRecord* record = FindTableRecord(address);
        if (record == nullptr || record->Ownership == nullptr || !record->Ownership->IsValid()) return false;
        if (m_PhysicalMemory->Release(*record->Ownership) != PhysicalAllocationError::Success) return false;

        if (record->Previous != nullptr) record->Previous->Next = record->Next;
        else m_TableRecords = record->Next;

        if (record->Next != nullptr) record->Next->Previous = record->Previous;

        record->Address = {};
        record->Ownership = nullptr;
        record->Previous = nullptr;
        record->Next = nullptr;

        if (m_Statistics.TablePages != 0) m_Statistics.TablePages--;

        return true;
    }

    PageMapInitializationError PageMap::Initialize(PhysicalMemoryManager& physical_memory, BootstrapMetadataArena& metadata) noexcept {
        if (IsInitialized()) return PageMapInitializationError::AlreadyInitialized;
        if (!physical_memory.IsInitialized() || !metadata.IsInitialized()) 
            return PageMapInitializationError::InvalidDependency;

        m_PhysicalMemory = &physical_memory;
        m_Metadata = &metadata;

        PhysicalAddress root{};
        const PageMapInitializationError error = AllocateTable(root);
        if (error != PageMapInitializationError::Success) {
            m_PhysicalMemory = nullptr;
            m_Metadata = nullptr;
            return error;
        }

        m_RootTable = root;
        return PageMapInitializationError::Success;
    }

    MappingError PageMap::ResolveNextTable(Entry* table, Uint64 index, bool user_mapping, TableResolution& output) noexcept {
        output = {};
        Entry& entry = table[index];
        output.ParentEntry = &entry;
        output.OriginalEntry = entry;

        if ((entry & Present) != 0) {
            if ((entry & LargePage) != 0) return MappingError::UnsupportedLargePage;

            const PhysicalAddress address = EntryAddress(entry);
            if (address.IsNull() || !address.IsPageAligned() || FindTableRecord(address) == nullptr) 
                return MappingError::CorruptPageTable;

            if (user_mapping && (entry & User) == 0) {
                entry |= User;
                output.Modified = true;
            }

            // Intermediate writable permission may be broader than the leaf;
            // the leaf still provides the effective per-page write restriction.
            if ((entry & Writable) == 0) {
                entry |= Writable;
                output.Modified = true;
            }

            output.Address = address;
            return MappingError::Success;
        }

        PhysicalAddress new_table{};
        const MappingError allocation_error = AllocateTableForMapping(new_table);
        if (allocation_error != MappingError::Success) return allocation_error;

        Uint64 flags = Present | Writable;
        if (user_mapping) flags |= User;

        entry = (new_table.Value() & AddressMask) | flags;
        output.Address = new_table;
        output.Created = true;
        output.Modified = true;
        return MappingError::Success;
    }

    const PageMap::Entry* PageMap::ResolveExistingNextTable(const Entry* table, Uint64 index, MappingError& error) const noexcept {
        const Entry entry = table[index];
        if ((entry & Present) == 0) {
            error = MappingError::NotMapped;
            return nullptr;
        }

        if ((entry & LargePage) != 0) {
            error = MappingError::UnsupportedLargePage;
            return nullptr;
        }

        const PhysicalAddress address = EntryAddress(entry);
        if (address.IsNull() || !address.IsPageAligned() || FindTableRecord(address) == nullptr) {
            error = MappingError::CorruptPageTable;
            return nullptr;
        }

        const Entry* next = BootstrapTablePointer(address);
        if (next == nullptr) {
            error = MappingError::CorruptPageTable;
            return nullptr;
        }

        error = MappingError::Success;
        return next;
    }

    bool PageMap::RollbackResolution(TableResolution& resolution) noexcept {
        if (!resolution.Created && !resolution.Modified) return true;
        if (resolution.ParentEntry == nullptr) return false;
        if (resolution.Created) {
            if (resolution.Address.IsNull()) return false;
            *resolution.ParentEntry = resolution.OriginalEntry;
            const bool released = ReleaseTable(resolution.Address);
            resolution = {};
            return released;
        }
        *resolution.ParentEntry = resolution.OriginalEntry;
        resolution = {};
        return true;
    }

    MappingError PageMap::MapPage(VirtualAddress virt_addr, PhysicalAddress phys_addr, MappingOptions options) noexcept {
        if (!IsInitialized()) return MappingError::NotInitialized;
        if (!IsCanonical(virt_addr) || !virt_addr.IsPageAligned() || virt_addr.Value() < MinimumMappableVirtualAddress) 
            return MappingError::InvalidVirtualAddress;
        if (!phys_addr.IsPageAligned() || (phys_addr.Value() & ~AddressMask) != 0)
            return MappingError::InvalidPhysicalAddress;
        if (!ValidOptions(options)) return MappingError::InvalidPermissions;

        Entry* pml4 = BootstrapTablePointer(m_RootTable);
        if (pml4 == nullptr) return MappingError::CorruptPageTable;

        const bool user_mapping = HasAccess(options.Access, PageAccess::User);
        TableResolution pdpt_resolution{};
        TableResolution pd_resolution{};
        TableResolution pt_resolution{};

        MappingError error = ResolveNextTable(pml4, Pml4Index(virt_addr), user_mapping, pdpt_resolution);
        if (error != MappingError::Success) return error;

        Entry* pdpt = BootstrapTablePointer(pdpt_resolution.Address);
        error = ResolveNextTable(pdpt, PdptIndex(virt_addr), user_mapping, pd_resolution);
        if (error != MappingError::Success) {
            if (!RollbackResolution(pdpt_resolution)) 
                return MappingError::CorruptPageTable;
            return error;
        }

        Entry* pd = BootstrapTablePointer(pd_resolution.Address);
        error = ResolveNextTable(pd, PdIndex(virt_addr), user_mapping, pt_resolution);
        if (error != MappingError::Success) {
            if (!RollbackResolution(pd_resolution) || !RollbackResolution(pdpt_resolution)) 
                return MappingError::CorruptPageTable;
            return error;
        }

        Entry* pt = BootstrapTablePointer(pt_resolution.Address);
        Entry& leaf = pt[PtIndex(virt_addr)];
        if ((leaf & Present) != 0) {
            // Existing paths should not have created new intermediate tables for
            // an already-present leaf, but retain transactional rollback anyway.
            if (!RollbackResolution(pt_resolution) || !RollbackResolution(pd_resolution) || !RollbackResolution(pdpt_resolution))
                return MappingError::CorruptPageTable;
            return MappingError::AlreadyMapped;
        }

        leaf = (phys_addr.Value() & AddressMask) | LeafFlags(options);
        m_Statistics.MappedPages++;
        return MappingError::Success;
    }

    MappingError PageMap::UnmapPage(VirtualAddress virt_addr) noexcept {
        if (!IsInitialized()) return MappingError::NotInitialized;
        if (!IsCanonical(virt_addr) || !virt_addr.IsPageAligned()) return MappingError::InvalidVirtualAddress; 

        Entry* pml4 = BootstrapTablePointer(m_RootTable);
        Entry& pml4_entry = pml4[Pml4Index(virt_addr)];
        if ((pml4_entry & Present) == 0) return MappingError::NotMapped;
        if ((pml4_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;

        const PhysicalAddress pdpt_address = EntryAddress(pml4_entry);
        Entry* pdpt = BootstrapTablePointer(pdpt_address);
        Entry& pdpt_entry = pdpt[PdptIndex(virt_addr)];
        if ((pdpt_entry & Present) == 0) return MappingError::NotMapped;
        if ((pdpt_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;

        const PhysicalAddress pd_address = EntryAddress(pdpt_entry);
        Entry* pd = BootstrapTablePointer(pd_address);
        Entry& pd_entry = pd[PdIndex(virt_addr)];
        if ((pd_entry & Present) == 0) return MappingError::NotMapped;
        if ((pd_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;

        const PhysicalAddress pt_address = EntryAddress(pd_entry);
        Entry* pt = BootstrapTablePointer(pt_address);
        Entry& leaf = pt[PtIndex(virt_addr)];
        if ((leaf & Present) == 0) return MappingError::NotMapped;

        leaf = 0;
        if (m_Statistics.MappedPages != 0) m_Statistics.MappedPages--;
        if (TableIsEmpty(pt)) {
            pd_entry = 0;
            if (!ReleaseTable(pt_address)) return MappingError::CorruptPageTable;
            if (TableIsEmpty(pd)) {
                pdpt_entry = 0;
                if (!ReleaseTable(pd_address)) return MappingError::CorruptPageTable;
                if (TableIsEmpty(pdpt)) {
                    pml4_entry = 0;
                    if (!ReleaseTable(pdpt_address)) return MappingError::CorruptPageTable;
                }
            }
        }

        return MappingError::Success;
    }

    TranslationResult PageMap::Translate(VirtualAddress virt_addr) const noexcept {
        TranslationResult result{};
        if (!IsInitialized() || !IsCanonical(virt_addr)) return result;

        MappingError error = MappingError::Success;
        const Entry* pml4 = BootstrapTablePointer(m_RootTable);
        const Entry* pdpt = ResolveExistingNextTable(pml4, Pml4Index(virt_addr), error);
        if (pdpt == nullptr) return result;

        const Entry* pd = ResolveExistingNextTable(pdpt, PdptIndex(virt_addr), error);
        if (pd == nullptr) return result;

        const Entry* pt = ResolveExistingNextTable(pd, PdIndex(virt_addr), error);
        if (pt == nullptr) return result;

        const Entry leaf = pt[PtIndex(virt_addr)];
        if ((leaf & Present) == 0 || (leaf & LargePage) != 0) return result;

        const Uint64 physical_base = EntryAddress(leaf).Value();
        const Uint64 offset = PageOffset(virt_addr);
        result.Mapped = true;
        result.Physical = PhysicalAddress(physical_base + offset);
        result.Options = DecodeOptions(leaf);
        return result;
    }

    bool PageMap::IsMapped(VirtualAddress virt_addr) const noexcept {
        return Translate(virt_addr).Mapped;
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
}