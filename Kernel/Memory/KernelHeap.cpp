#include <Kernel/Memory/KernelHeap.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>

extern "C" {
    void* memset(void* dst, int v, unsigned long long n) noexcept;
    void* memcpy(void* dst, const void* src, unsigned long long n) noexcept;
}

void* operator new(__SIZE_TYPE__, void* address) noexcept;

namespace Zos::Kernel::Memory {
    namespace {
        constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };
    }

    bool KernelHeap::IsPowerOfTwo(Uint64 value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool KernelHeap::TryAdd(Uint64 left, Uint64 right, Uint64& result) noexcept {
        if (right > MaximumValue - left) return false;
        result = left + right;
        return true;
    }

    bool KernelHeap::TryMultiply(Uint64 left, Uint64 right, Uint64& result) noexcept {
        if (left != 0 && right > MaximumValue / left) return false;
        result = left * right;
        return true;
    }

    bool KernelHeap::TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
        if (!IsPowerOfTwo(alignment)) return false;

        const Uint64 mask = alignment - 1;
        if (value > MaximumValue - mask) return false;

        result = (value + mask) & ~mask;
        return true;
    }

    Uint64 KernelHeap::BackingStorageOffset() noexcept {
        const Uint64 alignment = alignof(PhysicalAllocation);
        return (sizeof(HeapSegment) + alignment - 1) & ~(alignment - 1);
    }

    Uint64 KernelHeap::MinimumBlockBytes() noexcept {
        Uint64 minimum = sizeof(BlockHeader) + sizeof(AllocationPrefix) + DefaultAlignment;
        return (minimum + BlockAlignment - 1) & ~(BlockAlignment - 1);
    }

    bool KernelHeap::ComputeSegmentLayout(Uint64 minimum_payload_bytes, Uint64 alignment, Uint64& page_count, Uint64& metadata_bytes) const noexcept {
        page_count = 0;
        metadata_bytes = 0;

        if (minimum_payload_bytes == 0 || !IsPowerOfTwo(alignment)) return false;
        if (alignment < DefaultAlignment) alignment = DefaultAlignment;

        Uint64 candidate_pages = DefaultSegmentPages;
        for (;;) {
            if (candidate_pages == 0 || candidate_pages > ArenaPageCount) return false;

            Uint64 backing_bytes = 0;
            if (!TryMultiply(candidate_pages, sizeof(PhysicalAllocation), backing_bytes)) return false;

            Uint64 metadata_end = 0;
            if (!TryAdd(BackingStorageOffset(), backing_bytes, metadata_end)) return false;
            if (!TryAlignUp(metadata_end, BlockAlignment, metadata_end)) return false;

            Uint64 required_block_bytes = sizeof(BlockHeader) + sizeof(AllocationPrefix);
            if (!TryAdd(required_block_bytes, alignment - 1, required_block_bytes)) return false;
            if (!TryAdd(required_block_bytes, minimum_payload_bytes, required_block_bytes)) return false;
            if (!TryAlignUp(required_block_bytes, BlockAlignment, required_block_bytes)) return false;

            if (required_block_bytes < MinimumBlockBytes())
                required_block_bytes = MinimumBlockBytes();

            Uint64 required_bytes = 0;
            if (!TryAdd(metadata_end, required_block_bytes, required_bytes)) return false;

            Uint64 rounded_bytes = 0;
            if (!TryAdd(required_bytes, PageSize - 1, rounded_bytes)) return false;

            Uint64 required_pages = rounded_bytes / PageSize;
            if (required_pages < DefaultSegmentPages)
                required_pages = DefaultSegmentPages;

            if (required_pages <= candidate_pages) {
                page_count = candidate_pages;
                metadata_bytes = metadata_end;
                return true;
            }

            candidate_pages = required_pages;
        }
    }

    PhysicalAllocation* KernelHeap::BackingAt(HeapSegment& segment, Uint64 index) noexcept {
        auto* storage = reinterpret_cast<Uint8*>(&segment) + BackingStorageOffset();
        return reinterpret_cast<PhysicalAllocation*>(storage) + index;
    }

    const PhysicalAllocation* KernelHeap::BackingAt(const HeapSegment& segment, Uint64 index) noexcept {
        const auto* storage = reinterpret_cast<const Uint8*>(&segment) + BackingStorageOffset();
        return reinterpret_cast<const PhysicalAllocation*>(storage) + index;
    }

    KernelHeapError KernelHeap::Initialize(PhysicalMemoryManager& physical_memory, VirtualAddressAllocator& virtual_addresses, Architecture::AMD64::PageMap& page_map) noexcept {
        if (m_Initialized || m_ArenaReservation.IsValid())
            return KernelHeapError::AlreadyInitialized;

        if (!physical_memory.IsInitialized() || !physical_memory.IsMetadataAccessPromoted() ||
            !virtual_addresses.IsInitialized() || !page_map.IsInitialized() || !page_map.IsActive())
            return KernelHeapError::InvalidDependency;

        VirtualAllocationConstraints constraints{
            .Alignment = PageSize,
            .Preference = VirtualAllocationPreference::LowAddresses,
        };

        const Uint64 reservation_pages = ArenaPageCount + ArenaGuardPages;
        const VirtualAllocationError reservation_error = virtual_addresses.Reserve(reservation_pages, m_ArenaReservation, constraints);
        if (reservation_error != VirtualAllocationError::Success)
            return KernelHeapError::VirtualAllocationFailed;

        m_PhysicalMemory = &physical_memory;
        m_VirtualAddresses = &virtual_addresses;
        m_PageMap = &page_map;
        m_ArenaSpan = VirtualSpan{ m_ArenaReservation.Base() + PageSize, ArenaPageCount };
        m_Statistics.ReservedPages = ArenaPageCount;

        if (page_map.IsMapped(LowerGuard()) || page_map.IsMapped(UpperGuard())) {
            /*
             * The VAA handed us a range that the active page map says is
             * already occupied. Returning it to the VAA would make that 
             * disagreement worse, so preserve the reservation and fail hard.
             */
            return KernelHeapError::CorruptHeap;
        }

        const KernelHeapError segment_error = CreateSegment(1, DefaultAlignment);
        if (segment_error != KernelHeapError::Success) {
            /*
             * A rollback failure means some mapping or physical ownership
             * may still be live inside the arena. Do not return that virtual
             * range to the VAA in that state. Startup treats this as fatal
             */
            if (segment_error == KernelHeapError::RollbackFailed) return segment_error;

            const VirtualAllocationError release_error = virtual_addresses.Release(m_ArenaReservation);
            m_PhysicalMemory = nullptr;
            m_VirtualAddresses = nullptr;
            m_PageMap = nullptr;
            m_ArenaSpan = {};
            m_SegmentHead = nullptr;
            m_SegmentTail = nullptr;
            m_Statistics = {};
            if (release_error != VirtualAllocationError::Success)
                return KernelHeapError::RollbackFailed;
            return segment_error;
        }

        m_Initialized = true;
        return KernelHeapError::Success;
    }

    bool KernelHeap::RollbackSegmentCreation(HeapSegment& segment, Uint64 mapped_pages) noexcept {
        using Architecture::AMD64::MappingError;

        if (mapped_pages == 0 || mapped_pages > segment.PageCount) return false;

        for (Uint64 count = mapped_pages; count > 1; count--) {
            const Uint64 index = count - 1;
            PhysicalAllocation * backing = BackingAt(segment, index);
            const VirtualAddress virtual_address = segment.Base + index * PageSize;

            if (m_PageMap->UnmapPage(virtual_address) != MappingError::Success) return false;
            if (m_PhysicalMemory->Release(*backing) != PhysicalAllocationError::Success) return false;
        }

        PhysicalAllocation first_backing(static_cast<PhysicalAllocation&&>(*BackingAt(segment, 0)));
        if (m_PageMap->UnmapPage(segment.Base) != MappingError::Success) return false;
        if (m_PhysicalMemory->Release(first_backing) != PhysicalAllocationError::Success) return false;
        return true;
    }

    KernelHeapError KernelHeap::CreateSegment(Uint64 minimum_payload_bytes, Uint64 alignment) noexcept {
        using Architecture::AMD64::MappingError;

        if (m_PhysicalMemory == nullptr || m_PageMap == nullptr || m_ArenaSpan.IsEmpty()) 
            return KernelHeapError::InvalidDependency;

        Uint64 page_count = 0;
        Uint64 metadata_bytes = 0;
        if (!ComputeSegmentLayout(minimum_payload_bytes, alignment, page_count, metadata_bytes)) 
            return KernelHeapError::InvalidRequest;

        if (m_Statistics.CommittedPages > m_Statistics.ReservedPages ||
            page_count > m_Statistics.ReservedPages - m_Statistics.CommittedPages)
            return KernelHeapError::ArenaExhausted;

        Uint64 committed_bytes = 0; 
        if (!TryMultiply(m_Statistics.CommittedPages, PageSize, committed_bytes)) 
            return KernelHeapError::CorruptHeap;

        const VirtualAddress segment_base = m_ArenaSpan.Base + committed_bytes;
        HeapSegment* segment = nullptr;
        Uint64 mapped_pages = 0;

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        for (Uint64 page = 0; page < page_count; page++) {
            PhysicalAllocation backing{};
            const PhysicalAllocationError allocation_error = m_PhysicalMemory->AllocatePage(backing);
            if (allocation_error != PhysicalAllocationError::Success) {
                if (mapped_pages != 0 && !RollbackSegmentCreation(*segment, mapped_pages))
                    return KernelHeapError::RollbackFailed;
                return KernelHeapError::PhysicalAllocationFailed;
            }

            const VirtualAddress virtual_address = segment_base + page * PageSize;
            if (m_PageMap->IsMapped(virtual_address)) {
                if (m_PhysicalMemory->Release(backing) != PhysicalAllocationError::Success) 
                    return KernelHeapError::RollbackFailed;
                if (mapped_pages != 0 && !RollbackSegmentCreation(*segment, mapped_pages)) 
                    return KernelHeapError::RollbackFailed;
                return KernelHeapError::CorruptHeap;
            }

            const MappingError mapping_error = m_PageMap->MapPage(virtual_address, backing.Base(), options);
            if (mapping_error != MappingError::Success) {
                if (m_PhysicalMemory->Release(backing) != PhysicalAllocationError::Success) 
                    return KernelHeapError::RollbackFailed;
                if (mapped_pages != 0 && !RollbackSegmentCreation(*segment, mapped_pages)) 
                    return KernelHeapError::RollbackFailed;
                return KernelHeapError::CorruptHeap;
            }

            memset(reinterpret_cast<void*>(virtual_address.Value()), 0, PageSize);

            if (page == 0) {
                segment = reinterpret_cast<HeapSegment*>(segment_base.Value());
                new (segment) HeapSegment{};
                segment->Magic = SegmentMagic;
                segment->Base = segment_base;
                segment->PageCount = page_count;
                segment->MetadataBytes = metadata_bytes;
            }

            Uint64 slot_bytes = 0;
            Uint64 slot_offset = 0;
            if (!TryMultiply(page, sizeof(PhysicalAllocation), slot_bytes) ||
                !TryAdd(BackingStorageOffset(), slot_bytes, slot_offset)) {
                bool current_page_released = m_PageMap->UnmapPage(virtual_address) == MappingError::Success;
                if (current_page_released)
                    current_page_released = m_PhysicalMemory->Release(backing) == PhysicalAllocationError::Success;

                const bool previous_pages_released = mapped_pages == 0 || RollbackSegmentCreation(*segment, mapped_pages);
                return current_page_released && previous_pages_released ? KernelHeapError::CorruptHeap : KernelHeapError::RollbackFailed;
            }

            new (BackingAt(*segment, page)) PhysicalAllocation(static_cast<PhysicalAllocation&&>(backing));
            mapped_pages++;
        }

        Uint64 segment_bytes = 0;
        if (!TryMultiply(page_count, PageSize, segment_bytes) || 
            metadata_bytes >= segment_bytes ||
            segment_bytes - metadata_bytes < MinimumBlockBytes()) {
            if (!RollbackSegmentCreation(*segment, mapped_pages))
                return KernelHeapError::RollbackFailed;
            return KernelHeapError::CorruptHeap;       
        }

        auto* first_block = reinterpret_cast<BlockHeader*>(segment_base.Value() + metadata_bytes);
        new (first_block) BlockHeader{};
        first_block->Magic = BlockMagic;
        first_block->TotalBytes = segment_bytes - metadata_bytes;
        first_block->State = BlockFree;
        first_block->Segment = segment;
        segment->FirstBlock = first_block;

        Uint64 next_committed_pages = 0;
        Uint64 next_segment_count = 0;
        Uint64 next_metadata_bytes = 0;
        Uint64 next_free_bytes = 0;

        if (!TryAdd(m_Statistics.CommittedPages, page_count, next_committed_pages) ||
            !TryAdd(m_Statistics.SegmentCount, 1, next_segment_count) ||
            !TryAdd(m_Statistics.SegmentMetadataBytes, metadata_bytes, next_metadata_bytes) ||
            !TryAdd(m_Statistics.FreeBlockBytes, first_block->TotalBytes, next_free_bytes)) {
            if (!RollbackSegmentCreation(*segment, mapped_pages))
                return KernelHeapError::RollbackFailed;
            return KernelHeapError::CorruptHeap;
        }

        segment->Previous = m_SegmentTail;
        if (m_SegmentTail != nullptr) m_SegmentTail->Next = segment;
        else m_SegmentHead = segment;
        m_SegmentTail = segment;

        m_Statistics.CommittedPages = next_committed_pages;
        m_Statistics.SegmentCount = next_segment_count;
        m_Statistics.SegmentMetadataBytes = next_metadata_bytes;
        m_Statistics.FreeBlockBytes = next_free_bytes;

        return KernelHeapError::Success;
    }

    bool KernelHeap::TryPlacement(const BlockHeader& block, Uint64 size, Uint64 alignment, AllocationPlacement& placement) noexcept {
        placement = {};
        if (block.Magic != BlockMagic || block.State != BlockFree || size == 0) return false;

        const Uint64 block_base = reinterpret_cast<Uint64>(&block);
        
        Uint64 minimum_user = 0;
        if (!TryAdd(block_base, sizeof(BlockHeader), minimum_user) ||
            !TryAdd(minimum_user, sizeof(AllocationPrefix), minimum_user)) 
            return false;

        Uint64 user_address = 0;
        if (!TryAlignUp(minimum_user, alignment, user_address)) return false;

        Uint64 allocation_end = 0;
        if (!TryAdd(user_address, size, allocation_end)) return false;

        Uint64 consumed_end = 0;
        if (!TryAlignUp(allocation_end, BlockAlignment, consumed_end)) return false;
        if (consumed_end < block_base) return false;
        
        const Uint64 consumed_bytes = consumed_end - block_base;
        if (consumed_bytes > block.TotalBytes) return false;

        placement.UserAddress = user_address;
        placement.ConsumedBytes = consumed_bytes;
        return true;
    }

    KernelHeap::BlockHeader* KernelHeap::SplitBlock(BlockHeader& block, Uint64 first_block_bytes) noexcept {
        if (block.Magic != BlockMagic || first_block_bytes < sizeof(BlockHeader) ||
            (first_block_bytes & (BlockAlignment - 1)) != 0 ||
            first_block_bytes >= block.TotalBytes) return nullptr;

        const Uint64 remainder = block.TotalBytes - first_block_bytes;
        if (remainder < MinimumBlockBytes()) return nullptr;

        const Uint64 suffix_address = reinterpret_cast<Uint64>(&block) + first_block_bytes;
        auto* suffix = reinterpret_cast<BlockHeader*>(suffix_address);
        new (suffix) BlockHeader{};
        suffix->Magic = BlockMagic;
        suffix->TotalBytes = remainder;
        suffix->State = BlockFree;
        suffix->Segment = block.Segment;
        suffix->Previous = &block;
        suffix->Next = block.Next;

        if (block.Next != nullptr) block.Next->Previous = suffix;
        block.Next = suffix;
        block.TotalBytes = first_block_bytes;

        return suffix;
    }

    void KernelHeap::AbsorbNextBlock(BlockHeader& block) noexcept {
        BlockHeader* next = block.Next;
        if (next == nullptr) return;

        block.TotalBytes += next->TotalBytes;
        block.Next = next->Next;
        if (next->Next != nullptr) next->Next->Previous = &block;

        next->Magic = DeadBlockMagic;
        next->TotalBytes = 0;
        next->RequestedBytes = 0;
        next->Alignment = 0;
        next->UserOffset = 0;
        next->State = BlockDead;
        next->Segment = nullptr;
        next->Previous = nullptr;
        next->Next = nullptr;
    }

    KernelHeap::BlockHeader* KernelHeap::CoalesceFreeBlock(BlockHeader& block) noexcept {
        BlockHeader* result = &block;

        if (result->Next != nullptr && result->Next->State == BlockFree) 
            AbsorbNextBlock(*result);
        if (result->Previous != nullptr && result->Previous->State == BlockFree) {
            result = result->Previous;
            AbsorbNextBlock(*result);
        }
        return result;
    }

    KernelHeapError KernelHeap::AllocateFromBlock(BlockHeader& block, Uint64 size, Uint64 alignment, void*& output) noexcept {
        AllocationPlacement placement{};
        if (!TryPlacement(block, size, alignment, placement))
            return KernelHeapError::CorruptHeap;

        const Uint64 remainder = block.TotalBytes - placement.ConsumedBytes;
        const bool split = remainder >= MinimumBlockBytes();
        const Uint64 allocated_bytes = split ? placement.ConsumedBytes : block.TotalBytes;
        if (m_Statistics.FreeBlockBytes < allocated_bytes)
            return KernelHeapError::CorruptHeap;

        Uint64 next_allocated_bytes = 0;
        Uint64 next_requested_bytes = 0;
        Uint64 next_allocation_count = 0;
        if (!TryAdd(m_Statistics.AllocatedBlockBytes, allocated_bytes, next_allocated_bytes) ||
            !TryAdd(m_Statistics.RequestedBytes, size, next_requested_bytes) ||
            !TryAdd(m_Statistics.AllocationCount, 1, next_allocation_count))
            return KernelHeapError::CorruptHeap;
        
        /*
         * All accounting checks are complete before the free-list structure
         * is modified, so this operation cannot leave a half-split block on 
         * an ordinary error return.
         */
        if (split && SplitBlock(block, placement.ConsumedBytes) == nullptr)
            return KernelHeapError::CorruptHeap;

        block.RequestedBytes = size;
        block.Alignment = alignment;
        block.UserOffset = placement.UserAddress - reinterpret_cast<Uint64>(&block);
        block.State = BlockAllocated;

        auto* prefix = reinterpret_cast<AllocationPrefix*>(placement.UserAddress - sizeof(AllocationPrefix));
        new (prefix) AllocationPrefix{};
        prefix->Magic = PrefixMagic;
        prefix->Block = &block;

        m_Statistics.FreeBlockBytes -= allocated_bytes;
        m_Statistics.AllocatedBlockBytes = next_allocated_bytes;
        m_Statistics.RequestedBytes = next_requested_bytes;
        m_Statistics.AllocationCount = next_allocation_count;

        output = reinterpret_cast<void*>(placement.UserAddress);
        return KernelHeapError::Success;
    }

    KernelHeapError KernelHeap::Allocate(Uint64 size, void*& output, Uint64 alignment) noexcept {
        if (!m_Initialized) return KernelHeapError::NotInitialized;
        if (output != nullptr) return KernelHeapError::OutputAlreadyOwnsAllocation;
        if (size == 0 || !IsPowerOfTwo(alignment)) return KernelHeapError::InvalidRequest;

        if (alignment < DefaultAlignment)
            alignment = DefaultAlignment;

        for (HeapSegment* segment = m_SegmentHead; segment != nullptr; segment = segment->Next) {
            if (segment->Magic != SegmentMagic) return KernelHeapError::CorruptHeap;
            for (BlockHeader* block = segment->FirstBlock; block != nullptr; block = block->Next) {
                if (block->Magic != BlockMagic) return KernelHeapError::CorruptHeap;
                if (block->State != BlockFree) continue;

                AllocationPlacement placement{};
                if (!TryPlacement(*block, size, alignment, placement)) continue;
                return AllocateFromBlock(*block, size, alignment, output);
            }
        }

        const KernelHeapError grow_error = CreateSegment(size, alignment);
        if (grow_error != KernelHeapError::Success) return grow_error;
        if (m_SegmentTail == nullptr || m_SegmentTail->FirstBlock == nullptr) 
            return KernelHeapError::CorruptHeap;

        return AllocateFromBlock(*m_SegmentTail->FirstBlock, size, alignment, output);
    }

    KernelHeap::HeapSegment* KernelHeap::FindSegmentContaining(Uint64 address) noexcept {
        for (HeapSegment* segment = m_SegmentHead; segment != nullptr; segment = segment->Next) {
            Uint64 segment_bytes = 0;
            Uint64 segment_end = 0;
            if (!TryMultiply(segment->PageCount, PageSize, segment_bytes) ||
                !TryAdd(segment->Base.Value(), segment_bytes, segment_end))
                return nullptr;

            if (address >= segment->Base.Value() && address < segment_end)
                return segment;
        }
        return nullptr;
    }

    const KernelHeap::HeapSegment* KernelHeap::FindSegmentContaining(Uint64 address) const noexcept {
        for (const HeapSegment* segment = m_SegmentHead; segment != nullptr; segment = segment->Next) {
            Uint64 segment_bytes = 0;
            Uint64 segment_end = 0;
            if (!TryMultiply(segment->PageCount, PageSize, segment_bytes) ||
                !TryAdd(segment->Base.Value(), segment_bytes, segment_end))
                return nullptr;

            if (address >= segment->Base.Value() && address < segment_end)
                return segment;
        }
        return nullptr;
    }

    bool KernelHeap::Contains(const void* address) const noexcept {
        if (!m_Initialized || address == nullptr) return false;
        return FindSegmentContaining(reinterpret_cast<Uint64>(address)) != nullptr;
    }

    KernelHeapError KernelHeap::ResolveAllocation(void* allocation, HeapSegment*& segment, BlockHeader*& block) noexcept {
        segment = nullptr;
        block = nullptr;

        if (allocation == nullptr) return KernelHeapError::InvalidPointer;

        const Uint64 address = reinterpret_cast<Uint64>(allocation);
        segment = FindSegmentContaining(address);
        if (segment == nullptr || segment->Magic != SegmentMagic) 
            return KernelHeapError::InvalidPointer;

        Uint64 segment_bytes = 0;
        Uint64 segment_end = 0;
        Uint64 usable_base = 0;
        if (!TryMultiply(segment->PageCount, PageSize, segment_bytes) ||
            !TryAdd(segment->Base.Value(), segment_bytes, segment_end) ||
            !TryAdd(segment->Base.Value(), segment->MetadataBytes, usable_base))
            return KernelHeapError::CorruptHeap;

        if (address < usable_base + sizeof(BlockHeader) + sizeof(AllocationPrefix) ||
            address >= segment_end || address < sizeof(AllocationPrefix))
            return KernelHeapError::InvalidPointer;

        const Uint64 prefix_address = address - sizeof(AllocationPrefix);
        if (prefix_address < usable_base - sizeof(BlockHeader) ||
            prefix_address > segment_end - sizeof(AllocationPrefix))
            return KernelHeapError::InvalidPointer;

        auto* prefix = reinterpret_cast<AllocationPrefix*>(prefix_address);
        if (prefix->Magic != PrefixMagic || prefix->Block == nullptr)
            return KernelHeapError::InvalidPointer;

        block = prefix->Block;
        const Uint64 block_address = reinterpret_cast<Uint64>(block);
        if (block_address < usable_base || 
            block_address > segment_end - sizeof(BlockHeader) ||
            (block_address & (BlockAlignment - 1)) != 0 ||
            block->Magic != BlockMagic ||
            block->Segment != segment ||
            block->TotalBytes < sizeof(BlockHeader) ||
            block->TotalBytes > segment_end - block_address) 
            return KernelHeapError::InvalidPointer;

        if (block->Previous != nullptr) {
            const Uint64 previous_address = reinterpret_cast<Uint64>(block->Previous);
            if (previous_address >= block_address ||
                block->Previous->Magic != BlockMagic ||
                block->Previous->Segment != segment ||
                block->Previous->Next != block ||
                block->Previous->TotalBytes > block_address - previous_address ||
                previous_address + block->Previous->TotalBytes != block_address)
                return KernelHeapError::CorruptHeap;
        } else if (segment->FirstBlock != block) {
            return KernelHeapError::CorruptHeap;
        }

        if (block->Next != nullptr) {
            const Uint64 next_address = reinterpret_cast<Uint64>(block->Next);
            if (next_address <= block_address ||
                block->Next->Magic != BlockMagic ||
                block->Next->Segment != segment ||
                block->Next->Previous != block || 
                block_address + block->TotalBytes != next_address)
                return KernelHeapError::CorruptHeap;
        }

        if (block->State == BlockFree) return KernelHeapError::DoubleFree;
        if (block->State != BlockAllocated) return KernelHeapError::InvalidPointer;
        if (block->RequestedBytes == 0 ||
            block->Alignment < DefaultAlignment ||
            !IsPowerOfTwo(block->Alignment) ||
            block->UserOffset < sizeof(BlockHeader) + sizeof(AllocationPrefix) ||
            block->UserOffset >= block->TotalBytes ||
            block_address + block->UserOffset != address ||
            (address & (block->Alignment - 1)) != 0 ||
            block->RequestedBytes > block->TotalBytes - block->UserOffset)
            return KernelHeapError::InvalidPointer;

        return KernelHeapError::Success;
    }

    KernelHeapError KernelHeap::Free(void* allocation) noexcept {
        if (!m_Initialized) return KernelHeapError::NotInitialized;
        if (allocation == nullptr) return KernelHeapError::Success;

        HeapSegment* segment = nullptr;
        BlockHeader* block = nullptr;
        const KernelHeapError resolve_error = ResolveAllocation(allocation, segment, block);
        if (resolve_error != KernelHeapError::Success) return resolve_error;

        (void)segment;

        if (m_Statistics.AllocationCount == 0 ||
            m_Statistics.RequestedBytes < block->RequestedBytes ||
            m_Statistics.AllocatedBlockBytes < block->TotalBytes)
            return KernelHeapError::CorruptHeap;

        Uint64 next_free_bytes = 0;
        if (!TryAdd(m_Statistics.FreeBlockBytes, block->TotalBytes, next_free_bytes))
            return KernelHeapError::CorruptHeap;

        m_Statistics.AllocationCount--;
        m_Statistics.RequestedBytes -= block->RequestedBytes;
        m_Statistics.AllocatedBlockBytes -= block->TotalBytes;
        m_Statistics.FreeBlockBytes = next_free_bytes;

        block->RequestedBytes = 0;
        block->Alignment = 0;
        block->UserOffset = 0;
        block->State = BlockFree;

        (void)CoalesceFreeBlock(*block);
        return KernelHeapError::Success;
    }

    KernelHeapError KernelHeap::Reallocate(void* allocation, Uint64 new_size, void*& output) noexcept {
        if (!m_Initialized) return KernelHeapError::NotInitialized;
        if (output != nullptr) return KernelHeapError::OutputAlreadyOwnsAllocation;
        if (allocation == nullptr) {
            if (new_size == 0) return KernelHeapError::Success;
            return Allocate(new_size, output, DefaultAlignment);
        }

        if (new_size == 0) return Free(allocation);

        HeapSegment* segment = nullptr;
        BlockHeader* block = nullptr;
        const KernelHeapError resolve_error = ResolveAllocation(allocation, segment, block);
        if (resolve_error != KernelHeapError::Success) return resolve_error;

        (void)segment;

        const Uint64 old_requested_bytes = block->RequestedBytes;
        if (new_size == old_requested_bytes) {
            output = allocation;
            return KernelHeapError::Success;
        }

        const Uint64 block_address = reinterpret_cast<Uint64>(block);
        const Uint64 user_address = reinterpret_cast<Uint64>(allocation);

        Uint64 requested_end = 0;
        Uint64 required_end = 0;
        if (!TryAdd(user_address, new_size, requested_end) ||
            !TryAlignUp(requested_end, BlockAlignment, required_end) ||
            required_end < block_address)
            return KernelHeapError::InvalidRequest;

        const Uint64 required_total = required_end - block_address;
        if (required_total <= block->TotalBytes) {
            const Uint64 old_total = block->TotalBytes;
            const Uint64 remainder = old_total - required_total;
            const bool split = remainder >= MinimumBlockBytes();

            if (m_Statistics.RequestedBytes < old_requested_bytes)
                return KernelHeapError::CorruptHeap;

            Uint64 next_requested_bytes = m_Statistics.RequestedBytes - old_requested_bytes;
            if (!TryAdd(next_requested_bytes, new_size, next_requested_bytes))
                return KernelHeapError::CorruptHeap;

            Uint64 next_free_bytes = m_Statistics.FreeBlockBytes;
            Uint64 next_allocated_bytes = m_Statistics.AllocatedBlockBytes;

            if (split) {
                if (next_allocated_bytes < remainder ||
                    !TryAdd(next_free_bytes, remainder, next_free_bytes))
                    return KernelHeapError::CorruptHeap;
                next_allocated_bytes -= remainder;
            }

            if (split) {
                BlockHeader* suffix = SplitBlock(*block, required_total);
                if (suffix == nullptr) return KernelHeapError::CorruptHeap;
                (void)CoalesceFreeBlock(*suffix);
            }

            m_Statistics.RequestedBytes = next_requested_bytes;
            m_Statistics.FreeBlockBytes = next_free_bytes;
            m_Statistics.AllocatedBlockBytes = next_allocated_bytes;

            block->RequestedBytes = new_size;
            output = allocation;
            return KernelHeapError::Success;
        }

        BlockHeader* next = block->Next;
        if (next != nullptr && next->Magic == BlockMagic && next->State == BlockFree) {
            Uint64 combined = 0;
            if (!TryAdd(block->TotalBytes, next->TotalBytes, combined))
                return KernelHeapError::CorruptHeap;

            if (required_total <= combined) {
                const Uint64 old_total = block->TotalBytes;
                Uint64 final_total = combined;
                if (combined - required_total >= MinimumBlockBytes())
                    final_total = required_total;

                const Uint64 allocation_growth = final_total = old_total;
                const Uint64 requested_growth = new_size - old_requested_bytes;

                if (m_Statistics.FreeBlockBytes < allocation_growth ||
                    m_Statistics.AllocatedBlockBytes > MaximumValue - allocation_growth ||
                    m_Statistics.RequestedBytes > MaximumValue - requested_growth)
                    return KernelHeapError::CorruptHeap;

                AbsorbNextBlock(*block);

                if (combined - required_total >= MinimumBlockBytes())
                    if (SplitBlock(*block, required_total) == nullptr)
                        return KernelHeapError::CorruptHeap;

                m_Statistics.FreeBlockBytes -= allocation_growth;
                m_Statistics.AllocatedBlockBytes += allocation_growth;
                m_Statistics.RequestedBytes += requested_growth;
                block->RequestedBytes = new_size;
                output = allocation;
                return KernelHeapError::Success;
            }
        }

        void* replacement = nullptr;
        const KernelHeapError allocation_error = Allocate(new_size, replacement, block->Alignment);
        if (allocation_error != KernelHeapError::Success) return allocation_error;

        const Uint64 copy_bytes = old_requested_bytes < new_size ? old_requested_bytes : new_size;
        memcpy(replacement, allocation, copy_bytes);

        const KernelHeapError free_error = Free(allocation);
        if (free_error != KernelHeapError::Success) {
            const KernelHeapError rollback_error = Free(replacement);
            return rollback_error == KernelHeapError::Success ? KernelHeapError::CorruptHeap : KernelHeapError::RollbackFailed;
        }

        output = replacement;
        return KernelHeapError::Success;
    }

    bool KernelHeap::Validate() const noexcept {
        using Architecture::AMD64::TranslationResult;
        if (!m_Initialized || m_PhysicalMemory == nullptr || m_VirtualAddresses == nullptr || m_PageMap == nullptr ||
            !m_ArenaReservation.IsValid() || m_ArenaSpan.IsEmpty() ||
            m_Statistics.ReservedPages != m_ArenaSpan.PageCount || m_Statistics.CommittedPages > m_Statistics.ReservedPages)
            return false;

        if (m_PageMap->IsMapped(LowerGuard()) || m_PageMap->IsMapped(UpperGuard()))
            return false;

        if (m_Statistics.CommittedPages < m_Statistics.ReservedPages) {
            const VirtualAddress frontier = m_ArenaSpan.Base + m_Statistics.CommittedPages * PageSize;
            if (m_PageMap->IsMapped(frontier)) return false;
        }

        Uint64 observed_segments = 0;
        Uint64 observed_pages = 0;
        Uint64 observed_metadata = 0;
        Uint64 observed_free = 0;
        Uint64 observed_allocated = 0;
        Uint64 observed_requested = 0;
        Uint64 observed_allocations = 0;

        const HeapSegment* previous_segment = nullptr;
        for (const HeapSegment* segment = m_SegmentHead; segment != nullptr; segment = segment->Next) {
            if (segment->Magic != SegmentMagic || segment->Previous != previous_segment ||
                segment->PageCount == 0 || segment->MetadataBytes < BackingStorageOffset() ||
                (segment->MetadataBytes & (BlockAlignment - 1)) != 0)
                return false;

            const VirtualAddress expected_base = m_ArenaSpan.Base + observed_pages * PageSize;
            if (segment->Base != expected_base) return false;

            Uint64 segment_bytes = 0;
            if (!TryMultiply(segment->PageCount, PageSize, segment_bytes) ||
                segment->MetadataBytes >= segment_bytes) return false;

            Uint64 token_bytes = 0;
            Uint64 token_end = 0;
            if (!TryMultiply(segment->PageCount, sizeof(PhysicalAllocation), token_bytes) ||
                !TryAdd(BackingStorageOffset(), token_bytes, token_end) ||
                token_end > segment->MetadataBytes) return false;

            for (Uint64 page = 0; page < segment->PageCount; ++page) {
                const PhysicalAllocation* backing = BackingAt(*segment, page);
                if (!backing->IsValid() || backing->PageCount() != 1) return false;

                const VirtualAddress virtual_address = segment->Base + page * PageSize;
                const TranslationResult translation = m_PageMap->Translate(virtual_address);

                if (!translation.Mapped ||
                    translation.Physical != backing->Base() ||
                    translation.Options.Access != (PageAccess::Read | PageAccess::Write | PageAccess::Global) ||
                    translation.Options.Cache != CachePolicy::WriteBack)
                    return false;
            }

            const Uint64 segment_end = segment->Base.Value() + segment_bytes;
            Uint64 expected_block_address = segment->Base.Value() + segment->MetadataBytes;
            const BlockHeader* previous_block = nullptr;

            if (segment->FirstBlock == nullptr || reinterpret_cast<Uint64>(segment->FirstBlock) != expected_block_address)
                return false;

            for (const BlockHeader* block = segment->FirstBlock; block != nullptr; block = block->Next) {
                const Uint64 block_address = reinterpret_cast<Uint64>(block);
                if (block_address != expected_block_address || block->Magic != BlockMagic ||
                    block->Segment != segment || block->Previous != previous_block ||
                    block->TotalBytes < sizeof(BlockHeader) || block->TotalBytes > segment_end - block_address)
                    return false;

                if (block->State == BlockFree) {
                    if (block->RequestedBytes != 0 || block->Alignment != 0 || block->UserOffset != 0 ||
                        (block->Next != nullptr && block->Next->State == BlockFree))
                        return false;

                    if (!TryAdd(observed_free, block->TotalBytes, observed_free))
                        return false;
                } else if (block->State == BlockAllocated) {
                    if (block->RequestedBytes == 0 || block->Alignment < DefaultAlignment ||
                        !IsPowerOfTwo(block->Alignment) ||
                        block->UserOffset < sizeof(BlockHeader) + sizeof(AllocationPrefix) ||
                        block->UserOffset >= block->TotalBytes ||
                        block->RequestedBytes > block->TotalBytes - block->UserOffset)
                        return false;

                    const Uint64 user_address = block_address + block->UserOffset;
                    if ((user_address & (block->Alignment - 1)) != 0) return false;

                    const auto* prefix = reinterpret_cast<const AllocationPrefix*>(user_address - sizeof(AllocationPrefix)
                    );
                    if (prefix->Magic != PrefixMagic || prefix->Block != block)
                        return false;

                    if (!TryAdd(observed_allocated, block->TotalBytes, observed_allocated) ||
                        !TryAdd(observed_requested, block->RequestedBytes, observed_requested) ||
                        !TryAdd(observed_allocations, 1, observed_allocations))
                        return false;
                } else return false;

                expected_block_address += block->TotalBytes;
                previous_block = block;
            }

            if (expected_block_address != segment_end) return false;
            if (!TryAdd(observed_segments, 1, observed_segments) ||
                !TryAdd(observed_pages, segment->PageCount, observed_pages) ||
                !TryAdd(observed_metadata, segment->MetadataBytes, observed_metadata))
                return false;

            previous_segment = segment;
        }

        if (previous_segment != m_SegmentTail ||
            observed_segments != m_Statistics.SegmentCount ||
            observed_pages != m_Statistics.CommittedPages ||
            observed_metadata != m_Statistics.SegmentMetadataBytes ||
            observed_free != m_Statistics.FreeBlockBytes ||
            observed_allocated != m_Statistics.AllocatedBlockBytes ||
            observed_requested != m_Statistics.RequestedBytes ||
            observed_allocations != m_Statistics.AllocationCount)
            return false;

        Uint64 classified_bytes = 0;
        if (!TryAdd(observed_metadata, observed_free, classified_bytes) ||
            !TryAdd(classified_bytes, observed_allocated, classified_bytes))
            return false;

        return classified_bytes == m_Statistics.CommittedBytes();
    }

    const char* KernelHeap::Describe(KernelHeapError error) noexcept {
        switch (error) {
        case KernelHeapError::Success: return "success";
        case KernelHeapError::AlreadyInitialized: return "kernel heap is already initialized";
        case KernelHeapError::NotInitialized: return "kernel heap is not initialized";
        case KernelHeapError::InvalidDependency: return "kernel heap dependency is invalid";
        case KernelHeapError::InvalidRequest: return "kernel heap request is invalid";
        case KernelHeapError::OutputAlreadyOwnsAllocation: return "kernel heap output already contains an allocation";
        case KernelHeapError::VirtualAllocationFailed: return "failed to reserve kernel heap virtual address space";
        case KernelHeapError::PhysicalAllocationFailed: return "failed to allocate kernel heap physical backing";
        case KernelHeapError::MappingFailed: return "failed to map kernel heap backing";
        case KernelHeapError::ArenaExhausted: return "kernel heap virtual arena is exhausted";
        case KernelHeapError::InvalidPointer: return "pointer does not identify a live kernel heap allocation";
        case KernelHeapError::DoubleFree: return "kernel heap allocation was freed more than once";
        case KernelHeapError::CorruptHeap: return "kernel heap metadata is inconsistent";
        case KernelHeapError::RollbackFailed: return "failed to roll back kernel heap state";
        }
        return "unknown kernel heap error";
    }
}