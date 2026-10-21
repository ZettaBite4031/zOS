#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>

namespace Zos::Kernel::Memory {
    enum class MmioMappingError : Uint32 {
        Success,
        AlreadyMapped,
        InvalidDependency,
        InvalidRange,
        VirtualReservationFailed,
        MappingFailed,
        UnmappingFailed,
        VirtualReleaseFailed,
    };

    class MmioMapping final {
    public:
        constexpr MmioMapping() noexcept = default;

        MmioMapping(const MmioMapping&) = delete;
        MmioMapping& operator=(const MmioMapping&) = delete;

        [[nodiscard]] MmioMappingError Initialize(
            VirtualAddressAllocator& addresses,
            Architecture::AMD64::PageMap& page_map,
            PhysicalAddress physical_base,
            Uint64 page_count
        ) noexcept;

        [[nodiscard]] MmioMappingError Release() noexcept;

        [[nodiscard]] bool IsMapped() const noexcept { return m_Reservation.IsValid(); }
        [[nodiscard]] VirtualAddress Base() const noexcept { return m_Reservation.Base(); }
        [[nodiscard]] PhysicalAddress PhysicalBase() const noexcept { return m_PhysicalBase; }
        [[nodiscard]] Uint64 PageCount() const noexcept { return m_PageCount; }
        [[nodiscard]] Uint64 SizeBytes() const noexcept { return m_PageCount * PageSize; }

        [[nodiscard]] static const char* Describe(MmioMappingError error) noexcept;

    private:
        VirtualAddressAllocator* m_Addresses{};
        Architecture::AMD64::PageMap* m_PageMap{};

        VirtualReservation m_Reservation{};
        PhysicalAddress m_PhysicalBase{};
        Uint64 m_PageCount{};
    };
}