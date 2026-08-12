#pragma once

#include <Boot/Protocol.hpp>
#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Memory {
    enum class PhysicalMemoryInitializationError : Uint32 {
        Success,
        AlreadyInitialized,
        InvalidBootEnvironment,
        InvalidMemoryMap,
        DescriptorRangeOverflow,
        DescriptorRangesOverlap,
        NoUsableMemory,
        MetadataSizeOverflow,
        NoMetadataRegion,
    };

    enum class PhysicalAllocationError : Uint32 {
        Success,
        NotInitialized,
        InvalidRequest,
        OutputAlreadyOwnsMemory,
        OutOfMemory,
        WrongOwner,
        CorruptAllocation,
    };

    enum class PhysicalMemoryReclamationError : Uint32 {
        Success,
        NotInitialized,
        AlreadyReclaimed,
        CorruptState,
    };

    enum class PhysicalAllocationPreference : Uint32 {
        HighAddresses,
        LowAddresses,
    };

    struct PhysicalMemoryReclamationResult final {
        Uint64 ReclaimedPages{};
        [[nodiscard]] Uint64 ReclaimedBytes() const noexcept { return ReclaimedPages * PageSize; }
    };

    struct PhysicalAllocationConstraints final {
        PhysicalAddress MinimumAddress{ PhysicalAddress(PageSize) };
        PhysicalAddress MaximumAddress{ PhysicalAddress(~Uint64{ 0 }) };
        Uint64 Alignment{ PageSize };
        PhysicalAllocationPreference Preference{ PhysicalAllocationPreference::HighAddresses };

        [[nodiscard]] static constexpr PhysicalAllocationConstraints General() noexcept { return {}; }
        [[nodiscard]] static constexpr PhysicalAllocationConstraints Dma32() noexcept { 
            PhysicalAllocationConstraints constraints{};
            constraints.MaximumAddress = PhysicalAddress(Dma32AddressLimit);
            return constraints;
        }
    };

    struct PhysicalMemoryStatistics final {
        Uint64 ManagedPages{};
        Uint64 ConventionalPages{};
        Uint64 FreePages{};
        Uint64 AllocatedPages{};
        Uint64 DeferredBootPages{};
        Uint64 DeferredAcpiPages{};
        Uint64 MetadataBytes{};
        Uint64 ManagedRegionCount{};

        [[nodiscard]] constexpr Uint64 ReservedPages() const noexcept { 
            return ManagedPages - FreePages - AllocatedPages - DeferredBootPages - DeferredAcpiPages;
        }

        [[nodiscard]] constexpr Uint64 ReservedBytes() const noexcept { return ReservedPages() * PageSize; }

        [[nodiscard]] constexpr Uint64 ConventionalBytes() const noexcept { return ConventionalPages * PageSize; }
        [[nodiscard]] constexpr Uint64 FreeBytes() const noexcept { return FreePages * PageSize; }
        [[nodiscard]] constexpr Uint64 AllocatedBytes() const noexcept { return AllocatedPages * PageSize; }
        [[nodiscard]] constexpr Uint64 DeferredBootBytes() const noexcept { return DeferredBootPages * PageSize; }
        [[nodiscard]] constexpr Uint64 DeferredAcpiBytes() const noexcept { return DeferredAcpiPages * PageSize; }
    };

    class PhysicalMemoryManager;

    // An ownership token, not merely an address. It cannot be copied or forged
    // outside the PMM. Release() consumes it, preventing normal double/partial
    // frees and accidental cross-manager releases.
    class PhysicalAllocation final {
    public:
        constexpr PhysicalAllocation() noexcept = default;
        PhysicalAllocation(const PhysicalAllocation&) = delete;
        PhysicalAllocation& operator=(const PhysicalAllocation&) = delete;

        PhysicalAllocation(PhysicalAllocation&& other) noexcept;
        PhysicalAllocation& operator=(PhysicalAllocation&&) = delete;

        [[nodiscard]] bool IsValid() const noexcept { return m_Owner != nullptr && !m_Span.IsEmpty(); }
        [[nodiscard]] PhysicalAddress Base() const noexcept { return m_Span.Base; }
        [[nodiscard]] Uint64 PageCount() const noexcept { return m_Span.PageCount; }
        [[nodiscard]] Uint64 SizeBytes() const noexcept { return m_Span.SizeBytes(); }
        [[nodiscard]] PhysicalSpan Span() const noexcept { return m_Span; }
        
    private:
        friend class PhysicalMemoryManager;

        void Invalidate() noexcept { 
            m_Owner = nullptr;
            m_Span = {};
        }

        PhysicalMemoryManager* m_Owner{};
        PhysicalSpan m_Span{};
    };

    class PhysicalMemoryManager final {
    public:
        constexpr PhysicalMemoryManager() noexcept = default;
        PhysicalMemoryManager(const PhysicalMemoryManager&) = delete;
        PhysicalMemoryManager& operator=(const PhysicalMemoryManager&) = delete;

        [[nodiscard]] PhysicalMemoryInitializationError Initialize(const Boot::BootEnvironment_V1& environment) noexcept;
        [[nodiscard]] PhysicalMemoryReclamationError ReclaimBootMemory(PhysicalMemoryReclamationResult& result) noexcept;

        [[nodiscard]] PhysicalAllocationError AllocatePage(PhysicalAllocation& output, PhysicalAllocationConstraints constraints = PhysicalAllocationConstraints::General()) noexcept;
        [[nodiscard]] PhysicalAllocationError AllocateContiguous(Uint64 page_count, PhysicalAllocation& output, PhysicalAllocationConstraints constraints = PhysicalAllocationConstraints::General()) noexcept;
        [[nodiscard]] PhysicalAllocationError Release(PhysicalAllocation& allocation) noexcept;
        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] const PhysicalMemoryStatistics& Statistics() const noexcept { return m_Statistics; }
        [[nodiscard]] PhysicalSpan MetadataSpan() const noexcept { return m_MetadataSpan; }
        [[nodiscard]] bool IsBootMemoryReclaimed() const noexcept { return m_BootMemoryReclaimed; }
        
        [[nodiscard]] static const char* Describe(PhysicalMemoryReclamationError error) noexcept;
        [[nodiscard]] static const char* Describe(PhysicalMemoryInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(PhysicalAllocationError error) noexcept;

    private:
        // One byte per managed page is intentional. It costs ~0.024% of the RAM
        // represented while letting us distinguish ownership states that must not
        // be conflated for safety.
        enum class PageState : Uint8 {
            Reserved = 0,
            Free = 1,
            Allocated = 2,
            DeferredBoot = 3,
            DeferredAcpi = 4,
            Corrupt = 0xFF,
        };

        struct ManagedRegion final {
            Uint64 Base{};
            Uint64 PageCount{};
            Uint64 StateOffset{};
            PageState InitialState{ PageState::Reserved };
            Uint8 Reserved[7];
        };

        static_assert(sizeof(ManagedRegion) == 32);

        [[nodiscard]] static bool IsPowerOfTwo(Uint64 value) noexcept;
        [[nodiscard]] static bool IsManagedType(Boot::FirmwareMemoryType type) noexcept;
        [[nodiscard]] static PageState InitialStateFor(Boot::FirmwareMemoryType type) noexcept;
        [[nodiscard]] static bool TryRangeEnd(Uint64 base, Uint64 size, Uint64& end) noexcept;
        [[nodiscard]] static bool TryDescriptorEnd(const Boot::FirmwareMemoryDescriptor& descriptor, Uint64& end) noexcept;
        [[nodiscard]] static bool Overlaps(Uint64 left_base, Uint64 left_end, Uint64 right_base, Uint64 right_end) noexcept;
        [[nodiscard]] static Uint64 AlignDown(Uint64 value, Uint64 alignment) noexcept;
        [[nodiscard]] static bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept;

        [[nodiscard]] const Boot::FirmwareMemoryDescriptor* DescriptorAt(const Boot::BootEnvironment_V1& environment, Uint64 index) const noexcept;

        [[nodiscard]] PhysicalMemoryInitializationError ValidateMemoryMap(const Boot::BootEnvironment_V1& environment, Uint64 descriptor_count, Uint64& managed_pages, Uint64& managed_region_capacity, Uint64& conventional_pages) const noexcept;
        
        [[nodiscard]] bool FindMetadataRegion(const Boot::BootEnvironment_V1& environment, Uint64 descriptor_count, Uint64 metadata_page_count, PhysicalSpan& result) const noexcept;

        [[nodiscard]] bool CandidateOverlapsProtectedRange(const Boot::BootEnvironment_V1& environment, Uint64 candidate_base, Uint64 candidate_end, Uint64& next_candidate_end) const noexcept;

        void InitializeMetadataStorage(Uint64 managed_region_capcity, Uint64 managed_pages) noexcept;
        void BuildManagedRegions(const Boot::BootEnvironment_V1& environment, Uint64 descriptor_count, Uint64 managed_region_capacity) noexcept;
        void SortManagedRegions() noexcept;
        void MergedAdjacentManagedRegions() noexcept;
        void AssignStateOffsets() noexcept;
        void InitializePageStates() noexcept;

        void ReserveBootOwnedRanges(const Boot::BootEnvironment_V1& environment) noexcept;
        void ReserveSpan(PhysicalSpan span) noexcept;
        void AccountStateRemoval(PageState state) noexcept;

        [[nodiscard]] PageState GetState(const ManagedRegion& region, Uint64 page_offset) const noexcept;
        void SetState(const ManagedRegion& region, Uint64 page_offset, PageState state) noexcept;
        [[nodiscard]] bool RangeIsFree(const ManagedRegion& region, Uint64 first_page_offset, Uint64 page_count) const noexcept;
        void MarkAllocated(const ManagedRegion& region, Uint64 first_page_offset, Uint64 page_count) noexcept;
        void MarkFree(const ManagedRegion& region, Uint64 first_page_offset, Uint64 page_count) noexcept;

        [[nodiscard]] bool ValidateConstraints(Uint64 page_count, const PhysicalAllocationConstraints& constraints, Uint64& allocation_size, Uint64& minimum_address, Uint64& maximum_base_address, Uint64& alignment) const noexcept;

        [[nodiscard]] bool FindFreeRangeInRegionLow(const ManagedRegion& region, Uint64 pageCount, Uint64 allocation_size, Uint64 minimum_address, Uint64 maximum_base_address, Uint64 alignment, Uint64& firstPhysicalPage) const noexcept;

        [[nodiscard]] bool FindFreeRangeInRegionHigh(const ManagedRegion& region, Uint64 pageCount, Uint64 allocation_size, Uint64 minimum_address, Uint64 maximum_base_address, Uint64 alignment, Uint64& firstPhysicalPage) const noexcept;

        [[nodiscard]] const ManagedRegion* FindContainingRegion(PhysicalSpan span) const noexcept;

        ManagedRegion* m_Regions{ };
        Uint8* m_PageStates{};
        Uint64 m_RegionCount{};
        PhysicalSpan m_MetadataSpan{};
        PhysicalMemoryStatistics m_Statistics{};
        bool m_Initialized{};
        bool m_BootMemoryReclaimed{};
    };
}