#pragma once

#include <Kernel/Memory/VirtualMemory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    enum class PageMapInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        PhysicalAllocationFailed,
        VirtualAllocationFailed,
        MetadataAllocationFailed,
        PhysicalAddressUnsupported,
    };

    enum class PageMapActivationError : Memory::Uint32 {
        Success,
        NotInitialized,
        AlreadyActive,
        InterruptsEnabled,
        ExecuteDisableUnsupported,
        UnsupportedPagingMode,
        DirectMapUnavailable,
        ProtectionEnableFailed,
        RootTableMismatch,
    };

    enum class MappingError : Memory::Uint32 {
        Success,
        NotInitialized,
        InvalidVirtualAddress,
        InvalidPhysicalAddress,
        InvalidPermissions,
        AlreadyMapped,
        NotMapped,
        PhysicalAllocationFailed,
        VirtualAllocationFailed,
        MetadataAllocationFailed,
        UnsupportedLargePage,
        CorruptPageTable,
    };

    struct TranslationResult final {
        bool Mapped{};
        Memory::PhysicalAddress Physical{};
        Memory::MappingOptions Options{};
    };

    struct PageMapStatistics final {
        Memory::Uint64 TablePages{};
        Memory::Uint64 MappedPages{};
    };

    class PageMap final {
    public:
        PageMap() noexcept = default;
        PageMap(const PageMap&) = delete;
        PageMap& operator=(const PageMap&) = delete;

        [[nodiscard]] PageMapInitializationError Initialize(Memory::PhysicalMemoryManager& physical_memory, Memory::BootstrapMetadataArena& metadata) noexcept;

        [[nodiscard]] PageMapActivationError Activate() noexcept;

        [[nodiscard]] MappingError MapPage(Memory::VirtualAddress virt_addr, Memory::PhysicalAddress phys_addr, Memory::MappingOptions options) noexcept;
        [[nodiscard]] MappingError MapRange(Memory::VirtualAddress virt_addr, Memory::PhysicalAddress phys_addr, Memory::Uint64 page_count, Memory::MappingOptions) noexcept;

        [[nodiscard]] MappingError UnmapPage(Memory::VirtualAddress virt_addr) noexcept;

        [[nodiscard]] TranslationResult Translate(Memory::VirtualAddress virt_addr) const noexcept;
        [[nodiscard]] bool IsMapped(Memory::VirtualAddress virt_addr) const noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return !m_RootTable.IsNull(); }
        [[nodiscard]] bool IsActive() const noexcept { return m_Active; }
        [[nodiscard]] Memory::PhysicalAddress RootTable() const noexcept { return m_RootTable; }
        [[nodiscard]] const PageMapStatistics& Statistics() const noexcept { return m_Statistics; }
        [[nodiscard]] Memory::Uint64 TablePageCount() const noexcept { return m_Statistics.TablePages; }

        [[nodiscard]] Memory::PhysicalAddress TablePage(Memory::Uint64 index) const noexcept;

        [[nodiscard]] static Memory::PhysicalAddress CurrentRootTable() noexcept;

        [[nodiscard]] static bool IsCanonical(Memory::VirtualAddress address) noexcept;

        [[nodiscard]] static const char* Describe(PageMapInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(PageMapActivationError error) noexcept;
        [[nodiscard]] static const char* Describe(MappingError error) noexcept;

    private:
        using Entry = Memory::Uint64;

        inline static constexpr Memory::Uint64 EntriesPerTable{ 512 };
        inline static constexpr Memory::Uint64 AddressMask{ 0x000FFFFFFFFFF000ULL };
        inline static constexpr Memory::Uint64 MinimumMappableVirtualAddress{ 64 * 1024 };

        static_assert(sizeof(Entry) * EntriesPerTable == Memory::PageSize);

        enum EntryFlag : Memory::Uint64 {
            Present = 1ULL << 0,
            Writable = 1ULL << 1,
            User = 1ULL << 2, 
            PageWriteThrough = 1ULL << 3,
            PageCacheDisable = 1ULL << 4,
            Accessed = 1ULL << 5,
            Dirty = 1ULL << 6,
            LargePage = 1ULL << 7,
            Global = 1ULL << 8,
            NoExecute = 1ULL << 63,
        };

        struct TableRecord final {
            Memory::PhysicalAddress Address{};
            Memory::PhysicalAllocation* Ownership{};
            TableRecord* Previous{};
            TableRecord* Next{};
            TableRecord* NextFree{};
        };

        struct TableResolution final {
            Entry* ParentEntry{};
            Entry OriginalEntry;
            Memory::PhysicalAddress Address{};
            bool Created{};
            bool Modified{};
        };

        [[nodiscard]] static Memory::Uint64 Pml4Index(Memory::VirtualAddress address) noexcept;
        [[nodiscard]] static Memory::Uint64 PdptIndex(Memory::VirtualAddress address) noexcept;
        [[nodiscard]] static Memory::Uint64 PdIndex(Memory::VirtualAddress address) noexcept;
        [[nodiscard]] static Memory::Uint64 PtIndex(Memory::VirtualAddress address) noexcept;
        [[nodiscard]] static Memory::Uint64 PageOffset(Memory::VirtualAddress address) noexcept;
        [[nodiscard]] static bool ValidOptions(Memory::MappingOptions options) noexcept;
        [[nodiscard]] static Memory::Uint64 LeafFlags(Memory::MappingOptions options) noexcept;
        [[nodiscard]] static Memory::MappingOptions DecodeOptions(Entry entry) noexcept;
        [[nodiscard]] static Memory::PhysicalAddress EntryAddress(Entry entry) noexcept;
        [[nodiscard]] static bool TableIsEmpty(const Entry* table) noexcept;
        static void InvalidatePage(Memory::VirtualAddress address) noexcept;

        [[nodiscard]] Entry* TablePointer(Memory::PhysicalAddress address) const noexcept;
        [[nodiscard]] PageMapInitializationError AllocateTable(Memory::PhysicalAddress& output) noexcept;
        [[nodiscard]] MappingError AllocateTableForMapping(Memory::PhysicalAddress& output) noexcept;
        [[nodiscard]] TableRecord* AcquireTableRecord() noexcept;
        [[nodiscard]] TableRecord* FindTableRecord(Memory::PhysicalAddress address) noexcept;
        [[nodiscard]] const TableRecord* FindTableRecord(Memory::PhysicalAddress address) const noexcept;
        [[nodiscard]] bool ReleaseTable(Memory::PhysicalAddress address) noexcept;
        [[nodiscard]] MappingError ResolveNextTable(Entry* table, Memory::Uint64 index, bool userMapping, TableResolution& output) noexcept;
        [[nodiscard]] const Entry* ResolveExistingNextTable(const Entry* table, Memory::Uint64 index, MappingError& error) const noexcept;
        [[nodiscard]] Entry* ResolveExistingNextTable(Entry* table, Memory::Uint64 index, MappingError& error) noexcept;
        [[nodiscard]] bool RollbackResolution(TableResolution& resolution) noexcept;
        void RecycleTableRecord(TableRecord& record) noexcept;

        bool m_Active{};
        Memory::PhysicalMemoryManager* m_PhysicalMemory{};
        Memory::BootstrapMetadataArena* m_Metadata{};
        Memory::PhysicalAddress m_RootTable{};
        TableRecord* m_TableRecords{};
        TableRecord* m_RecycledTableRecords{};
        PageMapStatistics m_Statistics{};
    };
}