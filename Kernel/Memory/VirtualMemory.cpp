#include <Kernel/Memory/VirtualMemory.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>

extern "C" {
    void* memset(void* dst, int v, unsigned long long n) noexcept;

    extern unsigned char __KernelImageStart[];
    extern unsigned char __KernelImageEnd[];

    extern unsigned char __TextStart[];
    extern unsigned char __TextEnd[];

    extern unsigned char __RodataStart[];
    extern unsigned char __RodataEnd[];

    extern unsigned char __DataStart[];
    extern unsigned char __DataEnd[];

    extern unsigned char __BssStart[];
    extern unsigned char __BssEnd[];
}

void* operator new(__SIZE_TYPE__, void* addr) noexcept { 
    return addr;
}

namespace Zos::Kernel::Memory {
    namespace {
        constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };

        [[nodiscard]] Uint64 SymbolAddress(const unsigned char* symbol) noexcept {
            return reinterpret_cast<Uint64>(symbol);
        }

        [[nodiscard]] Uint64 AlignDownToPage(Uint64 value) noexcept {
            return value & ~(PageSize - 1);
        }

        [[nodiscard]] bool TryAlignUpToPage(Uint64& value, Uint64& result) noexcept {
            const Uint64 mask = PageSize - 1;
            if (value > MaximumValue - mask) return false;
            result = (value + mask) & ~mask;
            return true;
        }

        [[nodiscard]] bool TryRangeEnd(Uint64 base, Uint64 size, Uint64& end) noexcept {
            if (size > MaximumValue - base) return false;
            end = base + size;
            return true;
        }

        [[nodiscard]] bool IsDirectMappedFirmwareType(Boot::FirmwareMemoryType type) noexcept {
            using Boot::FirmwareMemoryType;

            switch (type) {
            case FirmwareMemoryType::LoaderCode:
            case FirmwareMemoryType::LoaderData:
            case FirmwareMemoryType::BootServicesCode:
            case FirmwareMemoryType::BootServicesData:
            case FirmwareMemoryType::ConventionalMemory:
            case FirmwareMemoryType::AcpiReclaimMemory:
                return true;
            default:
                return false;
            }
        }
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

    void* BootstrapMetadataArena::PhysicalPointer(PhysicalAddress address) const noexcept {
        if (!m_DirectMapAccess) return reinterpret_cast<void*>(address.Value());
        if (!Layout::IsDirectMappable(address)) return nullptr;
        return reinterpret_cast<void*>(Layout::DirectMapAddress(address).Value());
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

        auto* header = static_cast<PageHeader*>(PhysicalPointer(page.Base()));
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

        auto* new_page = static_cast<PageHeader*>(PhysicalPointer(page.Base()));
        if (new_page == nullptr) {
            (void)m_PhysicalMemory->Release(page);
            return false;
        }

        InitializePage(new_page);

        const PhysicalAddress new_page_address = page.Base();
        auto* ownership = reinterpret_cast<PhysicalAllocation*>(reinterpret_cast<Uint8*>(m_CurrentPage) + ReservedOwnershipOffset());

        new (ownership) PhysicalAllocation(static_cast<PhysicalAllocation&&>(page));
        m_CurrentPage->Next = new_page_address;
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

    PhysicalAddress BootstrapMetadataArena::BackingPage(Uint64 index) const noexcept {
        if (!IsInitialized() || index >= m_Statistics.PageCount) return {};

        PhysicalAddress address = m_FirstPageAllocation.Base();
        for (Uint64 current = 0; current < index; current ++) {
            const auto* header = static_cast<const PageHeader*>(PhysicalPointer(address));
            if (header == nullptr || header->Next.IsNull()) return {};
            address = header->Next;
        }
        return address;
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

    bool KernelAddressSpace::MapIdentityBytes(Uint64 base, Uint64 size, MappingOptions options) noexcept {
        if (size == 0) return true;

        Uint64 end = 0;
        if (!TryRangeEnd(base, size, end)) return false;

        const Uint64 aligned_base = AlignDownToPage(base);
        Uint64 aligned_end = 0;
        if (!TryAlignUpToPage(end, aligned_end)) return false;

        const Uint64 page_count = (aligned_end - aligned_base) / PageSize;
        return m_PageMap->MapRange(VirtualAddress(aligned_base), PhysicalAddress(aligned_base), page_count, options) == Architecture::AMD64::MappingError::Success;
    }

    bool KernelAddressSpace::MapKernelSection(Uint64 start, Uint64 end, MappingOptions options) noexcept {
        using Architecture::AMD64::MappingError;

        if (end < start) return false;
        if ((start & (PageSize - 1)) != 0 || (end & (PageSize - 1)) != 0) return false;

        const Uint64 size = end - start;

        /*
         * Empty linker sections are valid. There is
         * simply nothing to place into the page map.
         */
        if (size == 0) return true;

        const Uint64 page_count = size / PageSize;
        return m_PageMap->MapRange(VirtualAddress(start), PhysicalAddress(start), page_count, options) == MappingError::Success;
    }

    bool KernelAddressSpace::MapKernelImage() noexcept {
        using Architecture::AMD64::MappingError;

        const MappingOptions text{
            .Access = PageAccess::Read | PageAccess::Execute | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions read_only{
            .Access = PageAccess::Read | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions writable{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        if (!MapKernelSection(SymbolAddress(__TextStart), SymbolAddress(__TextEnd), text)) return false;
        if (!MapKernelSection(SymbolAddress(__RodataStart), SymbolAddress(__RodataEnd), read_only)) return false;
        if (!MapKernelSection(SymbolAddress(__DataStart), SymbolAddress(__DataEnd), writable)) return false;
        if (!MapKernelSection(SymbolAddress(__BssStart), SymbolAddress(__BssEnd), writable)) return false;
        
        return true;
    }

    bool KernelAddressSpace::ValidateKernelLayout(const Boot::BootEnvironment& environment) const noexcept {
        const Uint64 image_start = SymbolAddress(__KernelImageStart);
        const Uint64 image_end = SymbolAddress(__KernelImageEnd);
        if (image_start != environment.KernelImage.Base) return false;

        Uint64 loaded_end = 0;
        if (!TryRangeEnd(environment.KernelImage.Base, environment.KernelImage.Size, loaded_end)) return false;
        if (image_end > loaded_end) return false;

        const Uint64 boundaries[] = {
            SymbolAddress(__TextStart),
            SymbolAddress(__TextEnd),
            SymbolAddress(__RodataStart),
            SymbolAddress(__RodataEnd),
            SymbolAddress(__DataStart),
            SymbolAddress(__DataEnd),
            SymbolAddress(__BssStart),
            SymbolAddress(__BssEnd),
        };

        for (Uint64 boundary : boundaries) if ((boundary & (PageSize - 1)) != 0) return false;

        return
            boundaries[0] <= boundaries[1] && boundaries[1] <= boundaries[2] &&
            boundaries[2] <= boundaries[3] && boundaries[3] <= boundaries[4] &&
            boundaries[4] <= boundaries[5] && boundaries[5] <= boundaries[6] &&
            boundaries[6] <= boundaries[7];
    }

    bool KernelAddressSpace::MapBootstrapRanges(const Boot::BootEnvironment& environment) noexcept {
        const MappingOptions writable{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        if (!MapIdentityBytes(environment.KernelStack.Base, environment.KernelStack.Size, writable)) return false;
        if (!MapIdentityBytes(environment.EnvironmentStorage.Base, environment.EnvironmentStorage.Size, writable)) return false;
        if (!MapIdentityBytes(environment.MemoryMapStorage.Base, environment.MemoryMapStorage.Size, writable)) return false;

        const PhysicalSpan pmm_metadata = m_PhysicalMemory->MetadataSpan();
        if (!MapIdentityBytes(pmm_metadata.Base.Value(), pmm_metadata.SizeBytes(), writable)) return false;
        return true;
    }

    KernelAddressSpaceError KernelAddressSpace::ValidateDirectMapCoverage(const Boot::BootEnvironment& environment) const noexcept {
        const Uint64 descriptor_count = environment.MemoryMapSize / environment.MemoryMapDescriptorSize;
        const auto* map = reinterpret_cast<const Uint8*>(environment.MemoryMapStorage.Base);

        for (Uint64 i = 0; i < descriptor_count; i++) {
            const auto* descriptor = reinterpret_cast<const Boot::FirmwareMemoryDescriptor*>(map + i * environment.MemoryMapDescriptorSize);
            if (!IsDirectMappedFirmwareType(descriptor->Type) || descriptor->NumPages == 0) continue;
            if (descriptor->NumPages > MaximumValue / PageSize) return KernelAddressSpaceError::InvalidBootEnvironment;

            const Uint64 size = descriptor->NumPages * PageSize;
            Uint64 end = 0;
            if (!TryRangeEnd(descriptor->PhysStart, size, end)) return KernelAddressSpaceError::InvalidBootEnvironment;

            if (end > Layout::DirectMapSize) return KernelAddressSpaceError::DirectMapTooSmall;
        }

        return KernelAddressSpaceError::Success;
    }

    [[nodiscard]] MappingOptions DirectMapOptionsFor(PhysicalAddress physical) noexcept {
        const Uint64 address = physical.Value();
        const Uint64 text_start = SymbolAddress(__TextStart);
        const Uint64 text_end = SymbolAddress(__TextEnd);
        const Uint64 rodata_start = SymbolAddress(__RodataStart);
        const Uint64 rodata_end = SymbolAddress(__RodataEnd);

        const bool protected_kernel_page = (address >= text_start && address < text_end) || (address >= rodata_start && address < rodata_end);

        if (protected_kernel_page) {
            return MappingOptions{
                .Access = PageAccess::Read | PageAccess::Global,
                .Cache = CachePolicy::WriteBack,
            };
        }

        return MappingOptions{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };
    }

    KernelAddressSpaceError KernelAddressSpace::MapDirectMemory(const Boot::BootEnvironment& environment) noexcept {
        using Architecture::AMD64::MappingError;

        const Uint64 descriptor_count = environment.MemoryMapSize / environment.MemoryMapDescriptorSize;
        const auto* map = reinterpret_cast<const Uint8*>(environment.MemoryMapStorage.Base);

        for (Uint64 i = 0; i < descriptor_count; i++) {
            const auto* descriptor = reinterpret_cast<const Boot::FirmwareMemoryDescriptor*>(map + i * environment.MemoryMapDescriptorSize);
            if (!IsDirectMappedFirmwareType(descriptor->Type)) continue;
            for (Uint64 page =0 ; page < descriptor->NumPages; page++) {
                const PhysicalAddress physical{ descriptor->PhysStart + page * PageSize };
                const VirtualAddress virtual_address = Layout::DirectMapAddress(physical);
                if (virtual_address.IsNull()) return KernelAddressSpaceError::DirectMapTooSmall;

                const MappingError error = m_PageMap->MapPage(virtual_address, physical, DirectMapOptionsFor(physical));
                if (error != MappingError::Success) return KernelAddressSpaceError::MappingFailed;
            }
        }

        return KernelAddressSpaceError::Success;
    }

    bool KernelAddressSpace::MapBootstrapMetadata() noexcept {
        const MappingOptions writable{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        for (;;) {
            const Uint64 before = m_Metadata->BackingPageCount();

            for (Uint64 i = 0; i < before; i++) {
                const PhysicalAddress page = m_Metadata->BackingPage(i);
                if (!EnsureIdentityPage(page, writable)) return false;
            }

            const Uint64 after = m_Metadata->BackingPageCount();

            if (after == before) return true;

            /*
             * Counts must only grow during bootstrap.
             * Also prevents pathological corruption from
             * causing an endless loop.
            */
           if (after < before || after > m_PhysicalMemory->Statistics().ManagedPages) return false;
        }
    }

    bool KernelAddressSpace::EnsureIdentityPage(PhysicalAddress page, MappingOptions options) noexcept {
        using Architecture::AMD64::MappingError;

        if (page.IsNull() || !page.IsPageAligned()) return false;

        const VirtualAddress virtual_address{ page.Value() };
        const MappingError error = m_PageMap->MapPage(virtual_address, page, options);
        if (error == MappingError::Success) return true;
        if (error != MappingError::AlreadyMapped) return false;

        const auto translation = m_PageMap->Translate(virtual_address);
        return translation.Mapped && translation.Physical == page && translation.Options.Access == options.Access && translation.Options.Cache == options.Cache;
    }

    [[nodiscard]] bool ValidateMapping(const Architecture::AMD64::PageMap& page_map, VirtualAddress virt_addr, PhysicalAddress phys_addr, MappingOptions options) noexcept {
        const auto translation = page_map.Translate(virt_addr);
        return translation.Mapped && translation.Physical == phys_addr && translation.Options.Access == options.Access && translation.Options.Cache == options.Cache;
    }

    bool KernelAddressSpace::ValidateKernelSection(Uint64 start, Uint64 end, MappingOptions options) const noexcept {
        if (end < start) return false;
        if ((start & (PageSize - 1)) != 0 || (end & (PageSize - 1)) != 0) return false;
        if (start == end) return true;

        const Uint64 page_count = (end - start) / PageSize;
        for (Uint64 page = 0; page < page_count; page++) {
            const Uint64 offset = page * PageSize;
            if (!ValidateMapping(*m_PageMap, VirtualAddress(start + offset), PhysicalAddress(start + offset), options)) return false;
        }
        return true;
    }

    bool KernelAddressSpace::ValidateMappings(const Boot::BootEnvironment& environment) const noexcept {
        const MappingOptions text{
            .Access = PageAccess::Read | PageAccess::Execute | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions read_only{
            .Access = PageAccess::Read | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingOptions writable{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        if (!ValidateKernelSection(SymbolAddress(__TextStart), SymbolAddress(__TextEnd), text)) return false;
        if (!ValidateKernelSection(SymbolAddress(__RodataStart), SymbolAddress(__RodataEnd), read_only)) return false;
        if (!ValidateKernelSection(SymbolAddress(__DataStart), SymbolAddress(__DataEnd), writable)) return false;
        if (!ValidateKernelSection(SymbolAddress(__BssStart), SymbolAddress(__BssEnd), writable)) return false;
        
        Uint64 rsp = 0;
        __asm__ volatile(
            "mov %%rsp, %0"
            : "=r"(rsp)
        );

        Uint64 stack_end = 0;
        if (!TryRangeEnd(environment.KernelStack.Base, environment.KernelStack.Size, stack_end)) return false;
        if (rsp < environment.KernelStack.Base || rsp >= stack_end) return false;

        const auto stack_translation = m_PageMap->Translate(VirtualAddress(rsp));
        if (!stack_translation.Mapped || stack_translation.Physical.Value() != rsp) return false;
        if (m_PageMap->IsMapped(VirtualAddress(0))) return false;
        if (m_PageMap->IsMapped(Layout::KernelDynamicBase)) return false;

        if (!HasAccess(stack_translation.Options.Access, PageAccess::Read) 
         || !HasAccess(stack_translation.Options.Access, PageAccess::Write) 
         || HasAccess(stack_translation.Options.Access, PageAccess::Execute)) return false;

        /*
         * PMM metadata must currently remain identity
         * accessible and also be direct-mapped.
         */
        const PhysicalAddress pmm_metadata = m_PhysicalMemory->MetadataSpan().Base;
        if (!m_PageMap->Translate(VirtualAddress(pmm_metadata.Value())).Mapped) return false;

        const auto pmm_direct = m_PageMap->Translate(Layout::DirectMapAddress(pmm_metadata));
        if (!pmm_direct.Mapped || pmm_direct.Physical != pmm_metadata) return false;

        /*
         * Every page-table page must be available
         * through the permanent direct map.
         */
        for (Uint64 i = 0; i < m_PageMap->TablePageCount(); i++) {
            const PhysicalAddress table = m_PageMap->TablePage(i);
            const auto translation = m_PageMap->Translate(Layout::DirectMapAddress(table));
            if (!translation.Mapped || translation.Physical != table) return false;
        }

        /*
         * Bootstrap arena pages currently need both
         * aliases because existing allocator records
         * contain identity-based raw pointers.
         */
        for (Uint64 i = 0; i < m_Metadata->BackingPageCount(); i++) {
            const PhysicalAddress page = m_Metadata->BackingPage(i);
            const auto identity = m_PageMap->Translate(VirtualAddress(page.Value()));
            const auto direct = m_PageMap->Translate(Layout::DirectMapAddress(page));
            if (!identity.Mapped || !direct.Mapped || identity.Physical != page || direct.Physical != page) return false;
        }
        
        (void)environment;
        return true;
    }

    KernelAddressSpaceError KernelAddressSpace::Build(const Boot::BootEnvironment& environment) noexcept {
        if (m_Built) return KernelAddressSpaceError::AlreadyBuilt;

        if (m_PhysicalMemory == nullptr || m_Metadata == nullptr || m_PageMap == nullptr || 
            !m_PhysicalMemory->IsInitialized() || !m_Metadata->IsInitialized() || !m_PageMap->IsInitialized() || m_PageMap->IsActive()) 
            return KernelAddressSpaceError::InvalidDependency;

        /*
        * The infrastructure self-test should have
        * returned this PageMap to root-only state.
        */
        if (m_PageMap->Statistics().MappedPages != 0 || m_PageMap->Statistics().TablePages != 1) 
            return KernelAddressSpaceError::InvalidDependency;
        
        if (!ValidateKernelLayout(environment))
            return KernelAddressSpaceError::UnsupportedKernelLoadModel;

        const KernelAddressSpaceError coverage = ValidateDirectMapCoverage(environment);
        if (coverage != KernelAddressSpaceError::Success)  return coverage;

        if (!MapKernelImage()) return KernelAddressSpaceError::MappingFailed;
        if (!MapBootstrapRanges(environment)) return KernelAddressSpaceError::MappingFailed;

        const KernelAddressSpaceError direct = MapDirectMemory(environment);
        if (direct != KernelAddressSpaceError::Success) return direct;

        /*
        * Do this after the direct map because mapping
        * it may have caused the arena to grow.
        */
        if (!MapBootstrapMetadata()) return KernelAddressSpaceError::MappingFailed;
        if (!ValidateMappings(environment)) return KernelAddressSpaceError::ValidationFailed;
        
        m_Environment = &environment;
        m_Built = true;

        return KernelAddressSpaceError::Success;
    }

    KernelAddressSpaceError KernelAddressSpace::Activate() noexcept {
        using Architecture::AMD64::PageMapActivationError;

        if (!m_Built || m_Environment == nullptr) return KernelAddressSpaceError::NotBuilt;
        if (m_PageMap->IsActive()) return KernelAddressSpaceError::AlreadyActive;

        /*
         * One final structural validation while we 
         * are still running under firmware mappings.
         */
        if (!ValidateMappings(*m_Environment)) return KernelAddressSpaceError::ValidationFailed;

        const PageMapActivationError error = m_PageMap->Activate();
        if (error != PageMapActivationError::Success) return KernelAddressSpaceError::ActivationFailed;

        /*
         * This second pass is significant:
         * Translate() now traverses tables through
         *  the direct map rather than identity access.
         */
        if (!ValidateMappings(*m_Environment)) return KernelAddressSpaceError::ValidationFailed;
        return KernelAddressSpaceError::Success;
    }

    bool KernelAddressSpace::IsActive() const noexcept {
        return m_PageMap != nullptr && m_PageMap->IsActive();
    }

    const char* KernelAddressSpace::Describe(KernelAddressSpaceError error) noexcept {
        switch (error) {
        case KernelAddressSpaceError::Success: return "success";
        case KernelAddressSpaceError::AlreadyBuilt: return "kernel address space already built";
        case KernelAddressSpaceError::NotBuilt: return "kernel address space not yet built";
        case KernelAddressSpaceError::AlreadyActive: return "kernel address space already active";
        case KernelAddressSpaceError::InvalidDependency: return "invalid dependency";
        case KernelAddressSpaceError::InvalidBootEnvironment: return "invalid boot environment";
        case KernelAddressSpaceError::UnsupportedKernelLoadModel: return "unsupported kernel load model";
        case KernelAddressSpaceError::InvalidKernelLayout: return "invalid kernel layout";
        case KernelAddressSpaceError::DirectMapTooSmall: return "direct map too small";
        case KernelAddressSpaceError::MappingFailed: return "mapping failed";
        case KernelAddressSpaceError::ValidationFailed: return "validation failure";
        case KernelAddressSpaceError::ActivationFailed: return "activation failure";
        }
        return "UNKNOWN ERROR";
    }
}