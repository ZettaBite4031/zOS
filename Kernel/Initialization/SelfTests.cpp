#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Memory/Layout.hpp>

#include <Kernel/Runtime/New.hpp>

namespace Zos::Kernel {
    void RunPhysicalMemorySelfTest(Memory::PhysicalMemoryManager& manager) noexcept {
        using namespace Memory;

        const Uint64 free_pages_before = manager.Statistics().FreePages;
        const Uint64 allocated_pages_before = manager.Statistics().AllocatedPages;

        PhysicalAllocation first{};
        PhysicalAllocation second{};
        PhysicalAllocation contiguous{};

        PhysicalAllocationError error = manager.AllocatePage(first);
        if (error != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

        error = manager.AllocatePage(second);
        if (error != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

        if (first.Base() == second.Base() || !first.Base().IsPageAligned() || !second.Base().IsPageAligned()) 
            Diagnostics::Fatal("Memory", "single-page allocation invariant failed");

        PhysicalAllocationConstraints dma32 = PhysicalAllocationConstraints::Dma32();
        dma32.Alignment = 64 * 1024;

        error = manager.AllocateContiguous(4, contiguous, dma32);
        if (error != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("Memory", PhysicalMemoryManager:: Describe(error));

        const Uint64 contiguous_end = contiguous.Base().Value() + contiguous.SizeBytes() - 1;
        if (contiguous.Base().Value() > Dma32AddressLimit || contiguous_end > Dma32AddressLimit || (contiguous.Base().Value() & ((64 * 1024) - 1)) != 0) 
            Diagnostics::Fatal("Memory", "DMA32 allocation constraints were violated");

        if (manager.Release(second) != PhysicalAllocationError:: Success) 
            Diagnostics::Fatal("Memory", "single-page release validation failed");

        if (manager.Release(second) != PhysicalAllocationError::CorruptAllocation) 
            Diagnostics::Fatal("Memory", "double-free protection failed");

        if (manager.Release(first) != PhysicalAllocationError::Success || manager.Release(contiguous) != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("Memory", "allocation release validation failed");
        
        if (manager.Statistics().FreePages != free_pages_before || manager.Statistics().AllocatedPages != allocated_pages_before) 
            Diagnostics::Fatal("Memory", "physical allocation accounting did not return to baseline");

        Diagnostics::Write("[zOS/Memory] Allocation, DMA32, and double-free self-test passed.\n");
    }

    void RunVirtualMemorySelfTest(KernelRuntime& runtime) {
        using namespace Memory;
        using Architecture::AMD64::MappingError;

        auto& physical_memory = runtime.PhysicalMemory;
        auto& metadata = runtime.BootstrapMetadata;
        auto& kernel_addresses = runtime.KernelAddresses;
        auto& page_map = runtime.KernelPageMap;

        if (!physical_memory.IsInitialized() || !metadata.IsInitialized() ||
            !kernel_addresses.IsInitialized() || !page_map.IsInitialized()) 
            Diagnostics::Fatal("VMM", "virtual-memory self-test dependencies are not initialized");
        
        if (!kernel_addresses.Validate())
            Diagnostics::Fatal("VMM", "virtual address allocator failed initial validation");
        if (!page_map.Validate())
            Diagnostics::Fatal("VMM", "page map failed initial ownership validation");
            
        const VirtualAddressAllocatorStatistics virtual_baseline = kernel_addresses.Statistics();
        
        VirtualReservation lead_reservation{};
        auto virtual_error = kernel_addresses.Reserve(1, lead_reservation);
        if (virtual_error != VirtualAllocationError::Success) 
            Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

        VirtualReservation reservation{};
        VirtualAllocationConstraints virtual_constraints{};
        virtual_constraints.Alignment = 64 * 1024;
        virtual_error = kernel_addresses.Reserve(4, reservation, virtual_constraints);
        if (virtual_error != VirtualAllocationError::Success) 
            Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

        if (!reservation.Base().IsPageAligned() || (reservation.Base().Value() & ((64 * 1024) - 1)) != 0 ||
            reservation.PageCount() != 4 || reservation.Base() == lead_reservation.Base()) 
            Diagnostics::Fatal("VMM", "virtual reservation alignment invariant failed");
        
        PhysicalAllocation backing{};
        const auto physical_error = physical_memory.AllocateContiguous(4, backing);
        if (physical_error != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("VMM", PhysicalMemoryManager::Describe(physical_error));

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        for (Uint64 page = 0; page <reservation.PageCount(); ++page) {
            const VirtualAddress virtual_address = reservation.Base() + page * PageSize;
            const PhysicalAddress physical_address = backing.Base() + page * PageSize;
            const auto mapping_error = page_map.MapPage(virtual_address, physical_address, options);
            if (mapping_error != MappingError::Success) 
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

        if (page_map.MapPage(reservation.Base(), backing.Base(), options ) != MappingError::AlreadyMapped) 
            Diagnostics::Fatal("VMM", "duplicate mapping protection failed");
        
        const MappingOptions invalid_options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Execute,
            .Cache = CachePolicy::WriteBack,
        };

        if (page_map.MapPage(reservation.Base() + reservation.SizeBytes(), backing.Base(), invalid_options) != MappingError::InvalidPermissions) 
            Diagnostics::Fatal("VMM", "W^X mapping policy failed");

        if (page_map.MapPage(VirtualAddress(0), backing.Base(), options) != MappingError::InvalidVirtualAddress) 
            Diagnostics::Fatal("VMM", "null-region mapping protection failed");

        for (Uint64 page = 0; page < reservation.PageCount(); ++page) {
            const VirtualAddress virtual_address = reservation.Base() + page * PageSize;
            if (page_map.UnmapPage(virtual_address) != MappingError::Success) 
                Diagnostics::Fatal("VMM", "page-table unmap failed");

            if (page_map.IsMapped(virtual_address)) 
                Diagnostics::Fatal("VMM", "unmapped virtual page still translates");
        }

        if (page_map.UnmapPage(reservation.Base()) != MappingError::NotMapped) 
            Diagnostics::Fatal("VMM", "duplicate unmap protection failed");
        
        if (physical_memory.Release(backing) != PhysicalAllocationError::Success) 
            Diagnostics::Fatal("VMM", "physical backing release failed");

        if (kernel_addresses.Release(reservation) != VirtualAllocationError::Success) 
            Diagnostics::Fatal("VMM", "aligned virtual reservation release failed");

        if (kernel_addresses.Release(lead_reservation) != VirtualAllocationError::Success) 
            Diagnostics::Fatal("VMM", "lead virtual reservation release failed");

        if (!kernel_addresses.Validate())
            Diagnostics::Fatal("VMM", "virtual address allocator failed final validation");

        const auto& final_virtual = kernel_addresses.Statistics();
        if (final_virtual.FreePages != virtual_baseline.FreePages || 
            final_virtual.ReservedPages != virtual_baseline.ReservedPages || 
            final_virtual.FreeExtentCount != virtual_baseline.FreeExtentCount || 
            final_virtual.ActiveReservations != virtual_baseline.ActiveReservations) 
            Diagnostics::Fatal("VMM", "virtual address accounting did not return to baseline");
        

        if (page_map.Statistics().MappedPages != 0 ||
            page_map.Statistics().TablePages != 1) 
            Diagnostics::Fatal("VMM", "page-table cleanup did not return to root-only state");
        if (!page_map.Validate())
            Diagnostics::Fatal("VMM", "page map failed final ownership validation");


        Diagnostics::Write("[zOS/VMM] Reservation ownership, mapping, translation, W^X, null-guard, and release self-test passed.\n");
    }

    void RunBootMemoryReclamationSelfTest(KernelRuntime& runtime) noexcept {
        using namespace Memory; 

        PhysicalMemoryManager& manager = runtime.PhysicalMemory;

        if (!manager.IsBootMemoryReclaimed()) 
            Diagnostics::Fatal("Memory", "boot-memory reclamation self-test ran before reclamation");

        const PhysicalMemoryStatistics baseline = manager.Statistics();

        /*
        * Reclamation is a one-time state transition. A second request
        * must be explicitly rejected rather than silently doing
        * nothing.
        */
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

        /*
        * Prove that ordinary allocation/release remains consistent
        * after the state transition.
        */
        PhysicalAllocation probe{};
        const auto allocation_error = manager.AllocatePage(probe);
        if (allocation_error != PhysicalAllocationError::Success) 
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

    void RunKernelHeapSelfTest(KernelRuntime& runtime) noexcept {
        using namespace Memory;

        KernelHeap& heap = runtime.Heap;
        if (!heap.IsInitialized())
            Diagnostics::Fatal("Heap", "kernel heap self-test ran before initialization");

        const KernelHeapStatistics baseline = heap.Statistics();

        void* first = nullptr;
        void* second = nullptr;
        void* third = nullptr;

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

        void* resized = nullptr;
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

        void* page_aligned = nullptr;
        if (heap.Allocate(128, page_aligned, PageSize) != KernelHeapError::Success ||
            page_aligned == nullptr ||
            (reinterpret_cast<Uint64>(page_aligned) & (PageSize - 1)) != 0)
            Diagnostics::Fatal("Heap", "page-aligned kernel heap allocation failed");

        if (heap.Free(second) != KernelHeapError::Success ||
            heap.Free(page_aligned) != KernelHeapError::Success ||
            heap.Free(first) != KernelHeapError::Success)
            Diagnostics::Fatal("Heap", "kernel heap release/coalescing test failed");

        Uint64 foreign = 0;
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
        Memory::Uint64 First;
        Memory::Uint64 Second;
    };

    struct alignas(256) CxxAlignedAllocationProbe final {
        Memory::Uint8 Payload[257]{};
    };

    void RunCxxAllocationSelfTest(KernelRuntime& runtime) noexcept {
        using namespace Memory;

        if (!Runtime::IsKernelHeapBound()) 
            Diagnostics::Fatal("C++ Runtime", "C++ allocation self-test ran before heap binding");

        KernelHeap& heap = runtime.Heap;
        const KernelHeapStatistics baseline = heap.Statistics();

        /*
            * Scalar new/delete
            */
        auto* scalar = new CxxAllocationProbe{ 0x1122334455667788ULL, 0x8877665544332211ULL };
        if (!heap.Contains(scalar) || scalar->First != 0x1122334455667788ULL || scalar->Second != 0x8877665544332211) 
            Diagnostics::Fatal("C++ Runtime", "scalar new did not use the permanent kernel heap");

        /*
            * Array new/delete
            */
        auto* array = new Uint64[32]{};
        if (!heap.Contains(array)) 
            Diagnostics::Fatal("C++ Runtime", "array new did not use the permanent kernel heap");

        for (Uint64 i = 0; i < 32; i++)
            array[i] = 0x1000 + i;

        for (Uint64 i = 0; i < 32; i++) 
            if (array[i] != 0x1000 + i) 
                Diagnostics::Fatal("C++ Runtime", "array new allocation payload is invalid");
        
        /*
            * Compiler-generated over-aligned allocation.
            */
        auto* aligned = new CxxAlignedAllocationProbe{};
        if (!heap.Contains(aligned) || (reinterpret_cast<Uint64>(aligned) & 255) != 0) 
            Diagnostics::Fatal("C++ Runtime", "over-aligned new did not satisfy requested alignment");

        /*
            * Zero-length arrays are legal in C++.
            *
            * The runtime normalizes the underlying zero-sized allocation to
            * one byte so the successful result remains non-null and owned.
            */
        auto empty = new Uint8[0];
        if (empty == nullptr || !heap.Contains(empty)) 
            Diagnostics::Fatal("C++ Runtime", "zero-length array allocation contract failed");

        /*
            * Exercise a direct aligned nothrow allocation
            */
        void* nothrow_aligned = ::operator new(73, static_cast<std::align_val_t>(512), std::nothrow);
        if (nothrow_aligned == nullptr || !heap.Contains(nothrow_aligned) || (reinterpret_cast<Uint64>(nothrow_aligned) & 512) != 0) 
            Diagnostics::Fatal("C++ Runtime", "aligned nothrow allocation failed");

        ::operator delete(nothrow_aligned, static_cast<std::align_val_t>(511), std::nothrow);

        /*
            * A request that cannot possibly fit in the aerna must fail
            * without halting when the nothrow form is explicitly selected.
            */
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

        /*
        * These test allocations are intentionally small enough to fit in
        * the initial segment, so all logical allocation accounting should
        * return exactly to baseline.
        */
        if (after.ReservedPages != baseline.ReservedPages || after.CommittedPages != baseline.CommittedPages || 
            after.SegmentCount != baseline.SegmentCount || after.AllocationCount != baseline.AllocationCount || 
            after.RequestedBytes != baseline.RequestedBytes || after.AllocatedBlockBytes != baseline.AllocatedBlockBytes || 
            after.FreeBlockBytes != baseline.FreeBlockBytes || after.SegmentMetadataBytes != baseline.SegmentMetadataBytes) 
            Diagnostics::Fatal("C++ Runtime", "C++ allocation self-test did not return heap accounting to baseline");

        Diagnostics::Write("[zOS/Runtime] Scalar, array, aligned, and nothrow C++ allocation self-test passed.\n");
    }

    void RunPermanentMemorySelfTest(KernelRuntime& runtime) noexcept {
        using namespace Memory;
        using namespace Architecture::AMD64;

        PhysicalMemoryManager& physical_memory = runtime.PhysicalMemory;
        VirtualAddressAllocator& virtual_addresses = runtime.KernelAddresses;
        PageMap& page_map = runtime.KernelPageMap;
        KernelHeap& heap = runtime.Heap;
        BootstrapMetadataArena& bootstrap_metadata = runtime.BootstrapMetadata;

        if (!bootstrap_metadata.IsRetired() || bootstrap_metadata.IsInitialized() || !physical_memory.IsInitialized() ||
            !physical_memory.IsMetadataAccessPromoted() || !virtual_addresses.IsMetadataPromoted() || !page_map.IsMetadataPromoted() ||
            !page_map.IsActive() || !heap.IsInitialized())
            Diagnostics::Fatal("Memory", "permanent-memory self-test dependencies are invalid");

        VirtualReservation reservation{};
        if (virtual_addresses.Reserve(1, reservation) != VirtualAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement virtual reservation failed");
        if (virtual_addresses.Release(reservation) != VirtualAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement virtual reservation release failed");

        const VirtualAddress probe_virtual = Layout::KernelMmioBase;
        if (page_map.IsMapped(probe_virtual)) Diagnostics::Fatal("Memory", "permanent PageMap probe address is already mapped");

        PhysicalAllocation backing{};
        if (physical_memory.AllocatePage(backing) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement physical allocation failed");

        const Uint64 table_pages_before = page_map.TablePageCount();
        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        const MappingError mapping_error = page_map.MapPage(probe_virtual, backing.Base(), options);
        if (mapping_error != MappingError::Success) Diagnostics::Fatal("Memory", PageMap::Describe(mapping_error));
        if (page_map.TablePageCount() <= table_pages_before)
            Diagnostics::Fatal("Memory", "post-retirement mapping did not create permanent PageMap metadata");

        const TranslationResult translation = page_map.Translate(probe_virtual);
        if (!translation.Mapped || translation.Physical != backing.Base() || translation.Options.Access != options.Access || translation.Options.Cache != options.Cache)
            Diagnostics::Fatal("Memory", "post-retirement PageMap translation failed");

        const VirtualAddress direct = Layout::DirectMapAddress(backing.Base());
        if (direct.IsNull()) Diagnostics::Fatal("Memory", "permanent PageMap probe backing is outside the direct map");

        constexpr Uint64 ProbeValue{ 0x5A4F535045524D41ULL };
        auto* mapped_pointer = reinterpret_cast<volatile Uint64*>(probe_virtual.Value());
        auto* direct_pointer = reinterpret_cast<volatile Uint64*>(direct.Value());
        *mapped_pointer = ProbeValue;
        if (*direct_pointer != ProbeValue) Diagnostics::Fatal("Memory", "post-retirement PageMap hardware alias test failed");

        if (page_map.UnmapPage(probe_virtual) != MappingError::Success)
            Diagnostics::Fatal("Memory", "post-retirement PageMap unmap failed");
        if (page_map.TablePageCount() != table_pages_before)
            Diagnostics::Fatal("Memory", "post-retirement PageMap table accounting did not return to baseline");
        if (physical_memory.Release(backing) != PhysicalAllocationError::Success)
            Diagnostics::Fatal("Memory", "post-retirement physical release failed");

        void* heap_probe = nullptr;
        if (heap.Allocate(256, heap_probe) != KernelHeapError::Success || heap_probe == nullptr)
            Diagnostics::Fatal("Memory", "post-retirement heap allocation failed");
        if (heap.Free(heap_probe) != KernelHeapError::Success)
            Diagnostics::Fatal("Memory", "post-retirement heap release failed");

        if (!virtual_addresses.Validate() || !page_map.Validate() || !heap.Validate())
            Diagnostics::Fatal("Memory", "permanent memory infrastructure failed final validation");

        Diagnostics::Write("[zOS/Memory] Permanent PMM, VAA, PageMap, and heap self-test passed.\n");
    }

}