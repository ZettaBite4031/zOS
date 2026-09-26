#include <Kernel/Architecture/AMD64/Paging.hpp>

#include <Kernel/Runtime/New.hpp>

extern "C" void* memset(void* dst, int v, unsigned long long n) noexcept;

namespace Zos::Kernel::Architecture::AMD64 {
    using namespace Memory;

    inline constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };

    inline constexpr Uint32 ExtendedCpuidBase { 0x80000000u };
    inline constexpr Uint32 ExtendedFeaturesLeaf { 0x80000001u };
    inline constexpr Uint32 NxFeatureBit{ 1u << 20 };
    inline constexpr Uint32 EferMsr{ 0xC0000080u };
    inline constexpr Uint64 EferNxe{ 1ULL << 11 };
    inline constexpr Uint64 Cr0WriteProtect{ 1ULL << 16 };
    inline constexpr Uint64 Cr4PageGlobalEnable{ 1ULL << 7 };
    inline constexpr Uint64 Cr4FiveLevelPaging{ 1ULL << 12 };
    inline constexpr Uint64 RflagsInterruptEnable{ 1ULL << 9 };

    void Cpuid(Uint32 leaf, Uint32 subleaf, Uint32& eax, Uint32& ebx, Uint32& ecx, Uint32& edx) noexcept {
        __asm__ volatile(
            "cpuid"
            : "=a"(eax),
              "=b"(ebx),
              "=c"(ecx),
              "=d"(edx)
            : "a"(leaf),
              "c"(subleaf)
        );
    }

    bool SupportsExecuteDisable() noexcept { 
        Uint32 eax{};
        Uint32 ebx{};
        Uint32 ecx{};
        Uint32 edx{};

        Cpuid(ExtendedCpuidBase, 0, eax, ebx, ecx, edx);
        if (eax < ExtendedFeaturesLeaf) return false;

        Cpuid(ExtendedFeaturesLeaf, 0, eax, ebx, ecx, edx);

        return (edx & NxFeatureBit) != 0;
    }

    Uint64 ReadMsr(Uint32 msr) noexcept {
        Uint32 low{};
        Uint32 high{};

        __asm__ volatile(
            "rdmsr"
            : "=a"(low), "=d"(high)
            : "c"(msr)
        );

        return static_cast<Uint64>(low) | (static_cast<Uint64>(high) << 32);
    }

    void WriteMsr(Uint32 msr, Uint64 value) noexcept {
        __asm__ volatile(
            "wrmsr"
            :
            : "c"(msr),
              "a"(static_cast<Uint32>(value)),
              "d"(static_cast<Uint32>(value >> 32))
            : "memory"
        );
    }

    Uint64 ReadCr0() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "mov %%cr0, %0"
            : "=r"(value)
        ); 
        return value;
    }

    void WriteCr0(Uint64 value) noexcept {
        __asm__ volatile(
            "mov %0, %%cr0"
            :
            : "r"(value)
            : "memory"
        );
    }

    Uint64 ReadCr3() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "mov %%cr3, %0"
            : "=r"(value)
        );
        return value;
    }

    void WriteCr3(Uint64 value) noexcept {
        __asm__ volatile(
            "mov %0, %%cr3"
            :
            : "r"(value)
            : "memory"
        );
    }

    Uint64 ReadCr4() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "mov %%cr4, %0"
            : "=r"(value)
        );
        return value;
    }

    void WriteCr4(Uint64 value) noexcept {
        __asm__ volatile(
            "mov %0, %%cr4"
            :
            : "r"(value)
            : "memory"
        );
    }

    Uint64 ReadRflags() noexcept {
        Uint64 value{};
        __asm__ volatile(
            "pushfq\n\t"
            "popq %0"
            : "=r"(value)
        );
        return value;
    }

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

    PhysicalAddress PageMap::TablePage(Uint64 index) const noexcept {
        const TableRecord* record = m_TableRecords;

        for (Uint64 current = 0; record != nullptr && current < index; current++) 
            record = record->Next;

        return record != nullptr ? record->Address : PhysicalAddress{};
    }

    PhysicalAddress PageMap::CurrentRootTable() noexcept {
        return PhysicalAddress(ReadCr3() & AddressMask);
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

    void PageMap::InvalidatePage(VirtualAddress address) noexcept { 
        const Uint64 value = address.Value();

        __asm__ volatile(
            "invlpg (%0)"
            :
            : "r"(value)
            : "memory"
        );
    }

    PageMap::Entry* PageMap::TablePointer(PhysicalAddress address) const noexcept {
        if (address.IsNull() || !address.IsPageAligned()) return nullptr;
        if (!m_Active) return reinterpret_cast<Entry*>(address.Value());
        if (!Layout::IsDirectMappable(address)) return nullptr;
        return reinterpret_cast<Entry*>(Layout::DirectMapAddress(address).Value());
    }


    bool PageMap::MoveOwnership(PhysicalAllocation& destination, PhysicalAllocation& source) noexcept {
        if (destination.IsValid() || !source.IsValid()) return false;
        destination.~PhysicalAllocation();
        new (&destination) PhysicalAllocation(static_cast<PhysicalAllocation&&>(source));
        return destination.IsValid() && !source.IsValid();
    }

    PageMap::TableRecord* PageMap::MetadataRecordAt(MetadataPage& page, Uint64 index) noexcept {
        if (index >= MetadataRecordsPerPage) return nullptr;
        auto* storage = reinterpret_cast<Uint8*>(&page) + MetadataRecordsOffset;
        return reinterpret_cast<TableRecord*>(storage) + index;
    }

    const PageMap::TableRecord* PageMap::MetadataRecordAt(const MetadataPage& page, Uint64 index) noexcept {
        if (index >= MetadataRecordsPerPage) return nullptr;
        const auto* storage = reinterpret_cast<const Uint8*>(&page) + MetadataRecordsOffset;
        return reinterpret_cast<const TableRecord*>(storage) + index;
    }

    bool PageMap::AllocateMetadataPage(MetadataPool& pool) noexcept {
        if (!m_Active || m_PhysicalMemory == nullptr || !m_PhysicalMemory->IsInitialized() || !m_PhysicalMemory->IsMetadataAccessPromoted()) return false;

        PhysicalAllocation allocation{};
        if (m_PhysicalMemory->AllocatePage(allocation) != PhysicalAllocationError::Success) return false;

        if (!Layout::IsDirectMappable(allocation.Base())) {
            (void)m_PhysicalMemory->Release(allocation);
            return false;
        }

        const VirtualAddress direct = Layout::DirectMapAddress(allocation.Base());
        if (direct.IsNull()) {
            (void)m_PhysicalMemory->Release(allocation);
            return false;
        }

        auto* page = reinterpret_cast<MetadataPage*>(direct.Value());
        memset(static_cast<void*>(page), 0, PageSize);
        new (page) MetadataPage{};

        if (!MoveOwnership(page->Ownership, allocation)) {
            if (allocation.IsValid()) (void)m_PhysicalMemory->Release(allocation);
            return false;
        }

        if (pool.Tail != nullptr) pool.Tail->Next = page;
        else pool.Head = page;
        pool.Tail = page;
        return true;
    }

    PageMap::TableRecord* PageMap::AllocatePermanentTableRecord(MetadataPool& pool) noexcept {
        MetadataPage* page = pool.Tail;
        if (page == nullptr || page->UsedRecords >= MetadataRecordsPerPage) {
            if (!AllocateMetadataPage(pool)) return nullptr;
            page = pool.Tail;
        }

        if (page == nullptr || page->UsedRecords >= MetadataRecordsPerPage) return nullptr;
        TableRecord* record = MetadataRecordAt(*page, page->UsedRecords);
        if (record == nullptr) return nullptr;

        new (record) TableRecord{};
        page->UsedRecords++;
        return record;
    }

    bool PageMap::DestroyMetadataPool(MetadataPool& pool) noexcept {
        for (MetadataPage* page = pool.Head; page != nullptr; page = page->Next) {
            if (page->UsedRecords > MetadataRecordsPerPage) return false;
            for (Uint64 i = 0; i < page->UsedRecords; i++) {
                TableRecord* record = MetadataRecordAt(*page, i);
                if (record == nullptr || record->Ownership.IsValid()) return false;
            }
        }

        MetadataPage* page = pool.Head;
        while (page != nullptr) {
            MetadataPage* next = page->Next;
            if (!page->Ownership.IsValid() || page->Ownership.PageCount() != 1) return false;

            PhysicalAllocation ownership(static_cast<PhysicalAllocation&&>(page->Ownership));
            if (m_PhysicalMemory->Release(ownership) != PhysicalAllocationError::Success) return false;
            page = next;
        }

        pool = {};
        return true;
    }

    bool PageMap::PermanentMetadataContains(const TableRecord* record) const noexcept {
        if (record == nullptr) return false;
        for (const MetadataPage* page = m_PermanentMetadata.Head; page != nullptr; page = page->Next) {
            if (page->UsedRecords > MetadataRecordsPerPage) return false;
            for (Uint64 i = 0; i < page->UsedRecords; i++)
                if (MetadataRecordAt(*page, i) == record) return true;
        }
        return false;
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

        Entry* table = TablePointer(allocation.Base());
        if (table == nullptr) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::PhysicalAddressUnsupported;
        }

        memset(table, 0, PageSize);
        TableRecord* record = AcquireTableRecord();
        if (record == nullptr) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::MetadataAllocationFailed;
        }

        if (record->Ownership.IsValid()) {
            (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::MetadataAllocationFailed;
        }

        const PhysicalAddress address = allocation.Base();
        if (!MoveOwnership(record->Ownership, allocation)) {
            RecycleTableRecord(*record);
            if (allocation.IsValid()) (void)m_PhysicalMemory->Release(allocation);
            return PageMapInitializationError::MetadataAllocationFailed;
        }

        record->Address = address;
        record->Previous = nullptr;
        record->Next = m_TableRecords;
        record->NextFree = nullptr;
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

    PageMap::TableRecord* PageMap::AcquireTableRecord() noexcept {
        if (m_RecycledTableRecords != nullptr) {
            TableRecord* record = m_RecycledTableRecords;
            if (record->Ownership.IsValid()) return nullptr;

            m_RecycledTableRecords = record->NextFree;
            record->Address = {};
            record->Previous = nullptr;
            record->Next = nullptr;
            record->NextFree = nullptr;
            return record;
        }

        if (m_MetadataPromoted) return AllocatePermanentTableRecord(m_PermanentMetadata);
        if (m_BootstrapMetadata == nullptr || !m_BootstrapMetadata->IsInitialized()) return nullptr;

        void* storage = m_BootstrapMetadata->Allocate(sizeof(TableRecord), alignof(TableRecord));
        if (storage == nullptr) return nullptr;
        return new (storage) TableRecord{};
    }

    bool PageMap::ReleaseTable(PhysicalAddress address) noexcept {
        if (address == m_RootTable) return false;

        TableRecord* record = FindTableRecord(address);
        if (record == nullptr || record->Address != address || !record->Ownership.IsValid() || record->Ownership.Base() != address || record->Ownership.PageCount() != 1)
            return false;

        if (m_PhysicalMemory->Release(record->Ownership) != PhysicalAllocationError::Success) return false;

        if (record->Previous != nullptr) record->Previous->Next = record->Next;
        else m_TableRecords = record->Next;
        if (record->Next != nullptr) record->Next->Previous = record->Previous;

        RecycleTableRecord(*record);
        if (m_Statistics.TablePages != 0) m_Statistics.TablePages--;
        return true;
    }

    PageMapInitializationError PageMap::Initialize(PhysicalMemoryManager& physical_memory, BootstrapMetadataArena& metadata) noexcept {
        if (IsInitialized()) return PageMapInitializationError::AlreadyInitialized;
        if (!physical_memory.IsInitialized() || !metadata.IsInitialized()) return PageMapInitializationError::InvalidDependency;

        m_PhysicalMemory = &physical_memory;
        m_BootstrapMetadata = &metadata;
        m_MetadataPromoted = false;
        m_PermanentMetadata = {};

        PhysicalAddress root{};
        const PageMapInitializationError error = AllocateTable(root);
        if (error != PageMapInitializationError::Success) {
            m_PhysicalMemory = nullptr;
            m_BootstrapMetadata = nullptr;
            return error;
        }

        m_RootTable = root;
        return PageMapInitializationError::Success;
    }

    PageMapActivationError PageMap::Activate() noexcept {
        if (!IsInitialized()) return PageMapActivationError::NotInitialized;
        if (m_Active) return PageMapActivationError::AlreadyActive;
        if (!SupportsExecuteDisable()) return PageMapActivationError::ExecuteDisableUnsupported;

        /*
         * Once CR3 changes, PageMap accesses its own
         * tables through the direct physical map.
         * 
         * Verify every owned table has that alias
         * before making the transition
         */
        for (Uint64 i = 0; i < TablePageCount(); i++) {
            const PhysicalAddress physical = TablePage(i);
            if (!Layout::IsDirectMappable(physical)) return PageMapActivationError::DirectMapUnavailable;

            const VirtualAddress direct = Layout::DirectMapAddress(physical);
            const TranslationResult translation = Translate(direct);
            if (!translation.Mapped || translation.Physical != physical)
                return PageMapActivationError::DirectMapUnavailable;
        }

        /*
         * The metadata arena also changes its
         * physical-pointer conversion after CR3.
         */
        for (Uint64 i = 0; i < m_BootstrapMetadata->BackingPageCount(); i++) {
            const PhysicalAddress physical = m_BootstrapMetadata->BackingPage(i);
            if (!Layout::IsDirectMappable(physical)) return PageMapActivationError::DirectMapUnavailable;

            const TranslationResult translation = Translate(Layout::DirectMapAddress(physical));
            if (!translation.Mapped || translation.Physical != physical)
                return PageMapActivationError::DirectMapUnavailable;
        }

        Uint64 efer = ReadMsr(EferMsr);
        efer |= EferNxe;
        WriteMsr(EferMsr, efer);

        if ((ReadMsr(EferMsr) & EferNxe) == 0) 
            return PageMapActivationError::ProtectionEnableFailed;

        Uint64 cr0 = ReadCr0();
        cr0 |= Cr0WriteProtect;
        WriteCr0(cr0);

        if ((ReadCr0() & Cr0WriteProtect) == 0) 
            return PageMapActivationError::ProtectionEnableFailed;

        if ((ReadRflags() & RflagsInterruptEnable) != 0) 
            return PageMapActivationError::InterruptsEnabled;

        /*
         * A CR3 reload does not necessarily evict
         * global translations when PGE is enabled.
         * 
         * Temporarily clearing PGE ensures firmware
         * global translations cannot survive into
         * the new address space.
         */
        const Uint64 original_cr4 = ReadCr4();
        const bool pge_enabled = (original_cr4 & Cr4PageGlobalEnable) != 0;
        if ((original_cr4 & Cr4FiveLevelPaging) != 0) return PageMapActivationError::UnsupportedPagingMode;
        if (pge_enabled) WriteCr4(original_cr4 & ~Cr4PageGlobalEnable);
        WriteCr3(m_RootTable.Value());
        if (pge_enabled) WriteCr4(original_cr4);

        /*
         * Everything after this point executes using 
         * zOS-owned tables.
         */
        m_Active = true;

        m_BootstrapMetadata->EnableDirectMapAccess();

        if (CurrentRootTable() != m_RootTable)
            return PageMapActivationError::RootTableMismatch;
        return PageMapActivationError::Success;
    }

    PageMapMetadataPromotionError PageMap::PromoteMetadata() noexcept {
        if (!IsInitialized()) return PageMapMetadataPromotionError::NotInitialized;
        if (m_MetadataPromoted) return PageMapMetadataPromotionError::AlreadyPromoted;
        if (!m_Active || m_PhysicalMemory == nullptr || !m_PhysicalMemory->IsInitialized() || !m_PhysicalMemory->IsMetadataAccessPromoted() ||
            m_BootstrapMetadata == nullptr || !m_BootstrapMetadata->IsInitialized())
            return PageMapMetadataPromotionError::InvalidDependency;
        if (!Validate()) return PageMapMetadataPromotionError::CorruptState;

        MetadataPool candidate_pool{};
        TableRecord* candidate_head = nullptr;
        TableRecord* candidate_tail = nullptr;

        for (const TableRecord* old = m_TableRecords; old != nullptr; old = old->Next) {
            TableRecord* candidate = AllocatePermanentTableRecord(candidate_pool);
            if (candidate == nullptr) {
                if (!DestroyMetadataPool(candidate_pool)) return PageMapMetadataPromotionError::RollbackFailed;
                return PageMapMetadataPromotionError::AllocationFailed;
            }

            candidate->Address = old->Address;
            candidate->Previous = candidate_tail;
            candidate->Next = nullptr;
            candidate->NextFree = nullptr;
            if (candidate_tail != nullptr) candidate_tail->Next = candidate;
            else candidate_head = candidate;
            candidate_tail = candidate;
        }

        const TableRecord* old_probe = m_TableRecords;
        const TableRecord* candidate_probe = candidate_head;
        while (old_probe != nullptr && candidate_probe != nullptr) {
            if (!old_probe->Ownership.IsValid() || old_probe->Ownership.Base() != old_probe->Address || old_probe->Ownership.PageCount() != 1 || candidate_probe->Ownership.IsValid()) {
                if (!DestroyMetadataPool(candidate_pool)) return PageMapMetadataPromotionError::RollbackFailed;
                return PageMapMetadataPromotionError::CorruptState;
            }
            old_probe = old_probe->Next;
            candidate_probe = candidate_probe->Next;
        }

        if (old_probe != nullptr || candidate_probe != nullptr) {
            if (!DestroyMetadataPool(candidate_pool)) return PageMapMetadataPromotionError::RollbackFailed;
            return PageMapMetadataPromotionError::CorruptState;
        }

        TableRecord* old = m_TableRecords;
        TableRecord* candidate = candidate_head;
        while (old != nullptr) {
            if (!MoveOwnership(candidate->Ownership, old->Ownership)) return PageMapMetadataPromotionError::CorruptState;
            old = old->Next;
            candidate = candidate->Next;
        }

        m_TableRecords = candidate_head;
        m_RecycledTableRecords = nullptr;
        m_PermanentMetadata = candidate_pool;
        candidate_pool = {};
        m_BootstrapMetadata = nullptr;
        m_MetadataPromoted = true;

        if (!Validate()) return PageMapMetadataPromotionError::ValidationFailed;
        return PageMapMetadataPromotionError::Success;
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

        const Entry* next = TablePointer(address);
        if (next == nullptr) {
            error = MappingError::CorruptPageTable;
            return nullptr;
        }

        error = MappingError::Success;
        return next;
    }

    PageMap::Entry* PageMap::ResolveExistingNextTable(Entry* table, Uint64 index, MappingError& error) noexcept {
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

        Entry* next = TablePointer(address);
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

    void PageMap::RecycleTableRecord(TableRecord& record) noexcept {
        record.Address = {};
        record.Previous = nullptr;
        record.Next = nullptr;
        record.NextFree = m_RecycledTableRecords;
        m_RecycledTableRecords = &record;
    }

    MappingError PageMap::MapPage(VirtualAddress virt_addr, PhysicalAddress phys_addr, MappingOptions options) noexcept {
        if (!IsInitialized()) return MappingError::NotInitialized;
        if (!IsCanonical(virt_addr) || !virt_addr.IsPageAligned() || virt_addr.Value() < MinimumMappableVirtualAddress) 
            return MappingError::InvalidVirtualAddress;
        if (!phys_addr.IsPageAligned() || (phys_addr.Value() & ~AddressMask) != 0)
            return MappingError::InvalidPhysicalAddress;
        if (!ValidOptions(options)) return MappingError::InvalidPermissions;

        Entry* pml4 = TablePointer(m_RootTable);
        if (pml4 == nullptr) return MappingError::CorruptPageTable;

        const bool user_mapping = HasAccess(options.Access, PageAccess::User);
        TableResolution pdpt_resolution{};
        TableResolution pd_resolution{};
        TableResolution pt_resolution{};

        MappingError error = ResolveNextTable(pml4, Pml4Index(virt_addr), user_mapping, pdpt_resolution);
        if (error != MappingError::Success) return error;

        Entry* pdpt = TablePointer(pdpt_resolution.Address);
        error = ResolveNextTable(pdpt, PdptIndex(virt_addr), user_mapping, pd_resolution);
        if (error != MappingError::Success) {
            if (!RollbackResolution(pdpt_resolution)) 
                return MappingError::CorruptPageTable;
            return error;
        }

        Entry* pd = TablePointer(pd_resolution.Address);
        error = ResolveNextTable(pd, PdIndex(virt_addr), user_mapping, pt_resolution);
        if (error != MappingError::Success) {
            if (!RollbackResolution(pd_resolution) || !RollbackResolution(pdpt_resolution)) 
                return MappingError::CorruptPageTable;
            return error;
        }

        Entry* pt = TablePointer(pt_resolution.Address);
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
        if (m_Active) InvalidatePage(virt_addr);
        return MappingError::Success;
    }

    MappingError PageMap::MapRange(VirtualAddress virt_addr, PhysicalAddress phys_addr, Uint64 page_count, MappingOptions options) noexcept {
        if (page_count == 0 || page_count > MaximumValue / PageSize) return MappingError::InvalidVirtualAddress;

        const Uint64 size = page_count * PageSize;
        if (virt_addr.Value() > MaximumValue - (size - PageSize) || phys_addr.Value() > MaximumValue - (size - PageSize)) 
            return MappingError::InvalidVirtualAddress;

        Uint64 mapped_pages = 0;
        for (; mapped_pages < page_count; mapped_pages++) {
            const Uint64 offset = mapped_pages * PageSize;
            const MappingError error = MapPage(virt_addr + offset, phys_addr + offset, options);
            if (error == MappingError::Success) continue;
            while (mapped_pages != 0) { 
                mapped_pages--;
                const MappingError rollback = UnmapPage(virt_addr + mapped_pages * PageSize);
                if (rollback != MappingError::Success) return MappingError::CorruptPageTable;
            }
            return error;
        }
        return MappingError::Success;
    }

    MappingError PageMap::UnmapPage(VirtualAddress virt_addr) noexcept {
        if (!IsInitialized()) return MappingError::NotInitialized;
        if (!IsCanonical(virt_addr) || !virt_addr.IsPageAligned()) return MappingError::InvalidVirtualAddress; 

        Entry* pml4 = TablePointer(m_RootTable);
        if (pml4 == nullptr) return MappingError::CorruptPageTable;
        
        Entry& pml4_entry = pml4[Pml4Index(virt_addr)];
        if ((pml4_entry & Present) == 0) return MappingError::NotMapped;
        if ((pml4_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;
        
        const PhysicalAddress pdpt_address = EntryAddress(pml4_entry);
        MappingError error = MappingError::Success;
        Entry* pdpt = ResolveExistingNextTable(pml4, Pml4Index(virt_addr), error);
        if (pdpt == nullptr) return error;

        Entry& pdpt_entry = pdpt[PdptIndex(virt_addr)];
        if ((pdpt_entry & Present) == 0) return MappingError::NotMapped;
        if ((pdpt_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;

        const PhysicalAddress pd_address = EntryAddress(pdpt_entry);
        Entry* pd = ResolveExistingNextTable(pdpt, PdptIndex(virt_addr), error);
        if (pd == nullptr) return error;

        Entry& pd_entry = pd[PdIndex(virt_addr)];
        if ((pd_entry & Present) == 0) return MappingError::NotMapped;
        if ((pd_entry & LargePage) != 0) return MappingError::UnsupportedLargePage;
        
        const PhysicalAddress pt_address = EntryAddress(pd_entry);
        Entry* pt = ResolveExistingNextTable(pd, PdIndex(virt_addr), error);
        if (pt == nullptr) return error;

        Entry& leaf = pt[PtIndex(virt_addr)];
        if ((leaf & Present) == 0) return MappingError::NotMapped;

        leaf = 0;
        if (m_Active) InvalidatePage(virt_addr);
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
        const Entry* pml4 = TablePointer(m_RootTable);
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

    bool PageMap::Validate() const noexcept {
        if (!IsInitialized() || m_PhysicalMemory == nullptr || !m_PhysicalMemory->IsInitialized() || m_RootTable.IsNull() || !m_RootTable.IsPageAligned() ||
            (m_RootTable.Value() & ~AddressMask) != 0)
            return false;

        if (m_Active && CurrentRootTable() != m_RootTable) return false;

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