#include <Kernel/Memory/MMIO.hpp>

namespace Zos::Kernel::Memory {
    using Architecture::AMD64::MappingError;

    MmioMappingError MmioMapping::Initialize(VirtualAddressAllocator& addresses, Architecture::AMD64::PageMap& page_map, PhysicalAddress physical_base, Uint64 page_count) noexcept {
        if (IsMapped()) return MmioMappingError::AlreadyMapped;

        if (!addresses.IsInitialized() || !addresses.IsMetadataPromoted() ||
            !page_map.IsInitialized() || !page_map.IsActive())
            return MmioMappingError::InvalidDependency;

        if (!physical_base.IsPageAligned() || page_count == 0 || page_count > MaximumValue / PageSize)
            return MmioMappingError::InvalidRange;

        const Uint64 size = page_count * PageSize;
        if (physical_base.Value() > MaximumValue - (size - PageSize))
            return MmioMappingError::InvalidRange;

        const VirtualAllocationError reservation_error = addresses.Reserve(page_count, m_Reservation);
        if (reservation_error != VirtualAllocationError::Success)
            return MmioMappingError::VirtualReservationFailed;

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::Uncached,
        };

        const MappingError mapping_error = page_map.MapRange(m_Reservation.Base(), physical_base, page_count, options);
        if (mapping_error != MappingError::Success) {
            if (addresses.Release(m_Reservation) != VirtualAllocationError::Success)
                return MmioMappingError::VirtualReleaseFailed;
            return MmioMappingError::MappingFailed;
        }

        m_Addresses = &addresses;
        m_PageMap = &page_map;
        m_PhysicalBase = physical_base;
        m_PageCount = page_count;

        return MmioMappingError::Success;
    }

    MmioMappingError MmioMapping::Release() noexcept {
        if (!IsMapped() || m_Addresses == nullptr || m_PageMap == nullptr)
            return MmioMappingError::InvalidDependency;

        for (Uint64 page = 0; page < m_PageCount; page++) {
            const VirtualAddress address = m_Reservation.Base() + page * PageSize;
            if (m_PageMap->UnmapPage(address) != MappingError::Success)
                return MmioMappingError::UnmappingFailed;
        }

        if (m_Addresses->Release(m_Reservation) != VirtualAllocationError::Success)
            return MmioMappingError::VirtualReleaseFailed;

        m_Addresses = nullptr;
        m_PageMap = nullptr;
        m_PhysicalBase = {};
        m_PageCount = 0;

        return MmioMappingError::Success;
    }

    const char* MmioMapping::Describe(MmioMappingError error) noexcept {
        switch (error) {
        case MmioMappingError::Success: return "success";
        case MmioMappingError::AlreadyMapped: return "MMIO mapping is already initialized";
        case MmioMappingError::InvalidDependency: return "MMIO mapping dependencies are invalid";
        case MmioMappingError::InvalidRange: return "MMIO physical range is invalid";
        case MmioMappingError::VirtualReservationFailed: return "failed to reserve kernel MMIO virtual address space";
        case MmioMappingError::MappingFailed: return "failed to map MMIO physical pages";
        case MmioMappingError::UnmappingFailed: return "failed to unmap MMIO virtual pages";
        case MmioMappingError::VirtualReleaseFailed: return "failed to release kernel MMIO virtual address space";
        default: return "unknown MMIO mapping error";
        }
    }
}