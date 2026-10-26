#include <Kernel/Memory/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Runtime/New.hpp>

namespace Zos::Kernel::Memory::SelfTest {
    void VerifyPhysicalAllocator(State& memory) noexcept {
        PhysicalMemoryManager& manager = memory.Physical;

        const Uint64 free_pages_before = manager.Statistics().FreePages;
        const Uint64 allocated_pages_before = manager.Statistics().AllocatedPages;

        PhysicalAllocation first{};
        PhysicalAllocation second{};
        PhysicalAllocation contiguous{};

        auto error = manager.AllocatePage(first);
        if (error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

        error = manager.AllocatePage(second);
        if (error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

        if (first.Base() == second.Base() ||
            !first.Base().IsPageAligned() ||
            !second.Base().IsPageAligned())
            Diagnostics::Fatal("Memory", "single-page allocation invariant failed");

        PhysicalAllocationConstraints dma32 = PhysicalAllocationConstraints::Dma32();
        dma32.Alignment = 64 * 1024;

        error = manager.AllocateContiguous(4, contiguous, dma32);
        if (error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

        const Uint64 contiguous_end = contiguous.Base().Value() + contiguous.SizeBytes() - 1;

        if (contiguous.Base().Value() > Dma32AddressLimit ||
            contiguous_end > Dma32AddressLimit ||
            (contiguous.Base().Value() & ((64 * 1024) - 1)) != 0)
            Diagnostics::Fatal("Memory", "DMA32 allocation constraints were violated");

        if (manager.Release(second) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "single-page release validation failed");

        if (manager.Release(second) != PhysicalAllocationError::CorruptAllocation)
            Diagnostics::Fatal("Memory", "double-free protection failed");

        if (manager.Release(first) != PhysicalAllocationError::Success ||
            manager.Release(contiguous) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "allocation release validation failed");

        if (manager.Statistics().FreePages != free_pages_before ||
            manager.Statistics().AllocatedPages != allocated_pages_before)
            Diagnostics::Fatal("Memory", "physical allocation accounting did not return to baseline");

        Diagnostics::Write("[zOS/Memory] Allocation, DMA32, and double-free self-test passed.\n");
    }

    void VerifyVirtualMemory(State& memory) noexcept {
        auto& physical_memory = memory.Physical;
        auto& metadata = memory.BootstrapMetadata;
        auto& kernel_addresses = memory.KernelAddresses;
        auto& mmio_addresses = memory.MmioAddresses;
        auto& page_map = memory.KernelPageMap;

        if (!physical_memory.IsInitialized() || !metadata.IsInitialized() ||
            !kernel_addresses.IsInitialized() || !mmio_addresses.IsInitialized() ||
            !page_map.IsInitialized())
            Diagnostics::Fatal("VMM", "virtual-memory self-test dependencies are not initialized");

        if (!kernel_addresses.Validate() || !mmio_addresses.Validate())
            Diagnostics::Fatal("VMM", "virtual address allocator failed initial validation");

        if (!page_map.Validate())
            Diagnostics::Fatal("VMM", "page map failed initial ownership validation");

        const VirtualAddressAllocatorStatistics baseline = kernel_addresses.Statistics();

        VirtualReservation mmio_probe{};
        if (mmio_addresses.Reserve(1, mmio_probe) != VirtualAllocationError::Success)
            Diagnostics::Fatal("VMM", "kernel MMIO virtual reservation failed");

        if (mmio_probe.Base() != Layout::KernelMmioBase)
            Diagnostics::Fatal("VMM", "kernel MMIO allocator returned an unexpected base");

        if (mmio_addresses.Release(mmio_probe) != VirtualAllocationError::Success)
            Diagnostics::Fatal("VMM", "kernel MMIO virtual reservation release failed");

        VirtualReservation lead{};
        auto virtual_error = kernel_addresses.Reserve(1, lead);
        if (virtual_error != VirtualAllocationError::Success)
            Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

        VirtualReservation reservation{};
        VirtualAllocationConstraints constraints{};
        constraints.Alignment = 64 * 1024;

        virtual_error = kernel_addresses.Reserve(4, reservation, constraints);
        if (virtual_error != VirtualAllocationError::Success)
            Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

        if (!reservation.Base().IsPageAligned() || (reservation.Base().Value() & ((64 * 1024) - 1)) != 0 ||
            reservation.PageCount() != 4 || reservation.Base() == lead.Base())
            Diagnostics::Fatal("VMM", "virtual reservation alignment invariant failed");

        PhysicalAllocation backing{};
        const auto physical_error = physical_memory.AllocateContiguous(4, backing);
        if (physical_error != PhysicalAllocationError::Success)
            Diagnostics::Fatal("VMM", PhysicalMemoryManager::Describe(physical_error));

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        for (Uint64 page = 0; page < reservation.PageCount(); ++page) {
            const auto mapping_error = page_map.MapPage(reservation.Base() + page * PageSize, backing.Base() + page * PageSize, options);

            if (mapping_error != Architecture::AMD64::MappingError::Success)
                Diagnostics::Fatal("VMM", Architecture::AMD64::PageMap::Describe(mapping_error));
        }

        const Uint64 probe_offset = PageSize + 0x2A5;
        const VirtualAddress probe_virtual = reservation.Base() + probe_offset;
        const PhysicalAddress expected_physical = backing.Base() + probe_offset;

        const auto translation = page_map.Translate(probe_virtual);
        if (!translation.Mapped || translation.Physical != expected_physical ||
            !HasAccess(translation.Options.Access, PageAccess::Read) ||
            !HasAccess(translation.Options.Access, PageAccess::Write) ||
            !HasAccess(translation.Options.Access, PageAccess::Global) ||
            HasAccess(translation.Options.Access, PageAccess::Execute) ||
            translation.Options.Cache != CachePolicy::WriteBack)
            Diagnostics::Fatal("VMM", "page-table translation or permission decoding failed");

        if (page_map.MapPage(reservation.Base(), backing.Base(),options) != Architecture::AMD64::MappingError::AlreadyMapped)
            Diagnostics::Fatal("VMM", "duplicate mapping protection failed");

        const MappingOptions invalid_options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Execute,
            .Cache = CachePolicy::WriteBack,
        };

        if (page_map.MapPage(reservation.Base() + reservation.SizeBytes(), backing.Base(), invalid_options) != Architecture::AMD64::MappingError::InvalidPermissions)
            Diagnostics::Fatal("VMM", "W^X mapping policy failed");

        if (page_map.MapPage(VirtualAddress(0), backing.Base(), options) != Architecture::AMD64::MappingError::InvalidVirtualAddress)
            Diagnostics::Fatal("VMM", "null-region mapping protection failed");

        for (Uint64 page = 0; page < reservation.PageCount();++page) {
            const VirtualAddress address = reservation.Base() + page * PageSize;

            if (page_map.UnmapPage(address) != Architecture::AMD64::MappingError::Success)
                Diagnostics::Fatal("VMM", "page-table unmap failed");

            if (page_map.IsMapped(address))
                Diagnostics::Fatal("VMM", "unmapped virtual page still translates");
        }

        if (page_map.UnmapPage(reservation.Base()) != Architecture::AMD64::MappingError::NotMapped)
            Diagnostics::Fatal("VMM", "duplicate unmap protection failed");

        if (physical_memory.Release(backing) != PhysicalAllocationError::Success ||
            kernel_addresses.Release(reservation) != VirtualAllocationError::Success ||
            kernel_addresses.Release(lead) != VirtualAllocationError::Success)
            Diagnostics::Fatal("VMM", "virtual-memory self-test cleanup failed");

        if (!kernel_addresses.Validate())
            Diagnostics::Fatal("VMM", "virtual address allocator failed final validation");

        const auto& final = kernel_addresses.Statistics();

        if (final.FreePages != baseline.FreePages || final.ReservedPages != baseline.ReservedPages ||
            final.FreeExtentCount != baseline.FreeExtentCount || final.ActiveReservations != baseline.ActiveReservations)
            Diagnostics::Fatal("VMM", "virtual address accounting did not return to baseline");

        if (page_map.Statistics().MappedPages != 0 || page_map.Statistics().TablePages != 1 || !page_map.Validate())
            Diagnostics::Fatal("VMM", "page map did not return to root-only valid state");

        Diagnostics::Write("[zOS/VMM] Reservation ownership, mapping, translation, W^X, null-guard, and release self-test passed.\n");
    }

    void VerifyBootMemoryReclamation(State& memory) noexcept {
        PhysicalMemoryManager& manager = memory.Physical;

        if (!manager.IsBootMemoryReclaimed())
            Diagnostics::Fatal("Memory", "boot-memory reclamation self-test ran before reclamation");

        const PhysicalMemoryStatistics baseline = manager.Statistics();

        PhysicalMemoryReclamationResult duplicate_result{};
        const auto duplicate_error = manager.ReclaimBootMemory(duplicate_result);

        if (duplicate_error != PhysicalMemoryReclamationError::AlreadyReclaimed || duplicate_result.ReleasedPages != 0)
            Diagnostics::Fatal("Memory", "duplicate boot-memory reclamation protection failed");

        const PhysicalMemoryStatistics after_duplicate = manager.Statistics();
        if (after_duplicate.FreePages != baseline.FreePages ||
            after_duplicate.AllocatedPages != baseline.AllocatedPages ||
            after_duplicate.DeferredBootPages != baseline.DeferredBootPages ||
            after_duplicate.DeferredAcpiPages != baseline.DeferredAcpiPages ||
            after_duplicate.ReservedPages() != baseline.ReservedPages())
            Diagnostics::Fatal("Memory", "duplicate reclamation modified PMM state");

        PhysicalAllocation probe{};
        if (manager.AllocatePage(probe) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-reclamation allocation failed");

        if (manager.Release(probe) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-reclamation allocation release failed");

        const PhysicalMemoryStatistics final = manager.Statistics();
        if (final.FreePages != baseline.FreePages ||
            final.AllocatedPages != baseline.AllocatedPages ||
            final.DeferredBootPages != 0 ||
            final.DeferredAcpiPages != baseline.DeferredAcpiPages ||
            final.ReservedPages() != baseline.ReservedPages())
            Diagnostics::Fatal("Memory", "post-reclamation PMM accounting did not return to baseline");

        Diagnostics::Write("[zOS/Memory] Boot-memory reclamation self-test passed.\n");
    }

    void VerifyKernelHeap(State& memory) noexcept {
        KernelHeap& heap = memory.Heap;

        if (!heap.IsInitialized())
            Diagnostics::Fatal("Heap", "kernel heap self-test ran before initialization");

        const KernelHeapStatistics baseline = heap.Statistics();

        void* first{};
        void* second{};
        void* third{};

        if (heap.Allocate(24, first) != KernelHeapError::Success ||
            heap.Allocate(257, second, 64) != KernelHeapError::Success ||
            heap.Allocate(513, third, 256) != KernelHeapError::Success)
            Diagnostics::Fatal("Heap", "basic kernel heap allocation failed");

        if (first == nullptr || second == nullptr || third == nullptr ||
            first == second || first == third || second == third ||
            (reinterpret_cast<Uint64>(first) & (KernelHeap::DefaultAlignment - 1)) != 0 ||
            (reinterpret_cast<Uint64>(second) & 63) != 0 ||
            (reinterpret_cast<Uint64>(third) & 255) != 0)
            Diagnostics::Fatal("Heap", "kernel heap alignment or uniqueness invariant failed");

        auto* first_bytes = static_cast<Uint8*>(first);
        for (Uint64 i = 0; i < 24; ++i)
            first_bytes[i] = static_cast<Uint8>(0xA0 + i);

        void* resized{};
        if (heap.Reallocate(first, 2048, resized) != KernelHeapError::Success || resized == nullptr)
            Diagnostics::Fatal("Heap", "kernel heap reallocation failed");

        auto* resized_bytes = static_cast<Uint8*>(resized);
        for (Uint64 i = 0; i < 24; ++i)
            if (resized_bytes[i] != static_cast<Uint8>(0xA0 + i))
                Diagnostics::Fatal("Heap", "kernel heap reallocation did not preserve payload data");

        first = resized;
        if (heap.Free(third) != KernelHeapError::Success)
            Diagnostics::Fatal("Heap", "kernel heap free failed");

        if (heap.Free(third) != KernelHeapError::DoubleFree)
            Diagnostics::Fatal("Heap", "kernel heap double-free protection failed");

        void* page_aligned{};
        if (heap.Allocate(128, page_aligned, PageSize) != KernelHeapError::Success ||
            page_aligned == nullptr || (reinterpret_cast<Uint64>(page_aligned) & (PageSize - 1)) != 0)
            Diagnostics::Fatal("Heap", "page-aligned kernel heap allocation failed");

        if (heap.Free(second) != KernelHeapError::Success ||
            heap.Free(page_aligned) != KernelHeapError::Success ||
            heap.Free(first) != KernelHeapError::Success)
            Diagnostics::Fatal("Heap", "kernel heap release/coalescing test failed");

        Uint64 foreign{};
        if (heap.Free(&foreign) != KernelHeapError::InvalidPointer)
            Diagnostics::Fatal("Heap", "kernel heap foreign-pointer protection failed");

        if (!heap.Validate())
            Diagnostics::Fatal("Heap", "kernel heap structural validation failed");

        const KernelHeapStatistics after = heap.Statistics();
        if (after.ReservedPages != baseline.ReservedPages ||
            after.CommittedPages != baseline.CommittedPages ||
            after.SegmentCount != baseline.SegmentCount ||
            after.SegmentMetadataBytes != baseline.SegmentMetadataBytes ||
            after.FreeBlockBytes != baseline.FreeBlockBytes ||
            after.AllocationCount != 0 ||
            after.RequestedBytes != 0 ||
            after.AllocatedBlockBytes != 0)
            Diagnostics::Fatal("Heap", "kernel heap accounting did not return to baseline");

        Diagnostics::Write("[zOS/Heap] Allocation, alignment, reallocation, coalescing, and protection self-test passed.\n");
    }

    struct CxxAllocationProbe final {
        Uint64 First;
        Uint64 Second;
    };

    struct alignas(256) CxxAlignedAllocationProbe final {
        Uint8 Payload[257]{};
    };

    void VerifyCxxAllocation(State& memory) noexcept {
        if (!::Zos::Kernel::Runtime::IsKernelHeapBound())
            Diagnostics::Fatal("C++ Runtime", "C++ allocation self-test ran before heap binding");

        KernelHeap& heap = memory.Heap;
        const KernelHeapStatistics baseline = heap.Statistics();

        auto* scalar = new CxxAllocationProbe{
            0x1122334455667788ULL,
            0x8877665544332211ULL
        };

        if (!heap.Contains(scalar) ||
            scalar->First != 0x1122334455667788ULL ||
            scalar->Second != 0x8877665544332211ULL)
            Diagnostics::Fatal("C++ Runtime", "scalar new did not use the permanent kernel heap");

        auto* array = new Uint64[32]{};
        if (!heap.Contains(array))
            Diagnostics::Fatal("C++ Runtime", "array new did not use the permanent kernel heap");

        for (Uint64 i = 0; i < 32; ++i) array[i] = 0x1000 + i;
        for (Uint64 i = 0; i < 32; ++i)
            if (array[i] != 0x1000 + i)
                Diagnostics::Fatal("C++ Runtime", "array new allocation payload is invalid");

        auto* aligned = new CxxAlignedAllocationProbe{};
        if (!heap.Contains(aligned) || (reinterpret_cast<Uint64>(aligned) & 255) != 0)
            Diagnostics::Fatal("C++ Runtime", "over-aligned new did not satisfy requested alignment");

        auto* empty = new Uint8[0];
        if (empty == nullptr || !heap.Contains(empty))
            Diagnostics::Fatal("C++ Runtime", "zero-length array allocation contract failed");

        void* nothrow_aligned = ::operator new(73, static_cast<std::align_val_t>(512), std::nothrow);
        if (nothrow_aligned == nullptr || !heap.Contains(nothrow_aligned) ||
            (reinterpret_cast<Uint64>(nothrow_aligned) & 511) != 0)
            Diagnostics::Fatal("C++ Runtime", "aligned nothrow allocation failed");

        ::operator delete(nothrow_aligned, static_cast<std::align_val_t>(512), std::nothrow);

        void* impossible = ::operator new(static_cast<__SIZE_TYPE__>(~Uint64{ 0 }), std::nothrow);
        if (impossible != nullptr) {
            ::operator delete(impossible, std::nothrow);
            Diagnostics::Fatal("C++ Runtime", "nothrow allocation did not report exhaustion");
        }

        delete scalar;
        delete[] array;
        delete aligned;
        delete[] empty;

        if (!heap.Validate())
            Diagnostics::Fatal("C++ Runtime", "kernel heap validation failed after C++ allocation test");

        const KernelHeapStatistics after = heap.Statistics();
        if (after.ReservedPages != baseline.ReservedPages ||
            after.CommittedPages != baseline.CommittedPages ||
            after.SegmentCount != baseline.SegmentCount ||
            after.AllocationCount != baseline.AllocationCount ||
            after.RequestedBytes != baseline.RequestedBytes ||
            after.AllocatedBlockBytes != baseline.AllocatedBlockBytes ||
            after.FreeBlockBytes != baseline.FreeBlockBytes ||
            after.SegmentMetadataBytes != baseline.SegmentMetadataBytes)
            Diagnostics::Fatal("C++ Runtime", "C++ allocation self-test did not return heap accounting to baseline");

        Diagnostics::Write("[zOS/Runtime] Scalar, array, aligned, and nothrow C++ allocation self-test passed.\n");
    }

    void VerifyPermanentMemory(State& memory) noexcept {
        auto& physical_memory = memory.Physical;
        auto& virtual_addresses = memory.KernelAddresses;
        auto& mmio_addresses = memory.MmioAddresses;
        auto& page_map = memory.KernelPageMap;
        auto& heap = memory.Heap;
        auto& bootstrap_metadata = memory.BootstrapMetadata;

        if (!bootstrap_metadata.IsRetired() || bootstrap_metadata.IsInitialized() ||
            !physical_memory.IsInitialized() || !physical_memory.IsMetadataAccessPromoted() ||
            !virtual_addresses.IsMetadataPromoted() || !mmio_addresses.IsMetadataPromoted() ||
            !page_map.IsMetadataPromoted() || !page_map.IsActive() ||
            !heap.IsInitialized())
            Diagnostics::Fatal("Memory", "permanent-memory self-test dependencies are invalid");

        VirtualReservation reservation{};
        if (virtual_addresses.Reserve(1, reservation) != VirtualAllocationError::Success ||
            virtual_addresses.Release(reservation) != VirtualAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement virtual reservation round trip failed");

        VirtualReservation mmio_reservation{};
        if (mmio_addresses.Reserve(1, mmio_reservation) != VirtualAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement MMIO virtual reservation failed");

        const VirtualAddress probe_virtual = mmio_reservation.Base();
        if (page_map.IsMapped(probe_virtual))
            Diagnostics::Fatal("Memory", "permanent PageMap probe address is already mapped");

        PhysicalAllocation backing{};
        if (physical_memory.AllocatePage(backing) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement physical allocation failed");

        const Uint64 table_pages_before = page_map.TablePageCount();

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const auto mapping_error = page_map.MapPage(probe_virtual, backing.Base(), options);
        if (mapping_error != Architecture::AMD64::MappingError::Success)
            Diagnostics::Fatal("Memory", Architecture::AMD64::PageMap::Describe(mapping_error));

        if (page_map.TablePageCount() <= table_pages_before)
            Diagnostics::Fatal("Memory", "post-retirement mapping did not create permanent PageMap metadata");

        const auto translation = page_map.Translate(probe_virtual);
        if (!translation.Mapped ||
            translation.Physical != backing.Base() ||
            translation.Options.Access != options.Access ||
            translation.Options.Cache != options.Cache)
            Diagnostics::Fatal("Memory", "post-retirement PageMap translation failed");

        const VirtualAddress direct = Layout::DirectMapAddress(backing.Base());
        if (direct.IsNull())
            Diagnostics::Fatal("Memory", "permanent PageMap probe backing is outside the direct map");

        constexpr Uint64 ProbeValue{ 0x5A4F535045524D41ULL };
        auto* mapped_pointer = reinterpret_cast<volatile Uint64*>(probe_virtual.Value());
        auto* direct_pointer = reinterpret_cast<volatile Uint64*>(direct.Value());

        *mapped_pointer = ProbeValue;

        if (*direct_pointer != ProbeValue)
            Diagnostics::Fatal("Memory", "post-retirement PageMap hardware alias test failed");

        if (page_map.UnmapPage(probe_virtual) != Architecture::AMD64::MappingError::Success ||
            mmio_addresses.Release(mmio_reservation) != VirtualAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement PageMap cleanup failed");

        if (page_map.TablePageCount() != table_pages_before)
            Diagnostics::Fatal("Memory", "post-retirement PageMap table accounting did not return to baseline");

        if (physical_memory.Release(backing) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement physical release failed");

        void* heap_probe{};
        if (heap.Allocate(256, heap_probe) != KernelHeapError::Success ||
            heap_probe == nullptr || heap.Free(heap_probe) != KernelHeapError::Success)
            Diagnostics::Fatal("Memory", "post-retirement heap round trip failed");

        if (!virtual_addresses.Validate() || !mmio_addresses.Validate() ||
            !page_map.Validate() || !heap.Validate())
            Diagnostics::Fatal("Memory", "permanent memory infrastructure failed final validation");

        Diagnostics::Write("[zOS/Memory] Permanent PMM, VAA, MMIO VAA, PageMap, and heap self-test passed.\n");
    }
}
