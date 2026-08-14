#pragma once

#include <Kernel/Memory/VirtualMemory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class PageMap;
}

namespace Zos::Kernel::Memory {
    enum class KernelHeapError : Uint32 {
        Success,
        AlreadyInitialized,
        NotInitialized,
        InvalidDependency,
        InvalidRequest,
        OutputAlreadyOwnsAllocation,
        VirtualAllocationFailed,
        PhysicalAllocationFailed,
        MappingFailed,
        ArenaExhausted,
        InvalidPointer,
        DoubleFree,
        CorruptHeap,
        RollbackFailed,
    };

    struct KernelHeapStatistics final {
        Uint64 ReservedPages{};
        Uint64 CommittedPages{};
        Uint64 SegmentCount{};
        Uint64 AllocationCount{};
        Uint64 RequestedBytes{};
        Uint64 AllocatedBlockBytes{};
        Uint64 FreeBlockBytes{};
        Uint64 SegmentMetadataBytes{};

        [[nodiscard]] constexpr Uint64 ReservedBytes() const noexcept { return ReservedPages * PageSize; }
        [[nodiscard]] constexpr Uint64 CommittedBytes() const noexcept { return CommittedPages * PageSize; } 
    };

    class KernelHeap final {
    public:
        inline static constexpr Uint64 DefaultAlignment{ 16 };

        constexpr KernelHeap() noexcept = default;
        KernelHeap(const KernelHeap&) = delete;
        KernelHeap& operator=(const KernelHeap&) = delete;

        [[nodiscard]] KernelHeapError Initialize(PhysicalMemoryManager& physical_memory, VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map) noexcept;

        [[nodiscard]] KernelHeapError Allocate(Uint64 size, void*& output, Uint64 alignment = DefaultAlignment) noexcept;

        [[nodiscard]] KernelHeapError Reallocate(void* allocation, Uint64 new_size, void*& output) noexcept;

        [[nodiscard]] KernelHeapError Free(void* allocation) noexcept;

        [[nodiscard]] bool Validate() const noexcept;
        [[nodiscard]] bool Contains(const void* address) const noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] VirtualSpan ArenaSpan() const noexcept { return m_ArenaSpan; }
        [[nodiscard]] VirtualSpan CommittedSpan() const noexcept { return VirtualSpan{ m_ArenaSpan.Base, m_Statistics.CommittedPages }; }

        [[nodiscard]] VirtualAddress LowerGuard() const noexcept { return m_ArenaReservation.IsValid() ? m_ArenaReservation.Base() : VirtualAddress{}; }
        [[nodiscard]] VirtualAddress UpperGuard() const noexcept { return m_ArenaReservation.IsValid() ? m_ArenaSpan.Base + m_ArenaSpan.SizeBytes() : VirtualAddress{}; }

        [[nodiscard]] const KernelHeapStatistics& Statistics() const noexcept { return m_Statistics; }

        [[nodiscard]] static const char* Describe(KernelHeapError error) noexcept;

    private:
        inline static constexpr Uint64 ArenaSizeBytes{ 1ULL * 1024 * 1024 * 1024 * 1024 };
        inline static constexpr Uint64 ArenaPageCount{ ArenaSizeBytes / PageSize };
        inline static constexpr Uint64 ArenaGuardPages{ 2 };
        inline static constexpr Uint64 DefaultSegmentPages{ 16 };
        inline static constexpr Uint64 BlockAlignment{ 16 };

        inline static constexpr Uint64 SegmentMagic{ 0x5A4F534845415053ULL };
        inline static constexpr Uint64 BlockMagic{ 0x5A4F53424C4F434BULL };
        inline static constexpr Uint64 DeadBlockMagic{ 0xDEAD424C4F434B00ULL };
        inline static constexpr Uint64 PrefixMagic{ 0x5A4F53414C4C4F43ULL };

        inline static constexpr Uint64 BlockFree{ 1 };
        inline static constexpr Uint64 BlockAllocated{ 2 };
        inline static constexpr Uint64 BlockDead{ 3 };

        struct BlockHeader;

        struct alignas(16) HeapSegment final {
            Uint64 Magic{};
            VirtualAddress Base{};
            Uint64 PageCount{};
            Uint64 MetadataBytes{};
            BlockHeader* FirstBlock{};
            HeapSegment* Previous{};
            HeapSegment* Next{};
        };

        struct alignas(16) BlockHeader final {
            Uint64 Magic{};
            Uint64 TotalBytes{};
            Uint64 RequestedBytes{};
            Uint64 Alignment{};
            Uint64 UserOffset{};
            Uint64 State{};
            HeapSegment* Segment{};
            BlockHeader* Previous{};
            BlockHeader* Next{};
        };

        struct AllocationPrefix final {
            Uint64 Magic{};
            BlockHeader* Block{};
        };

        struct AllocationPlacement final {
            Uint64 UserAddress{};
            Uint64 ConsumedBytes{};
        };

        static_assert((ArenaSizeBytes % PageSize) == 0);
        static_assert(ArenaSizeBytes < Layout::KernelDynamicSize);
        static_assert((sizeof(HeapSegment) % alignof(PhysicalAllocation)) == 0);
        static_assert((sizeof(BlockHeader) % BlockAlignment) == 0);
        static_assert(sizeof(AllocationPrefix) == 16);

        [[nodiscard]] static bool IsPowerOfTwo(Uint64 value) noexcept;
        [[nodiscard]] static bool TryAdd(Uint64 left, Uint64 right, Uint64& result) noexcept;
        [[nodiscard]] static bool TryMultiply(Uint64 left, Uint64 right, Uint64& result) noexcept;
        [[nodiscard]] static bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept;
        [[nodiscard]] static Uint64 BackingStorageOffset() noexcept;
        [[nodiscard]] static Uint64 MinimumBlockBytes() noexcept;

        [[nodiscard]] bool ComputeSegmentLayout(Uint64 minimum_payload_bytes, Uint64 alignment, Uint64& page_count, Uint64& metadata_bytes) const noexcept;

        [[nodiscard]] static PhysicalAllocation* BackingAt(HeapSegment& segment, Uint64 index) noexcept;
        [[nodiscard]] static const PhysicalAllocation* BackingAt(const HeapSegment& segment, Uint64 index) noexcept;

        [[nodiscard]] KernelHeapError CreateSegment(Uint64 minimum_payload_bytes, Uint64 alignment) noexcept;
        
        [[nodiscard]] bool RollbackSegmentCreation(HeapSegment& segment, Uint64 mapped_pages) noexcept;

        [[nodiscard]] static bool TryPlacement(const BlockHeader& block, Uint64 size, Uint64 alignment, AllocationPlacement& placement) noexcept;

        [[nodiscard]] KernelHeapError AllocateFromBlock(BlockHeader& header, Uint64 size, Uint64 alignment, void*& output) noexcept;

        [[nodiscard]] static BlockHeader* SplitBlock(BlockHeader& block, Uint64 first_block_bytes) noexcept;

        static void AbsorbNextBlock(BlockHeader& block) noexcept;
        [[nodiscard]] static BlockHeader* CoalesceFreeBlock(BlockHeader& block) noexcept;

        [[nodiscard]] HeapSegment* FindSegmentContaining(Uint64 address) noexcept;
        [[nodiscard]] const HeapSegment* FindSegmentContaining(Uint64 address) const noexcept;

        [[nodiscard]] KernelHeapError ResolveAllocation(void* allocation, HeapSegment*& segment, BlockHeader*& block) noexcept;

        PhysicalMemoryManager* m_PhysicalMemory{};
        VirtualAddressAllocator* m_VirtualAddresses{};
        Architecture::AMD64::PageMap* m_PageMap{};

        VirtualReservation m_ArenaReservation{};
        VirtualSpan m_ArenaSpan{};

        HeapSegment* m_SegmentHead{};
        HeapSegment* m_SegmentTail{};

        KernelHeapStatistics m_Statistics{};
        bool m_Initialized{};
    };
}