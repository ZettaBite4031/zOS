#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Memory {
    namespace {
        constexpr Uint64 MaximumValue{ ~Uint64{ 0 } };

        [[nodiscard]] Uint64 Minimum(Uint64 left, Uint64 right) noexcept {
            return left < right ? left : right;
        }

        [[nodiscard]] Uint64 Maximum(Uint64 left, Uint64 right) noexcept {
            return left > right ? left : right;
        }

        // The current handoff already sets RSP to a loader-allocated physical
        // address, so the pre-VMM contract necessarily relies on the firmware's
        // identity mapping for RAM. Keep the pa->pointer bridge isolated here.
        // The VMM will replace this with an explicit direct physical mapping.
        [[nodiscard]] Uint8* BootstrapIdentityPointer(PhysicalAddress address) noexcept {
            return reinterpret_cast<Uint8*>(address.Value());
        }
    }
    
    PhysicalAllocation::PhysicalAllocation(PhysicalAllocation&& other) noexcept
        : m_Owner(other.m_Owner), m_Span(other.m_Span) { other.Invalidate(); }

    bool PhysicalMemoryManager::IsPowerOfTwo(Uint64 value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool PhysicalMemoryManager::IsManagedType(Boot::FirmwareMemoryType type) noexcept {
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

    PhysicalMemoryManager::PageState PhysicalMemoryManager::InitialStateFor(Boot::FirmwareMemoryType type) noexcept {
        using Boot::FirmwareMemoryType;

        switch (type) {
        case FirmwareMemoryType::ConventionalMemory:
            return PageState::Free;
        case FirmwareMemoryType::LoaderCode:
        case FirmwareMemoryType::LoaderData:
        case FirmwareMemoryType::BootServicesCode:
        case FirmwareMemoryType::BootServicesData:
            return PageState::DeferredBoot;
        case FirmwareMemoryType::AcpiReclaimMemory:
            return PageState::DeferredAcpi;
        default:
            return PageState::Reserved;
        }
    }

    bool PhysicalMemoryManager::TryRangeEnd(Uint64 base, Uint64 size, Uint64& end) noexcept {
        if (size > MaximumValue - base) return false;
        end = base + size;
        return true;
    }

    bool PhysicalMemoryManager::TryDescriptorEnd(const Boot::FirmwareMemoryDescriptor& descriptor, Uint64& end) noexcept {
        if (descriptor.NumPages > MaximumValue / PageSize) return false;
        return TryRangeEnd(descriptor.PhysStart, descriptor.NumPages * PageSize, end);
    }

    bool PhysicalMemoryManager::Overlaps(Uint64 left_base, Uint64 left_end, Uint64 right_base, Uint64 right_end) noexcept {
        return left_base < right_end && right_base < left_end;
    }

    Uint64 PhysicalMemoryManager::AlignDown(Uint64 value, Uint64 alignment) noexcept {
        return value & ~(alignment - 1);
    }

    bool PhysicalMemoryManager::TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
        const Uint64 mask = alignment - 1;
        if (value > MaximumValue - mask) return false;
        result = (value + mask) & ~mask;
        return true;
    }

    const Boot::FirmwareMemoryDescriptor* PhysicalMemoryManager::DescriptorAt(const Boot::BootEnvironment& environment, Uint64 index) const noexcept {
        const auto* bytes = reinterpret_cast<const Uint8*>(environment.MemoryMapStorage.Base);
        return reinterpret_cast<const Boot::FirmwareMemoryDescriptor*>(bytes + (index * environment.MemoryMapDescriptorSize));
    }

    PhysicalMemoryInitializationError PhysicalMemoryManager::ValidateMemoryMap(const Boot::BootEnvironment& environment, Uint64 descriptor_count, Uint64& managed_pages, Uint64& managed_region_capacity, Uint64& conventional_pages) const noexcept {
        managed_pages = 0;
        managed_region_capacity = 0;
        conventional_pages = 0;

        for (Uint64 i = 0; i < descriptor_count; i++) {
            const Boot::FirmwareMemoryDescriptor& descriptor = *DescriptorAt(environment, i);
            if ((descriptor.PhysStart & (PageSize - 1)) != 0) 
                return PhysicalMemoryInitializationError::InvalidMemoryMap;

            Uint64 descriptor_end = 0;
            if (!TryDescriptorEnd(descriptor, descriptor_end))
                return PhysicalMemoryInitializationError::DescriptorRangeOverflow;

            if (descriptor.NumPages == 0) continue;

            for (Uint64 j = i + 1; j < descriptor_count; j++) {
                const Boot::FirmwareMemoryDescriptor& other = *DescriptorAt(environment, j);
                if (other.NumPages == 0) continue;

                Uint64 other_end = 0;
                if (!TryDescriptorEnd(other, other_end)) 
                    return PhysicalMemoryInitializationError::DescriptorRangeOverflow;

                if (Overlaps(descriptor.PhysStart, descriptor_end, other.PhysStart, other_end))
                    return PhysicalMemoryInitializationError::DescriptorRangesOverlap;
            }

            if (!IsManagedType(descriptor.Type)) continue;

            if (managed_pages > MaximumValue - descriptor.NumPages)
                return PhysicalMemoryInitializationError::MetadataSizeOverflow;

            managed_pages += descriptor.NumPages;
            managed_region_capacity++;

            if (descriptor.Type == Boot::FirmwareMemoryType::ConventionalMemory) {
                if (conventional_pages > MaximumValue - descriptor.NumPages) 
                    return PhysicalMemoryInitializationError::MetadataSizeOverflow;
                conventional_pages += descriptor.NumPages;
            }
        }

        if (managed_pages == 0 || managed_region_capacity == 0 || conventional_pages == 0)
            return PhysicalMemoryInitializationError::NoUsableMemory;

        return PhysicalMemoryInitializationError::Success;
    }

    bool PhysicalMemoryManager::CandidateOverlapsProtectedRange(const Boot::BootEnvironment& environment, Uint64 candidate_base, Uint64 candidate_end, Uint64& next_candidate_end) const noexcept {
        const Boot::PhysicalRange ranges[] = {
            environment.KernelImage, environment.KernelStack,
            environment.EnvironmentStorage, environment.MemoryMapStorage,
        };

        bool overlap_found = false;
        next_candidate_end = candidate_end;

        for (const Boot::PhysicalRange& range : ranges) {
            if (range.Size == 0) continue;

            Uint64 range_end = 0;
            if (!TryRangeEnd(range.Base, range.Size, range_end)) return true;
            if (!Overlaps(candidate_base, candidate_end, range.Base, range_end)) continue;

            overlap_found = true;
            const Uint64 before_range = AlignDown(range.Base, PageSize);
            if (before_range < next_candidate_end) next_candidate_end = before_range;
        }

        if (Overlaps(candidate_base, candidate_end, 0, PageSize)) {
            overlap_found = true;
            next_candidate_end = 0;
        }

        return overlap_found;
    }

    bool PhysicalMemoryManager::MetadataViewsMatch(const ManagedRegion* candidate_regions, const Uint8* candidate_page_states) const noexcept {
        if (candidate_regions == nullptr ||
            candidate_page_states == nullptr ||
            m_Regions == nullptr || 
            m_PageStates == nullptr) 
            return false;

        /*
         * Compare the logical ManagedRegion contents rather than raw
         * structure bytes. Padding/reserved storage is not part of the
         * allocator's semantic state.
         */
        for (Uint64 i = 0; i < m_RegionCount; i++) {
            const ManagedRegion& current = m_Regions[i];
            const ManagedRegion& candidate = candidate_regions[i];
            if (candidate.Base != current.Base ||
                candidate.PageCount != current.PageCount ||
                candidate.StateOffset != current.StateOffset ||
                candidate.InitialState != current.InitialState)
                return false;
        }

        /*
         * Every managed physical page has exactly one state byte.
         */
        for (Uint64 page = 0; page < m_Statistics.ManagedPages; page++) 
            if (candidate_page_states[page] != m_PageStates[page]) return false;

        return true;
    }

    bool PhysicalMemoryManager::FindMetadataRegion(const Boot::BootEnvironment& environment, Uint64 descriptor_count, Uint64 metadata_page_count, PhysicalSpan& result) const noexcept {
        if (metadata_page_count == 0 || metadata_page_count > MaximumValue / PageSize) return false;

        const Uint64 metadata_size = metadata_page_count * PageSize;
        bool found = false;
        Uint64 best_base = 0;

        // Preserve lower physical memory for constrainted DMA allocations by
        // preferring the highest safe ConventionalMemory region.
        for (Uint64 i = 0; i < descriptor_count; i++) {
            const Boot::FirmwareMemoryDescriptor& descriptor = *DescriptorAt(environment, i);
            if (descriptor.Type != Boot::FirmwareMemoryType::ConventionalMemory || descriptor.NumPages == 0) continue;

            Uint64 descriptor_end = 0;
            if (!TryDescriptorEnd(descriptor, descriptor_end)) continue;

            Uint64 candidate_end = descriptor_end;
            while (candidate_end >= descriptor.PhysStart && candidate_end - descriptor.PhysStart >= metadata_size) {
                const Uint64 candidate_base = AlignDown(candidate_end - metadata_size, PageSize);
                if (candidate_base < descriptor.PhysStart) break;

                const Uint64 actual_end = candidate_base + metadata_size;
                Uint64 next_candidate_end = actual_end;
                if (!CandidateOverlapsProtectedRange(environment, candidate_base, actual_end, next_candidate_end)) {
                    if (!found || candidate_base > best_base) {
                        found = true;
                        best_base = candidate_base;
                    }
                    break;
                }

                if (next_candidate_end <= descriptor.PhysStart || next_candidate_end >= candidate_end) break;

                candidate_end = next_candidate_end;
            }
        }

        if (!found) return false;

        result.Base = PhysicalAddress(best_base);
        result.PageCount = metadata_page_count;
        return true;
    }

    void PhysicalMemoryManager::InitializeMetadataStorage(Uint64 managed_region_capacity, Uint64 managed_pages) noexcept {
        Uint8* storage = BootstrapIdentityPointer(m_MetadataSpan.Base);
        for (Uint64 i = 0; i < m_MetadataSpan.SizeBytes(); i++) 
            storage[i] = 0;

        const Uint64 region_metadata_bytes = managed_region_capacity * sizeof(ManagedRegion);

        m_PageStatesOffset = managed_region_capacity * sizeof(ManagedRegion);
        m_MetadataAccessBase = VirtualAddress(m_MetadataSpan.Base.Value());
        m_Regions = reinterpret_cast<ManagedRegion*>(storage);
        m_PageStates = storage + region_metadata_bytes;
        m_Statistics.ManagedPages = managed_pages;
    }

    void PhysicalMemoryManager::BuildManagedRegions(const Boot::BootEnvironment& environment, Uint64 descriptor_count, Uint64 managed_region_capacity) noexcept {
        m_RegionCount = 0;

        for (Uint64 i = 0; i < descriptor_count; i++) {
            const Boot::FirmwareMemoryDescriptor& descriptor = *DescriptorAt(environment, i);
            if (!IsManagedType(descriptor.Type) || descriptor.NumPages == 0) continue;

            ManagedRegion& region = m_Regions[m_RegionCount++];
            region.Base = descriptor.PhysStart;
            region.PageCount = descriptor.NumPages;
            region.StateOffset = 0;
            region.InitialState = InitialStateFor(descriptor.Type);
        }

        if (m_RegionCount > managed_region_capacity)
            m_RegionCount = managed_region_capacity;

        SortManagedRegions();
        MergedAdjacentManagedRegions();
        AssignStateOffsets();
        m_Statistics.ManagedRegionCount = m_RegionCount;
    }

    void PhysicalMemoryManager::SortManagedRegions() noexcept {
        for (Uint64 index = 1; index < m_RegionCount; ++index) {
            ManagedRegion value = m_Regions[index];
            Uint64 position = index;

            while (position != 0 && m_Regions[position - 1].Base > value.Base) {
                m_Regions[position] = m_Regions[position - 1];
                --position;
            }

            m_Regions[position] = value;
        }
    }

    void PhysicalMemoryManager::MergedAdjacentManagedRegions() noexcept {
        if (m_RegionCount < 2) return;

        Uint64 output = 0;
        for (Uint64 input = 1; input < m_RegionCount; input++) {
            ManagedRegion& current = m_Regions[output];
            const ManagedRegion& next = m_Regions[input];

            const Uint64 current_end = current.Base + (current.PageCount * PageSize);
            if (current_end == next.Base && current.InitialState == next.InitialState) {
                current.PageCount += next.PageCount;
                continue;
            }

            output++;
            if (output != input)
                m_Regions[output] = next;
        }

        m_RegionCount = output + 1;
    }

    void PhysicalMemoryManager::AssignStateOffsets() noexcept {
        Uint64 state_offset = 0;
        for (Uint64 index = 0; index < m_RegionCount; index++) {
            m_Regions[index].StateOffset = state_offset;
            state_offset += m_Regions[index].PageCount;
        }
    }

    void PhysicalMemoryManager::InitializePageStates() noexcept {
        m_Statistics.FreePages = 0;
        m_Statistics.DeferredBootPages = 0;
        m_Statistics.DeferredAcpiPages = 0;

        for (Uint64 region_index = 0; region_index < m_RegionCount; region_index++) {
            const ManagedRegion& region = m_Regions[region_index];
            for (Uint64 page = 0; page < region.PageCount; page++) 
                SetState(region, page, region.InitialState);

            switch(region.InitialState) {
            case PageState::Free:
                m_Statistics.FreePages += region.PageCount;
                break;
            case PageState::DeferredBoot:
                m_Statistics.DeferredBootPages += region.PageCount;
                break;
            case PageState::DeferredAcpi:
                m_Statistics.DeferredAcpiPages += region.PageCount;
                break;
            default:
                break;
            }
        }
    }

    PhysicalMemoryManager::PageState PhysicalMemoryManager::GetState(const ManagedRegion& region, Uint64 page_offset) const noexcept {
        if (page_offset >= region.PageCount) return PageState::Corrupt;
        return static_cast<PageState>(m_PageStates[region.StateOffset + page_offset]);
    }

    void PhysicalMemoryManager::SetState(const ManagedRegion& region, Uint64 page_offset, PageState state) noexcept {
        if (page_offset < region.PageCount) m_PageStates[region.StateOffset + page_offset] = static_cast<Uint8>(state);
    }

    void PhysicalMemoryManager::AccountStateRemoval(PageState state) noexcept {
        switch (state) {
        case PageState::Free:
            m_Statistics.FreePages--;
            break;
        case PageState::Allocated:
            m_Statistics.AllocatedPages--;
            break;
        case PageState::DeferredBoot:
            m_Statistics.DeferredBootPages--;
            break;
        case PageState::DeferredAcpi:
            m_Statistics.DeferredAcpiPages--;
            break;
        default:
            break;
        }
    }

    void PhysicalMemoryManager::ReserveSpan(PhysicalSpan span) noexcept {
        if (span.IsEmpty() || !span.Base.IsPageAligned() || span.PageCount > MaximumValue / PageSize) return;

        Uint64 span_end = 0;
        if (!TryRangeEnd(span.Base.Value(), span.SizeBytes(), span_end)) return;

        for (Uint64 region_index = 0; region_index < m_RegionCount; region_index++) {
            ManagedRegion& region = m_Regions[region_index];
            const Uint64 region_end = region.Base + (region.PageCount * PageSize);

            if (!Overlaps(span.Base.Value(), span_end, region.Base, region_end)) continue;

            const Uint64 overlap_base = Maximum(span.Base.Value(), region.Base);
            const Uint64 overlap_end = (Minimum(span_end, region_end));
            const Uint64 first_page = (overlap_base - region.Base) / PageSize;
            const Uint64 page_count = (overlap_end - overlap_base) / PageSize;

            for (Uint64 page = 0; page < page_count; page++) {
                const PageState current = GetState(region, first_page + page);
                if (current == PageState::Reserved) continue;

                AccountStateRemoval(current);
                SetState(region, first_page + page, PageState::Reserved);
            }
        }
    }

    void PhysicalMemoryManager::ReserveBootOwnedRanges(const Boot::BootEnvironment& environment) noexcept {
        auto reserve_byte_range = [this](const Boot::PhysicalRange& range) noexcept {
            if (range.Size == 0) return;

            Uint64 end = 0;
            if (!TryRangeEnd(range.Base, range.Size, end)) return;

            Uint64 aligned_end = 0;
            if (!TryAlignUp(end, PageSize, aligned_end)) return;

            const Uint64 aligned_base = AlignDown(range.Base, PageSize);
            ReserveSpan(PhysicalSpan{ PhysicalAddress(aligned_base), (aligned_end - aligned_base) / PageSize });
        };

        reserve_byte_range(environment.KernelImage);
        reserve_byte_range(environment.KernelStack);
        reserve_byte_range(environment.EnvironmentStorage);
        reserve_byte_range(environment.MemoryMapStorage);
        ReserveSpan(m_MetadataSpan);

        // Never allocate physical page zero. The VMM will independently keep VA
        // zero unmapped so null-pointer accesses fault rather than aliasing RAM.
        ReserveSpan(PhysicalSpan{ PhysicalAddress(0), 1 });
    }

    PhysicalMemoryInitializationError PhysicalMemoryManager::Initialize(const Boot::BootEnvironment& environment) noexcept {
        if (m_Initialized) return PhysicalMemoryInitializationError::AlreadyInitialized;

        if (environment.Signature != Boot::EnvironmentSignature
            || environment.Version != Boot::ProtocolVersion 
            || environment.Size < sizeof(Boot::BootEnvironment)
            || environment.MemoryMapStorage.Base == 0 
            || environment.MemoryMapSize == 0
            || environment.MemoryMapDescriptorSize < sizeof(Boot::FirmwareMemoryDescriptor)
            || environment.MemoryMapSize > environment.MemoryMapStorage.Size
            || (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0) 
            return PhysicalMemoryInitializationError::InvalidBootEnvironment;

        const Boot::PhysicalRange protected_ranges[] = {
            environment.KernelImage,
            environment.KernelStack,
            environment.EnvironmentStorage,
            environment.MemoryMapStorage,
        };

        for (const Boot::PhysicalRange& range : protected_ranges) {
            if (range.Base == 0 || range.Size == 0 || (range.Base & (PageSize - 1)) != 0 || (range.Size & (PageSize - 1)) != 0) 
                return PhysicalMemoryInitializationError::InvalidBootEnvironment;

            Uint64 end = 0;
            if (!TryRangeEnd(range.Base, range.Size, end)) 
                return PhysicalMemoryInitializationError::InvalidBootEnvironment;
        }

        for (Uint64 left = 0; left < sizeof(protected_ranges) / sizeof(protected_ranges[0]); left++) {
            Uint64 left_end = 0;
            if (!TryRangeEnd(protected_ranges[left].Base, protected_ranges[left].Size, left_end)) 
                return PhysicalMemoryInitializationError::InvalidBootEnvironment;

            for (Uint64 right = left + 1; right < sizeof(protected_ranges) / sizeof(protected_ranges[0]); right++) {
                Uint64 right_end = 0;
                if (!TryRangeEnd(protected_ranges[right].Base, protected_ranges[right].Size, right_end))
                    return PhysicalMemoryInitializationError::InvalidBootEnvironment;

                if (Overlaps(protected_ranges[left].Base, left_end, protected_ranges[right].Base, right_end)) 
                    return PhysicalMemoryInitializationError::InvalidBootEnvironment;
            }
        }

        const Uint64 descriptor_count = environment.MemoryMapSize / environment.MemoryMapDescriptorSize;
        if (descriptor_count == 0) 
            return PhysicalMemoryInitializationError::InvalidMemoryMap;

        Uint64 managed_pages = 0;
        Uint64 managed_region_capacity = 0;
        Uint64 conventional_pages = 0;
        const PhysicalMemoryInitializationError validation = ValidateMemoryMap(environment, descriptor_count, managed_pages, managed_region_capacity, conventional_pages);
        if (validation != PhysicalMemoryInitializationError::Success) return validation;

        if (managed_region_capacity > MaximumValue / sizeof(ManagedRegion)) 
            return PhysicalMemoryInitializationError::MetadataSizeOverflow;

        const Uint64 region_metadata_bytes = managed_region_capacity * sizeof(ManagedRegion);
        if (managed_pages > MaximumValue - region_metadata_bytes) 
            return PhysicalMemoryInitializationError::MetadataSizeOverflow;

        const Uint64 page_state_bytes = managed_pages * sizeof(Uint8);
        const Uint64 metadata_bytes = region_metadata_bytes + page_state_bytes;
        Uint64 metadata_allocation_bytes = 0;
        if (!TryAlignUp(metadata_bytes, PageSize, metadata_allocation_bytes))
            return PhysicalMemoryInitializationError::MetadataSizeOverflow;

        const Uint64 metadata_page_count = metadata_allocation_bytes / PageSize;
        PhysicalSpan metadata_span{};
        if (!FindMetadataRegion(environment, descriptor_count, metadata_page_count, metadata_span))
            return PhysicalMemoryInitializationError::NoMetadataRegion;

        m_MetadataSpan = metadata_span;
        m_Statistics.ConventionalPages = conventional_pages;
        m_Statistics.MetadataBytes = metadata_bytes;

        InitializeMetadataStorage(managed_region_capacity, managed_pages);
        BuildManagedRegions(environment, descriptor_count, managed_region_capacity);
        InitializePageStates();
        ReserveBootOwnedRanges(environment);

        m_BootstrapStackSpan = PhysicalSpan{ PhysicalAddress(environment.KernelStack.Base), environment.KernelStack.Size / PageSize };
        m_EnvironmentStorageSpan = PhysicalSpan{ PhysicalAddress(environment.EnvironmentStorage.Base), environment.EnvironmentStorage.Size / PageSize };
        m_MemoryMapStorageSpan = PhysicalSpan{ PhysicalAddress(environment.MemoryMapStorage.Base), environment.MemoryMapStorage.Size / PageSize };

        m_Initialized = true;
        return PhysicalMemoryInitializationError::Success;
    }

    PhysicalMemoryReclamationError PhysicalMemoryManager::ReclaimBootMemory(PhysicalMemoryReclamationResult& result) noexcept {
        result = {};

        if (!m_Initialized) return PhysicalMemoryReclamationError::NotInitialized;
        if (m_BootMemoryReclaimed) return PhysicalMemoryReclamationError::AlreadyReclaimed;
        if (m_Regions == nullptr || m_PageStates == nullptr || m_RegionCount == 0) return PhysicalMemoryReclamationError::CorruptState;

        /*
         * Preflight the complete PMM state before modifying anything.
         *
         * This verifies both the encoded page states and the statistics
         * maintained by the allocator.
         */
        Uint64 observed_pages =0;

        Uint64 free_pages = 0;
        Uint64 allocated_pages = 0;
        Uint64 reserved_pages = 0;
        Uint64 deferred_boot_pages = 0;
        Uint64 deferred_acpi_pages = 0;
        for (Uint64 i = 0; i < m_RegionCount; i++) {
            const ManagedRegion& region = m_Regions[i];
            if (region.PageCount > m_Statistics.ManagedPages - observed_pages)
                return PhysicalMemoryReclamationError::CorruptState;
            
            observed_pages += region.PageCount;
            for (Uint64 page = 0; page < region.PageCount; page++) {
                const PageState state = GetState(region, page);
                switch (state) {
                case PageState::Reserved: ++reserved_pages; break;
                case PageState::Free: ++free_pages; break;
                case PageState::Allocated: ++allocated_pages; break;
                case PageState::DeferredBoot: ++deferred_boot_pages; break;
                case PageState::DeferredAcpi: ++deferred_acpi_pages; break;
                case PageState::Corrupt:
                default:
                    return PhysicalMemoryReclamationError::CorruptState;
                }
            }
        }

        /*
         * Region coverage must exactly describe every page represented
         * by the PMM metadata
         */
        if (observed_pages != m_Statistics.ManagedPages) 
            return PhysicalMemoryReclamationError::CorruptState;

        /*
         * The observed classifications must account for every managed
         * page exactly once
         */
        if (reserved_pages > observed_pages || free_pages > observed_pages - reserved_pages)
            return PhysicalMemoryReclamationError::CorruptState;

        Uint64 classified_pages = reserved_pages + free_pages;
        if (allocated_pages > observed_pages - classified_pages) 
            return PhysicalMemoryReclamationError::CorruptState;

        classified_pages += allocated_pages;
        if (deferred_boot_pages > observed_pages - classified_pages) 
            return PhysicalMemoryReclamationError::CorruptState;

        classified_pages += deferred_boot_pages;
        if (deferred_acpi_pages != observed_pages - classified_pages) 
            return PhysicalMemoryReclamationError::CorruptState;

        /*
         * We have now completed every operation that can fail.
         *
         * From this point onward the transition is deterministic:
         *  - DeferredBoot -> Free
         */
        for (Uint64 i = 0; i < m_RegionCount; i++) {
            const ManagedRegion& region = m_Regions[i];
            for (Uint64 page = 0; page < region.PageCount; page++) {
                if (GetState(region, page) != PageState::DeferredBoot) continue;
                SetState(region, page, PageState::Free);
            }
        }

        m_Statistics.FreePages += deferred_boot_pages;
        m_Statistics.DeferredBootPages = 0;
        m_BootMemoryReclaimed = true;
        result.ReleasedPages = deferred_boot_pages;
        return PhysicalMemoryReclamationError::Success;
    }

    PhysicalMemoryMetadataAccessError PhysicalMemoryManager::PromoteMetadataAccess(VirtualAddress metadata_base) noexcept {
        if (!m_Initialized) return PhysicalMemoryMetadataAccessError::NotInitialized;
        if (m_MetadataAccessPromoted) return PhysicalMemoryMetadataAccessError::AlreadyPromoted;
        if (metadata_base.IsNull() ||
            !metadata_base.IsPageAligned() ||
            metadata_base == m_MetadataAccessBase ||
            m_MetadataSpan.IsEmpty() ||
            m_Regions == nullptr ||
            m_PageStates == nullptr ||
            m_RegionCount == 0)
            return PhysicalMemoryMetadataAccessError::InvalidAddress;

        const Uint64 metadata_size = m_MetadataSpan.SizeBytes();
        if (m_PageStatesOffset > metadata_size) return PhysicalMemoryMetadataAccessError::MetadataMismatch;
        if (m_Statistics.ManagedPages > metadata_size - m_PageStatesOffset)
            return PhysicalMemoryMetadataAccessError::MetadataMismatch;

        /*
         * Ensure the candidate virtual span itself cannot wrap.
         */
        if (metadata_base.Value() > MaximumValue - metadata_size)
            return PhysicalMemoryMetadataAccessError::InvalidAddress;

        auto* candidate_storage = reinterpret_cast<Uint8*>(metadata_base.Value());
        auto* candidate_regions = reinterpret_cast<ManagedRegion*>(candidate_storage);
        Uint8* candidate_page_states = candidate_storage + m_PageStatesOffset;

        /*
         * Both aliases are still present here.
         *
         * The caller has already proven that metadata_base maps the same
         * physical pages. This check additionally proves that the complete
         * allocator state is visible through the candidate virtual view
         * before changing any persistent pointer.
         */
        if (!MetadataViewsMatch(candidate_regions, candidate_page_states)) 
            return PhysicalMemoryMetadataAccessError::MetadataMismatch;

        /*
         * One-way pointer transition.
         *
         * From this point onward the PMM no longer dereferences its
         * bootstrap identity alias.
         */
        m_Regions = candidate_regions;
        m_PageStates = candidate_page_states;
        m_MetadataAccessBase = metadata_base;
        m_MetadataAccessPromoted = true;

        return PhysicalMemoryMetadataAccessError::Success;
    }

    BootstrapResourceReleaseError PhysicalMemoryManager::ReleaseBootstrapResources(BootstrapResourceReleaseResult& result) noexcept {
        result = {};
        if (!m_Initialized) return BootstrapResourceReleaseError::NotInitialized;
        if (!m_BootMemoryReclaimed) return BootstrapResourceReleaseError::BootMemoryNotReclaimed;
        if (m_BootstrapResourcesReleased) return BootstrapResourceReleaseError::AlreadyReleased;
        if (m_BootstrapStackSpan.IsEmpty() ||
            m_EnvironmentStorageSpan.IsEmpty() ||
            m_MemoryMapStorageSpan.IsEmpty()) 
            return BootstrapResourceReleaseError::InvalidState;

        /*
         * Preflight everything before changing a single page.
         *
         * These ranges were explicitly converted to Reserved during
         * PMM initialization and must still be Reserved now.
         */
        if (!SpanHasState(m_BootstrapStackSpan, PageState::Reserved) ||
            !SpanHasState(m_EnvironmentStorageSpan, PageState::Reserved) ||
            !SpanHasState(m_MemoryMapStorageSpan, PageState::Reserved))
            return BootstrapResourceReleaseError::InvalidState;

        Uint64 released_pages = m_BootstrapStackSpan.PageCount;
        if (m_EnvironmentStorageSpan.PageCount > MaximumValue - released_pages) 
            return BootstrapResourceReleaseError::InvalidState;

        released_pages += m_EnvironmentStorageSpan.PageCount;
        if (m_MemoryMapStorageSpan.PageCount > MaximumValue - released_pages)
            return BootstrapResourceReleaseError::InvalidState;

        released_pages += m_MemoryMapStorageSpan.PageCount;
        if (released_pages > MaximumValue - m_Statistics.FreePages)
            return BootstrapResourceReleaseError::InvalidState;

        /*
         * Complete deterministic transition:
         *  - Reserved -> Free
         */
        if (!SetSpanState(m_BootstrapStackSpan, PageState::Free) || 
            !SetSpanState(m_EnvironmentStorageSpan, PageState::Free) ||
            !SetSpanState(m_MemoryMapStorageSpan, PageState::Free))
            return BootstrapResourceReleaseError::InvalidState;

        m_Statistics.FreePages += released_pages;

        m_BootstrapResourcesReleased = true;
        result.ReleasedPages = released_pages;
        return BootstrapResourceReleaseError::Success;
    }

    bool PhysicalMemoryManager::RangeIsFree(const ManagedRegion& region, Uint64 first_page_offset, Uint64 page_count) const noexcept {
        if (page_count == 0 || first_page_offset >= region.PageCount || page_count > region.PageCount - first_page_offset) return false;

        for (Uint64 page = 0; page < page_count; page++) 
            if (GetState(region, first_page_offset + page) != PageState::Free)
                return false;
        return true;
    }

    void PhysicalMemoryManager::MarkAllocated(const ManagedRegion& region, Uint64 first_page_offset, Uint64 page_count) noexcept {
        for (Uint64 page = 0; page < page_count; page++) 
            SetState(region, first_page_offset + page, PageState::Allocated);
        m_Statistics.FreePages -= page_count;
        m_Statistics.AllocatedPages += page_count;
    }

    void PhysicalMemoryManager::MarkFree(const ManagedRegion& region, Uint64 firstPageOffset, Uint64 pageCount) noexcept {
        for (Uint64 page = 0; page < pageCount; ++page)
            SetState(region, firstPageOffset + page, PageState::Free);
        m_Statistics.AllocatedPages -= pageCount;
        m_Statistics.FreePages += pageCount;
    }

    bool PhysicalMemoryManager::ValidateConstraints(Uint64 page_count, const PhysicalAllocationConstraints& constraints, Uint64& allocation_size, Uint64& minimum_address, Uint64& maximum_base_address, Uint64& alignment) const noexcept {
        if (page_count == 0 || constraints.MinimumAddress > constraints.MaximumAddress || constraints.Alignment < PageSize || !IsPowerOfTwo(constraints.Alignment) || (constraints.Alignment % PageSize) != 0) return false;
        if (page_count > MaximumValue / PageSize) return false;

        allocation_size = page_count * PageSize;
        alignment = constraints.Alignment;   
        
        minimum_address = constraints.MinimumAddress.Value();

        if (minimum_address < PageSize) minimum_address = PageSize;

        const Uint64 maximum_address = constraints.MaximumAddress.Value();

        if (allocation_size - 1 > maximum_address) return false;

        maximum_base_address = maximum_address - (allocation_size - 1);

        return minimum_address <= maximum_base_address;
    }

    bool PhysicalMemoryManager::FindFreeRangeInRegionLow(const ManagedRegion& region, Uint64 page_count, Uint64 allocation_size, Uint64 minimum_address, Uint64 maximum_base_address, Uint64 alignment, Uint64& first_physical_page) const noexcept {
        if (region.PageCount > MaximumValue / PageSize) return false;

        const Uint64 region_size = region.PageCount * PageSize;

        Uint64 region_end = 0;
        if (!TryRangeEnd(region.Base, region_size, region_end)) return false;

        if (allocation_size > region_size) return false;

        const Uint64 first_allowed = Maximum(region.Base, minimum_address);
        const Uint64 last_allowed = Minimum(region_end - allocation_size, maximum_base_address);
        if (first_allowed > last_allowed) return false;

        Uint64 candidate = 0;
        if (!TryAlignUp(first_allowed, alignment, candidate)) return false;

        while (candidate <= last_allowed) {
            if ((candidate & (alignment - 1)) != 0) return false;
            const Uint64 offset_bytes = candidate - region.Base;
            if ((offset_bytes & (PageSize - 1)) != 0) return false;
            const Uint64 page_offset = offset_bytes / PageSize;
            if (RangeIsFree(region, page_offset, page_count)) {
                first_physical_page = candidate;
                return true;
            }

            if (candidate > MaximumValue - alignment) return false;
            candidate += alignment;
        }

        return false;
    }

    bool PhysicalMemoryManager::FindFreeRangeInRegionHigh(const ManagedRegion& region, Uint64 page_count, Uint64 allocation_size, Uint64 minimum_address, Uint64 maximum_base_address, Uint64 alignment, Uint64& first_physical_page) const noexcept {
        if (region.PageCount > MaximumValue / PageSize)
            return false;

        const Uint64 region_size = region.PageCount * PageSize;

        Uint64 region_end = 0;
        if (!TryRangeEnd(region.Base, region_size, region_end)) return false;

        if (allocation_size > region_size) return false;

        const Uint64 first_allowed = Maximum(region.Base, minimum_address);

        const Uint64 last_allowed = Minimum(region_end - allocation_size, maximum_base_address);

        if (first_allowed > last_allowed) return false;

        Uint64 candidate = AlignDown(last_allowed, alignment);

        for (;;) {
            if (candidate < first_allowed) return false;

            // This should be mathematically guaranteed by AlignDown(), but keep
            // the assertion explicit at this safety boundary.
            if ((candidate & (alignment - 1)) != 0) return false;

            const Uint64 offset_bytes = candidate - region.Base;

            if ((offset_bytes & (PageSize - 1)) != 0) return false;

            const Uint64 page_offset = offset_bytes / PageSize;

            if (RangeIsFree(region, page_offset, page_count)) {
                first_physical_page = candidate;
                return true;
            }

            if (candidate < alignment)
                return false;

            candidate -= alignment;
        }
    }

    PhysicalAllocationError PhysicalMemoryManager::AllocatePage(PhysicalAllocation& output, PhysicalAllocationConstraints constraints) noexcept {
        return AllocateContiguous(1, output, constraints);
    }

    PhysicalAllocationError PhysicalMemoryManager::AllocateContiguous(Uint64 page_count, PhysicalAllocation& output, PhysicalAllocationConstraints constraints) noexcept {
        if (!m_Initialized) return PhysicalAllocationError::NotInitialized;
        if (output.IsValid()) return PhysicalAllocationError::OutputAlreadyOwnsMemory;

        Uint64 allocation_size = 0;
        Uint64 minimum_address = 0;
        Uint64 maximum_base_address = 0;
        Uint64 alignment = 0;
        if (!ValidateConstraints(page_count, constraints, allocation_size, minimum_address, maximum_base_address, alignment)) 
            return PhysicalAllocationError::InvalidRequest;

        Uint64 first_physical_address = 0;
        const ManagedRegion* selected_region = nullptr;

        if (constraints.Preference == PhysicalAllocationPreference::HighAddresses) {
            for (Uint64 i = m_RegionCount; i != 0; i--) {
                const ManagedRegion& region = m_Regions[i - 1];
                if (FindFreeRangeInRegionHigh(region, page_count, allocation_size, minimum_address, maximum_base_address, alignment, first_physical_address)) {
                    selected_region = &region;
                    break;
                }
            }
        } else {
            for (Uint64 i = 0; i < m_RegionCount; i++) {
                const ManagedRegion& region = m_Regions[i];
                if (FindFreeRangeInRegionLow(region, page_count, allocation_size, minimum_address, maximum_base_address, alignment, first_physical_address)) {
                    selected_region = &region;
                    break;
                }
            }
        }

        if (selected_region == nullptr)
            return PhysicalAllocationError::OutOfMemory;

        const Uint64 maximum_address = constraints.MaximumAddress.Value();

        Uint64 allocation_end = 0;
        if (!TryRangeEnd(first_physical_address, allocation_size, allocation_end)) 
            return PhysicalAllocationError::CorruptAllocation;

        if (first_physical_address < constraints.MinimumAddress.Value()
         || allocation_end == 0 
         || allocation_end - 1 > maximum_address
         || (first_physical_address & (constraints.Alignment - 1)) != 0) 
            return PhysicalAllocationError::CorruptAllocation;
        
        const Uint64 offset_bytes = first_physical_address - selected_region->Base;
        if ((offset_bytes & (PageSize - 1)) != 0) 
            return PhysicalAllocationError::CorruptAllocation;

        const Uint64 first_page_offset = offset_bytes / PageSize;
        MarkAllocated(*selected_region, first_page_offset, page_count);

        output.m_Owner = this;
        output.m_Span = PhysicalSpan{ PhysicalAddress{ first_physical_address }, page_count };
        return PhysicalAllocationError::Success;
    }

    const PhysicalMemoryManager::ManagedRegion* PhysicalMemoryManager::FindContainingRegion(PhysicalSpan span) const noexcept {
        if (span.IsEmpty() || !span.Base.IsPageAligned() || span.PageCount > MaximumValue / PageSize) return nullptr;

        Uint64 span_end = 0;
        if (!TryRangeEnd(span.Base.Value(), span.SizeBytes(), span_end)) return nullptr;

        for (Uint64 i = 0; i < m_RegionCount; i++) {
            const ManagedRegion& region = m_Regions[i];
            const Uint64 region_end = region.Base + (region.PageCount * PageSize);
            if (span.Base.Value() >= region.Base && span_end <= region_end)
                return &region;
        }
        return nullptr;
    }

    bool PhysicalMemoryManager::SpanHasState(PhysicalSpan span, PageState expected) const noexcept {
        if (span.IsEmpty() || !span.Base.IsPageAligned() || span.PageCount > MaximumValue / PageSize) return false;
        for (Uint64 page = 0; page < span.PageCount; page++) {
            const PhysicalAddress address = span.Base + page * PageSize;
            const PhysicalSpan single_page{ address, 1 };
            const ManagedRegion* region = FindContainingRegion(single_page);
            if (region == nullptr) return false;
            const Uint64 page_offset = (address.Value() - region->Base) / PageSize;
            if (GetState(*region, page_offset) != expected) return false;
        }
        return true;
    }

    bool PhysicalMemoryManager::SetSpanState(PhysicalSpan span, PageState state) noexcept {
        for (Uint64 page = 0; page < span.PageCount; page++) {
            const PhysicalAddress address = span.Base + page * PageSize;
            const PhysicalSpan single_page{ address, 1 };
            const ManagedRegion* region = FindContainingRegion(single_page);

            /*
             * This is only called after a complete preflight using
             * SpanHasState(), while the kernel is still single-CPU.
             */
            if (region == nullptr) return false;
            const Uint64 page_offset = (address.Value() - region->Base) / PageSize;
            SetState(*region, page_offset, state);
        }

        return true;
    }

    PhysicalAllocationError PhysicalMemoryManager::Release(PhysicalAllocation& allocation) noexcept {
        if (!m_Initialized) return PhysicalAllocationError::NotInitialized;

        if (!allocation.IsValid() || allocation.m_Owner != this) 
            return allocation.m_Owner == nullptr
                ? PhysicalAllocationError::CorruptAllocation
                : PhysicalAllocationError::WrongOwner;

        const PhysicalSpan span = allocation.m_Span;
        const ManagedRegion* region = FindContainingRegion(span);
        if (region == nullptr)
            return PhysicalAllocationError::CorruptAllocation;

        const Uint64 first_page_offset = (span.Base.Value() - region->Base) / PageSize;
        for (Uint64 page = 0; page < span.PageCount; page++) 
            if (GetState(*region, first_page_offset + page) != PageState::Allocated) 
                return PhysicalAllocationError::CorruptAllocation;

        MarkFree(*region, first_page_offset, span.PageCount);
        allocation.Invalidate();
        return PhysicalAllocationError::Success;
    }

    const char* PhysicalMemoryManager::Describe(PhysicalMemoryInitializationError error) noexcept {
        switch (error) {
            case PhysicalMemoryInitializationError::Success: return "success";
            case PhysicalMemoryInitializationError::AlreadyInitialized: return "already initialized";
            case PhysicalMemoryInitializationError::InvalidBootEnvironment: return "invalid boot environment";
            case PhysicalMemoryInitializationError::InvalidMemoryMap: return "invalid firmware memory map";
            case PhysicalMemoryInitializationError::DescriptorRangeOverflow: return "firmware descriptor range overflow";
            case PhysicalMemoryInitializationError::DescriptorRangesOverlap: return "overlapping firmware memory descriptors";
            case PhysicalMemoryInitializationError::NoUsableMemory: return "no immediately usable physical memory";
            case PhysicalMemoryInitializationError::MetadataSizeOverflow: return "physical-memory metadata size overflow";
            case PhysicalMemoryInitializationError::NoMetadataRegion: return "no safe ConventionalMemory region for physical-memory metadata";
        }
        return "unknown initialization error";
    }

    const char* PhysicalMemoryManager::Describe(PhysicalAllocationError error) noexcept {
        switch (error) {
            case PhysicalAllocationError::Success: return "success";
            case PhysicalAllocationError::NotInitialized: return "physical memory manager is not initialized";
            case PhysicalAllocationError::InvalidRequest: return "invalid physical allocation request";
            case PhysicalAllocationError::OutputAlreadyOwnsMemory: return "output token already owns physical memory";
            case PhysicalAllocationError::OutOfMemory: return "no matching physical pages are available";
            case PhysicalAllocationError::WrongOwner: return "allocation belongs to another manager";
            case PhysicalAllocationError::CorruptAllocation: return "physical allocation token or page state is invalid";
        }
        return "unknown physical allocation error";
    }

    const char* PhysicalMemoryManager::Describe(PhysicalMemoryReclamationError error) noexcept {
        switch (error) {
        case PhysicalMemoryReclamationError::Success: return "success";
        case PhysicalMemoryReclamationError::NotInitialized: return "physical memory manager is not initialized";
        case PhysicalMemoryReclamationError::AlreadyReclaimed: return "boot memory has already been reclaimed";
        case PhysicalMemoryReclamationError::CorruptState: return "physical memory state is inconsistent";
        }
        return "unknown physical memory reclamation error";
    }

    const char* PhysicalMemoryManager::Describe(BootstrapResourceReleaseError error) noexcept {
        switch (error) {
        case BootstrapResourceReleaseError::Success: return "success";
        case BootstrapResourceReleaseError::NotInitialized: return "physical memory manager is not initialized";
        case BootstrapResourceReleaseError::BootMemoryNotReclaimed: return "deferred boot memory has not been reclaimed";
        case BootstrapResourceReleaseError::AlreadyReleased: return "bootstrap resources have already been released";
        case BootstrapResourceReleaseError::InvalidState: return "bootstrap resource page state is inconsistent";
        }
        return "unknown bootstrap resource release error";
    }

    const char* PhysicalMemoryManager::Describe(PhysicalMemoryMetadataAccessError error) noexcept {
        switch (error) {
        case PhysicalMemoryMetadataAccessError::Success: return "success";
        case PhysicalMemoryMetadataAccessError::NotInitialized: return "physical memory manager is not initialized";
        case PhysicalMemoryMetadataAccessError::AlreadyPromoted: return "physical-memory metadata access is already promoted";
        case PhysicalMemoryMetadataAccessError::InvalidAddress: return "physical-memory metadata access address is invalid";
        case PhysicalMemoryMetadataAccessError::MetadataMismatch: return "physical-memory metadata aliases do not contain identical state";
        }
        return "unknown physical-memory metadata access error";
    }
}   