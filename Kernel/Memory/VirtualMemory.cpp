#include <Kernel/Memory/VirtualMemory.hpp>

extern "C" void* memset(void* dst, int v, unsigned long long n) noexcept;

void* operator new(__SIZE_TYPE__, void* addr) noexcept { 
    return addr;
}

namespace Zos::Kernel::Memory {
    namespace {
        constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };
    }

    bool BootstrapMetadataArena::IsPowerOfTwo(Uint64 value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool BootstrapMetadataArena::TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
        const Uint64 mask = alignment - 1;
        if (value > MaximumValue - mask) return false;
        result = (value + mask) & ~mask;
        return true;
    }

    Uint64 BootstrapMetadataArena::ReservedOwnershipOffset() noexcept {
        constexpr Uint64 alignment = alignof(PhysicalAllocation);
        return (PageSize - sizeof(PhysicalAllocation)) & ~(alignment - 1);
    }

    void* BootstrapMetadataArena::BootstrapPointer(PhysicalAddress address) noexcept {
        return reinterpret_cast<void*>(address.Value());
    }

    void BootstrapMetadataArena::InitializePage(PageHeader* page) noexcept {
        memset(page, 0, PageSize);
        page->Offset = sizeof(PageHeader);
    }

    void BootstrapMetadataArena::MoveTokenInto(PhysicalAllocation& dst, PhysicalAllocation& src) noexcept {
        dst.~PhysicalAllocation();
        new (&dst) PhysicalAllocation(static_cast<PhysicalAllocation&&>(src));
    }

    MetadataArenaInitializationError BootstrapMetadataArena::Initialize(PhysicalMemoryManager& physical_memory) noexcept {
        if (IsInitialized()) return MetadataArenaInitializationError::AlreadyInitialized;
        if (!physical_memory.IsInitialized()) return MetadataArenaInitializationError::PhysicalAllocationFailed;

        PhysicalAllocation page{};
        const PhysicalAllocationError allocation_error = physical_memory.AllocatePage(page);
        if (allocation_error != PhysicalAllocationError::Success) return MetadataArenaInitializationError::PhysicalAllocationFailed;

        if (page.Base().IsNull()) {
            (void)physical_memory.Release(page);
            return MetadataArenaInitializationError::AddressUnavailable;
        }

        auto* header = static_cast<PageHeader*>(BootstrapPointer(page.Base()));
        if (header == nullptr) {
            (void)physical_memory.Release(page);
            return MetadataArenaInitializationError::AddressUnavailable;
        }

        InitializePage(header);
        MoveTokenInto(m_FirstPageAllocation, page);

        m_PhysicalMemory = &physical_memory;
        m_FirstPage = header;
        m_CurrentPage = header;
        m_Statistics.PageCount = 1;
        return MetadataArenaInitializationError::Success;
    }

    bool BootstrapMetadataArena::Grow() noexcept {
        if (!IsInitialized() || m_CurrentPage == nullptr) return false;

        PhysicalAllocation page{};
        const PhysicalAllocationError error = m_PhysicalMemory->AllocatePage(page);
        if (error != PhysicalAllocationError::Success) return false;

        if (page.Base().IsNull()) {
            (void)m_PhysicalMemory->Release(page);
            return false;
        }

        auto* new_page = static_cast<PageHeader*>(BootstrapPointer(page.Base()));
        if (new_page == nullptr) {
            (void)m_PhysicalMemory->Release(page);
            return false;
        }

        InitializePage(new_page);

        auto* ownership = reinterpret_cast<PhysicalAllocation*>(reinterpret_cast<Uint8*>(m_CurrentPage) + ReservedOwnershipOffset());

        new (ownership) PhysicalAllocation(static_cast<PhysicalAllocation&&>(page));
        m_CurrentPage->Next = new_page;
        m_CurrentPage = new_page;
        m_Statistics.PageCount++;
        return true;
    }

    void* BootstrapMetadataArena::TryAllocateFromCurrent(Uint64 size, Uint64 alignment) noexcept {
        if (m_CurrentPage == nullptr) return nullptr;

        Uint64 aligned_offset = 0;
        if (!TryAlignUp(m_CurrentPage->Offset, alignment, aligned_offset)) return nullptr;

        const Uint64 usable_end = ReservedOwnershipOffset();
        if (aligned_offset > usable_end || size > usable_end - aligned_offset) return nullptr;

        auto* result = reinterpret_cast<Uint8*>(m_CurrentPage) + aligned_offset;
        m_CurrentPage->Offset = aligned_offset + size;
        m_Statistics.BytesRequested += size;
        memset(result, 0, size);
        return result;
    }

    void* BootstrapMetadataArena::Allocate(Uint64 size, Uint64 alignment) noexcept {
        if (!IsInitialized() || size == 0 || !IsPowerOfTwo(alignment)) return nullptr;

        if (alignment > PageSize || size > ReservedOwnershipOffset() - sizeof(PageHeader)) return nullptr;

        if (void* allocation = TryAllocateFromCurrent(size, alignment); allocation != nullptr) return allocation;

        if (!Grow()) return nullptr;

        return TryAllocateFromCurrent(size, alignment);
    }

    PhysicalAllocation* BootstrapMetadataArena::Retain(PhysicalAllocation&& allocation) noexcept {
        if (!allocation.IsValid()) return nullptr;
        
        void* storage = Allocate(sizeof(PhysicalAllocation), alignof(PhysicalAllocation));
        if (storage == nullptr) return nullptr;

        return new (storage) PhysicalAllocation(static_cast<PhysicalAllocation&&>(allocation));
    }

    const char* BootstrapMetadataArena::Describe(MetadataArenaInitializationError error) noexcept {
        switch (error) {
        case MetadataArenaInitializationError::Success: return "success";
        case MetadataArenaInitializationError::AlreadyInitialized: return "metadata arena is already initialized";
        case MetadataArenaInitializationError::PhysicalAllocationFailed: return "failed to allocate metadata storage";
        case MetadataArenaInitializationError::AddressUnavailable: return "metadata physical address is unavailable to bootstrap code";
        default: return "unknown metadata arena initialization error";
        }
    }

    VirtualReservation::VirtualReservation(VirtualReservation&& other) noexcept
        : m_Owner(other.m_Owner), m_Span(other.m_Span), m_ReleaseExtent(other.m_ReleaseExtent) { other.Invalidate(); }

    bool VirtualAddressAllocator::IsPowerOfTwo(Uint64 value) noexcept { 
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool VirtualAddressAllocator::TryRangeEnd(Uint64 base, Uint64 size, Uint64& end) noexcept {
        if (size > MaximumValue - base) return false;
        end = base + size;
        return true;
    }

    bool VirtualAddressAllocator::TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
        const Uint64 mask = alignment - 1;
        if (value > MaximumValue - mask) return false;
        result = (value + mask) & ~mask;
        return true;
    }

    Uint64 VirtualAddressAllocator::AlignDown(Uint64 value, Uint64 alignment) noexcept {
        return value & ~(alignment - 1);
    }

    VirtualAddressAllocator::FreeExtent* VirtualAddressAllocator::CreateExtentStorage() noexcept {
        if (m_Metadata == nullptr) return nullptr;

        void* storage = m_Metadata->Allocate(sizeof(FreeExtent), alignof(FreeExtent));
        if (storage == nullptr) return nullptr;

        return static_cast<FreeExtent*>(storage);
    }

    VirtualAddressAllocator::FreeExtent* VirtualAddressAllocator::AcquireExtent(Uint64 base, Uint64 page_count) noexcept {
        if (page_count == 0) return nullptr;

        FreeExtent* extent = m_RecycledExtents;
        if (extent != nullptr) m_RecycledExtents = extent->Next;
        else { 
            extent = CreateExtentStorage();
            if (extent == nullptr) return nullptr;
        }

        extent->Base = base;
        extent->PageCount = page_count;
        extent->Previous = nullptr;
        extent->Next = nullptr;
        return extent;
    }

    void VirtualAddressAllocator::RecycleExtent(FreeExtent& extent) noexcept {
        extent.Base = 0;
        extent.PageCount = 0;
        extent.Previous = 0;
        extent.Next = m_RecycledExtents;
        m_RecycledExtents = &extent;
    }

    void VirtualAddressAllocator::RemoveExtent(FreeExtent& extent) noexcept {
        if (extent.Previous != nullptr) extent.Previous->Next = extent.Next;
        else m_FreeHead = extent.Next;

        if (extent.Next != nullptr) extent.Next->Previous = extent.Previous;
        else m_FreeTail = extent.Previous;

        extent.Previous = nullptr;
        extent.Next = nullptr;

        if (m_Statistics.FreeExtentCount != 0) m_Statistics.FreeExtentCount--;
    }

    void VirtualAddressAllocator::InsertBefore(FreeExtent* position, FreeExtent& extent) noexcept {
        if (position == nullptr) {
            extent.Previous = m_FreeTail;
            extent.Next = nullptr;

            if (m_FreeTail != nullptr) m_FreeTail->Next = &extent;
            else m_FreeHead = &extent;

            m_FreeTail = &extent;
        } else {
            extent.Next = position;
            extent.Previous = position->Previous;

            if (position->Previous != nullptr) position->Previous->Next = &extent;
            else m_FreeHead = &extent;

            position->Previous = &extent;
        }

        m_Statistics.FreeExtentCount++;
    }

    void VirtualAddressAllocator::InsertAfter(FreeExtent* position, FreeExtent& extent) noexcept {
        if (position == nullptr) {
            InsertBefore(m_FreeHead, extent);
            return;
        }

        extent.Previous = position;
        extent.Next = position->Next;

        if (position->Next != nullptr) position->Next->Previous = &extent;
        else m_FreeTail = &extent;

        position->Next = &extent;
        m_Statistics.FreeExtentCount++;
    }

    VirtualAllocationError VirtualAddressAllocator::Initialize(VirtualSpan managed_range, BootstrapMetadataArena& metadata) noexcept {
        if (IsInitialized()) return VirtualAllocationError::AlreadyInitialized;
        if (!metadata.IsInitialized() || managed_range.IsEmpty() || !managed_range.Base.IsPageAligned()) return VirtualAllocationError::InvalidRequest;
        if (managed_range.PageCount > MaximumValue / PageSize) return VirtualAllocationError::InvalidRequest;

        Uint64 managed_end = 0;
        if (!TryRangeEnd(managed_range.Base.Value(), managed_range.SizeBytes(), managed_end)) return VirtualAllocationError::InvalidRequest;

        (void)managed_end;

        m_Metadata = &metadata;
        m_ManagedRange = managed_range;

        FreeExtent* initial = AcquireExtent(managed_range.Base.Value(), managed_range.PageCount);
        if (initial == nullptr) {
            m_Metadata = nullptr;
            m_ManagedRange = {};
            return VirtualAllocationError::OutOfMetadata;
        }

        m_FreeHead = initial;
        m_FreeTail = initial;
        m_Statistics.ManagedPages = managed_range.PageCount;
        m_Statistics.FreePages = managed_range.PageCount;
        m_Statistics.FreeExtentCount = 1;
        return VirtualAllocationError::Success;
    }

    bool VirtualAddressAllocator::FindLowCandidate(const FreeExtent& extent, Uint64 page_count, Uint64 alignment, Uint64& candidate) const noexcept {
        if (page_count > MaximumValue / PageSize) return false;

        const Uint64 allocation_size = page_count * PageSize;
        const Uint64 extent_size = extent.PageCount * PageSize;
        if (allocation_size > extent_size) return false;

        Uint64 extent_end = 0;
        if (!TryRangeEnd(extent.Base, extent_size, extent_end)) return false;
        if (!TryAlignUp(extent.Base, alignment, candidate)) return false;

        return candidate <= extent_end && allocation_size <= extent_end - candidate;
    }

    bool VirtualAddressAllocator::FindHighCandidate(const FreeExtent& extent, Uint64 pageCount, Uint64 alignment, Uint64& candidate) const noexcept {
        if (pageCount > MaximumValue / PageSize) return false;

        const Uint64 allocationSize = pageCount * PageSize;
        const Uint64 extentSize = extent.PageCount * PageSize;
        if (allocationSize > extentSize) return false;

        Uint64 extentEnd = 0;
        if (!TryRangeEnd(extent.Base, extentSize, extentEnd)) return false;

        const Uint64 latestBase = extentEnd - allocationSize;
        candidate = AlignDown(latestBase, alignment);
        return candidate >= extent.Base;
    }

    VirtualAllocationError VirtualAddressAllocator::ReserveFromExtent(FreeExtent& extent, Uint64 allocation_base, Uint64 page_count, VirtualReservation& output) noexcept {
        const Uint64 allocation_size = page_count * PageSize;
        const Uint64 extent_size = extent.PageCount * PageSize;
        const Uint64 extent_end = extent.Base + extent_size;
        const Uint64 allocation_end = allocation_base + allocation_size;
        if (allocation_base < extent.Base || allocation_end > extent_end) return VirtualAllocationError::CorruptReservation;

        const Uint64 prefix_bytes = allocation_base - extent.Base;
        const Uint64 suffix_bytes = extent_end - allocation_end;
        const Uint64 prefix_pages = prefix_bytes / PageSize;
        const Uint64 suffix_pages = suffix_bytes / PageSize;
        
        if (m_Statistics.FreePages < page_count) return VirtualAllocationError::CorruptReservation;

        FreeExtent* release_extent = AcquireExtent(allocation_base, page_count);
        if (release_extent == nullptr) return VirtualAllocationError::OutOfMetadata;

        FreeExtent* suffix = nullptr;
        if (prefix_pages != 0 && suffix_pages != 0) {
            suffix = AcquireExtent(allocation_end, suffix_pages);
            if (suffix == nullptr) {
                RecycleExtent(*release_extent);
                return VirtualAllocationError::OutOfMetadata;
            }
        }

        if (prefix_pages != 0 && suffix_pages != 0) {
            suffix->Previous = &extent;
            suffix->Next = extent.Next;
            if (extent.Next != nullptr) extent.Next->Previous = suffix;
            else m_FreeTail = suffix;

            extent.Next = suffix;
            extent.PageCount = prefix_pages;
            m_Statistics.FreeExtentCount++;
        } else if (prefix_pages != 0) {
            extent.PageCount = prefix_pages;
        } else if (suffix_pages != 0) {
            extent.Base = allocation_end;
            extent.PageCount = suffix_pages;
        } else {
            RemoveExtent(extent);
            RecycleExtent(extent);
        }

        m_Statistics.FreePages -= page_count;
        m_Statistics.ReservedPages += page_count;

        output.m_Owner = this;
        output.m_Span = VirtualSpan{ VirtualAddress(allocation_base), page_count };
        output.m_ReleaseExtent = release_extent;
        return VirtualAllocationError::Success;
    }

    VirtualAllocationError VirtualAddressAllocator::Reserve(Uint64 page_count, VirtualReservation& output, VirtualAllocationConstraints constraints) noexcept {
        if (!IsInitialized()) return VirtualAllocationError::NotInitialized;
        if (output.IsValid()) return VirtualAllocationError::OutputAlreadyOwnsRange;
        if (page_count == 0 || page_count > MaximumValue / PageSize || constraints.Alignment < PageSize || !IsPowerOfTwo(constraints.Alignment) || (constraints.Alignment % PageSize) != 0) 
            return VirtualAllocationError::InvalidRequest;

        if (constraints.Preference == VirtualAllocationPreference::LowAddresses) 
            for (FreeExtent* extent = m_FreeHead; extent != nullptr; extent = extent->Next) {
                Uint64 candidate = 0;
                if (!FindLowCandidate(*extent, page_count, constraints.Alignment, candidate)) continue;
                return ReserveFromExtent(*extent, candidate, page_count, output);
            }
        else 
            for (FreeExtent* extent = m_FreeTail; extent != nullptr; extent = extent->Previous) {
                Uint64 candidate = 0;
                if (!FindHighCandidate(*extent, page_count, constraints.Alignment, candidate)) continue;
                return ReserveFromExtent(*extent, candidate, page_count, output);
            }
        
        return VirtualAllocationError::OutOfAddressSpace;
    }

    bool VirtualAddressAllocator::ReservationInsideManagedRange(VirtualSpan span) const noexcept {
        if (span.IsEmpty() || !span.Base.IsPageAligned() || span.PageCount > MaximumValue / PageSize) return false;

        Uint64 managed_end = 0;
        Uint64 span_end = 0;
        if (!TryRangeEnd(m_ManagedRange.Base.Value(), m_ManagedRange.SizeBytes(), managed_end) 
         || !TryRangeEnd(span.Base.Value(), span.SizeBytes(), span_end)) return false;

        return span.Base.Value() >= m_ManagedRange.Base.Value() && span_end <= managed_end;
    }

    VirtualAllocationError VirtualAddressAllocator::Release(VirtualReservation& reservation) noexcept {
        if (!IsInitialized()) return VirtualAllocationError::NotInitialized;
        if (!reservation.IsValid() || reservation.m_ReleaseExtent == nullptr) return VirtualAllocationError::CorruptReservation;
        if (reservation.m_Owner != this) return VirtualAllocationError::WrongOwner;
        if (!ReservationInsideManagedRange(reservation.m_Span) || m_Statistics.ReservedPages < reservation.m_Span.PageCount) 
            return VirtualAllocationError::CorruptReservation;

        const Uint64 base = reservation.m_Span.Base.Value();
        const Uint64 page_count = reservation.m_Span.PageCount;
        const Uint64 size = reservation.m_Span.SizeBytes();
        const Uint64 end = base + size;
        auto* release_extent = static_cast<FreeExtent*>(reservation.m_ReleaseExtent);

        FreeExtent* position = m_FreeHead;
        while (position != nullptr && position->Base < base) position = position->Next;

        FreeExtent* previous = position != nullptr ? position->Previous : m_FreeTail;
        if (previous != nullptr) {
            const Uint64 previous_end = previous->Base + previous->PageCount * PageSize;
            if (base < previous_end) return VirtualAllocationError::CorruptReservation;
        }

        if (position != nullptr && end > position->Base) 
            return VirtualAllocationError::CorruptReservation;

        const bool merge_previous = previous != nullptr && previous->Base + previous->PageCount * PageSize == base;
        const bool merge_next = position != nullptr && end == position->Base;

        if (merge_previous && merge_next) {
            previous->PageCount += page_count + position->PageCount;
            RemoveExtent(*position);
            RecycleExtent(*position);
            RecycleExtent(*release_extent);
        } else if (merge_previous) {
            previous->PageCount += page_count;
            RecycleExtent(*release_extent);
        } else if (merge_next) {
            position->Base = base;
            position->PageCount += page_count;
            RecycleExtent(*release_extent);
        } else {
            release_extent->Base = base;
            release_extent->PageCount = page_count;
            release_extent->Next = nullptr;
            release_extent->Previous = nullptr;
            InsertBefore(position, *release_extent);
        }

        m_Statistics.FreePages += page_count;
        m_Statistics.ReservedPages -= page_count;
        reservation.Invalidate();
        return VirtualAllocationError::Success;
    }

    const char* VirtualAddressAllocator::Describe(VirtualAllocationError error) noexcept {
        switch (error) {
        case VirtualAllocationError::Success: return "success";
        case VirtualAllocationError::AlreadyInitialized: return "virtual address allocator is already initialized";
        case VirtualAllocationError::NotInitialized: return "virtual address allocator is not initialized";
        case VirtualAllocationError::InvalidRequest: return "invalid virtual address allocation request";
        case VirtualAllocationError::OutputAlreadyOwnsRange: return "output already owns a virtual range";
        case VirtualAllocationError::OutOfAddressSpace: return "virtual address space is exhausted";
        case VirtualAllocationError::OutOfMetadata: return "virtual address allocator metadata is exhausted";
        case VirtualAllocationError::WrongOwner: return "virtual reservation belongs to another allocator";
        case VirtualAllocationError::CorruptReservation: return "virtual reservation state is invalid";
        default: return "unknown virtual allocation error";
        }
    }
}