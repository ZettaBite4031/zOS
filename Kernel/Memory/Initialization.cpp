#include <Kernel/Memory/Initialization.hpp>
#include <Kernel/Memory/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Architecture/AMD64/Stack.hpp>
#include <Kernel/Memory/KernelAddressSpace.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Runtime/New.hpp>

#include <Kernel/Architecture/AMD64/Interrupts.hpp>

namespace Zos::Kernel::Memory {
    namespace {
        using namespace Architecture::AMD64;

        inline constexpr Uint64 PrimaryKernelStackPages{ 16 };
        static_assert(PrimaryKernelStackPages * PageSize == 64 * 1024);

        [[nodiscard]] bool SamePhysicalMemoryStatistics(const PhysicalMemoryStatistics& left, const PhysicalMemoryStatistics& right) noexcept {
            return left.ManagedPages == right.ManagedPages &&
                left.ConventionalPages == right.ConventionalPages &&
                left.FreePages == right.FreePages &&
                left.AllocatedPages == right.AllocatedPages &&
                left.DeferredBootPages == right.DeferredBootPages &&
                left.DeferredAcpiPages == right.DeferredAcpiPages &&
                left.MetadataBytes == right.MetadataBytes &&
                left.ManagedRegionCount == right.ManagedRegionCount;
        }

        void UnmapIdentitySpan(PageMap& page_map, PhysicalSpan span) noexcept {
            if (span.IsEmpty())
                Diagnostics::Fatal("VMM", "attempted to retire an empty identity span");

            for (Uint64 page = 0; page < span.PageCount; ++page) {
                const PhysicalAddress physical = span.Base + page * PageSize;
                const VirtualAddress identity{ physical.Value() };

                const auto translation = page_map.Translate(identity);
                if (!translation.Mapped || translation.Physical != physical)
                    Diagnostics::Fatal("VMM", "identity mapping is inconsistent");

                const auto error = page_map.UnmapPage(identity);
                if (error != MappingError::Success)
                    Diagnostics::Fatal("VMM", PageMap::Describe(error));

                if (page_map.IsMapped(identity))
                    Diagnostics::Fatal("VMM", "retired identity page still maps");

                const VirtualAddress direct = Layout::DirectMapAddress(physical);
                if (direct.IsNull())
                    Diagnostics::Fatal("VMM", "physical page is outside the direct map");

                const auto direct_translation = page_map.Translate(direct);
                if (!direct_translation.Mapped || direct_translation.Physical != physical)
                    Diagnostics::Fatal("VMM", "physical page lost direct-map coverage");
            }
        }

        void InitializePhysical(State& memory, const ::Zos::Boot::BootEnvironment& environment) noexcept {
            const auto error = memory.Physical.Initialize(environment);

            if (error != PhysicalMemoryInitializationError::Success)
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const auto& statistics = memory.Physical.Statistics();
            const PhysicalSpan metadata = memory.Physical.MetadataSpan();

            Diagnostics::Write("[zOS/Memory] Physical allocator initialized.\n");
            Diagnostics::Write("[zOS/Memory] Conventional memory: ");
            Diagnostics::WriteDecimal(statistics.ConventionalBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Memory] Free memory: ");
            Diagnostics::WriteDecimal(statistics.FreeBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Memory] Deferred boot-service memory: ");
            Diagnostics::WriteDecimal(statistics.DeferredBootBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Memory] Deferred ACPI memory: ");
            Diagnostics::WriteDecimal(statistics.DeferredAcpiBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Memory] PMM metadata: ");
            Diagnostics::WriteHex(metadata.Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(metadata.SizeBytes());
            Diagnostics::Write(" bytes\n");

            SelfTest::VerifyPhysicalAllocator(memory);

            Diagnostics::Write("[zOS/Startup] Physical memory ownership established.\n");
        }

        void InitializeVirtual(State& memory) noexcept {
            const auto metadata_error = memory.BootstrapMetadata.Initialize(memory.Physical);
            if (metadata_error != MetadataArenaInitializationError::Success)
                Diagnostics::Fatal("VMM", BootstrapMetadataArena::Describe(metadata_error));

            const auto virtual_error = memory.KernelAddresses.Initialize(Layout::KernelDynamicSpan(), memory.BootstrapMetadata);
            if (virtual_error != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

            const auto mmio_error = memory.MmioAddresses.Initialize(Layout::KernelMmioSpan(), memory.BootstrapMetadata);
            if (mmio_error != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(mmio_error));

            const auto page_map_error = memory.KernelPageMap.Initialize(memory.Physical, memory.BootstrapMetadata);
            if (page_map_error != PageMapInitializationError::Success)
                Diagnostics::Fatal("VMM", PageMap::Describe(page_map_error));

            SelfTest::VerifyVirtualMemory(memory);

            Diagnostics::Write("[zOS/Startup] Virtual-memory infrastructure established.\n");
        }

        void ActivateAddressSpace(State& memory, const ::Zos::Boot::BootEnvironment& environment) noexcept {
            KernelAddressSpace address_space{ memory.Physical, memory.BootstrapMetadata, memory.KernelPageMap};

            const auto build_error = address_space.Build(environment);
            if (build_error != KernelAddressSpaceError::Success)
                Diagnostics::Fatal("VMM", KernelAddressSpace::Describe(build_error));

            Diagnostics::Write("[zOS/VMM] Kernel address space constructed.\n");
            Diagnostics::Write("[zOS/VMM] New CR3 root: ");
            Diagnostics::WriteHex(memory.KernelPageMap.RootTable().Value());
            Diagnostics::Write("\n");

            const auto activation_error = address_space.Activate();
            if (activation_error != KernelAddressSpaceError::Success)
                Diagnostics::Fatal("VMM", KernelAddressSpace::Describe(activation_error));

            Diagnostics::Write("[zOS/VMM] Kernel-owned address space active.\n");
            Diagnostics::Write("[zOS/VMM] Active CR3: ");
            Diagnostics::WriteHex(PageMap::CurrentRootTable().Value());
            Diagnostics::Write("\n");
        }

        void PromotePhysicalMetadata(State& memory) noexcept {
            PhysicalMemoryManager& physical_memory = memory.Physical;
            PageMap& page_map = memory.KernelPageMap;

            if (!physical_memory.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot promote PMM metadata before initialization");

            if (!page_map.IsActive())
                Diagnostics::Fatal("Memory", "cannot promote PMM metadata before address-space activation");

            if (physical_memory.IsMetadataAccessPromoted())
                Diagnostics::Fatal("Memory", "PMM metadata was promoted more than once");

            const PhysicalSpan metadata = physical_memory.MetadataSpan();
            if (metadata.IsEmpty())
                Diagnostics::Fatal("Memory", "PMM metadata span is empty");

            const VirtualAddress direct_base = Layout::DirectMapAddress(metadata.Base);

            if (direct_base.IsNull())
                Diagnostics::Fatal("Memory", "PMM metadata lies outside the direct map");

            const MappingOptions expected{
                .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
                .Cache = CachePolicy::WriteBack,
            };

            for (Uint64 page = 0; page < metadata.PageCount; ++page) {
                const PhysicalAddress physical = metadata.Base + page * PageSize;
                const VirtualAddress direct = Layout::DirectMapAddress(physical);

                if (direct.IsNull())
                    Diagnostics::Fatal("Memory", "PMM metadata direct-map address is unavailable");

                const TranslationResult translation = page_map.Translate(direct);
                if (!translation.Mapped ||
                    translation.Physical != physical ||
                    translation.Options.Access != expected.Access ||
                    translation.Options.Cache != expected.Cache)
                    Diagnostics::Fatal("Memory", "PMM metadata direct-map mapping is invalid");
            }

            const PhysicalMemoryStatistics before = physical_memory.Statistics();
            const auto error = physical_memory.PromoteMetadataAccess(direct_base);
            if (error != PhysicalMemoryMetadataAccessError::Success)
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            if (!physical_memory.IsMetadataAccessPromoted() ||
                physical_memory.MetadataAccessBase() != direct_base)
                Diagnostics::Fatal("Memory", "PMM metadata promotion state is inconsistent");

            if (!SamePhysicalMemoryStatistics(before, physical_memory.Statistics()))
                Diagnostics::Fatal("Memory", "PMM metadata promotion modified allocator accounting");

            Diagnostics::Write("[zOS/Memory] PMM metadata promoted to direct map at ");
            Diagnostics::WriteHex(direct_base.Value());
            Diagnostics::Write("\n");

            UnmapIdentitySpan(page_map, metadata);

            const PhysicalMemoryStatistics baseline = physical_memory.Statistics();

            PhysicalAllocation probe{};

            if (physical_memory.AllocatePage(probe) != PhysicalAllocationError::Success)
                Diagnostics::Fatal("Memory", "post-promotion PMM allocation failed");

            if (physical_memory.Release(probe) != PhysicalAllocationError::Success)
                Diagnostics::Fatal("Memory", "post-promotion PMM release failed");

            if (!SamePhysicalMemoryStatistics(baseline, physical_memory.Statistics()))
                Diagnostics::Fatal("Memory", "post-promotion PMM accounting did not return to baseline");

            Diagnostics::Write("[zOS/Memory] PMM identity alias retired.\n");
            Diagnostics::Write("[zOS/Memory] PMM direct-map self-test passed.\n");
        }

        void ReclaimBootMemory(State& memory, const InterruptManager& interrupts) noexcept {
            if (!memory.Physical.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before PMM initialization");

            if (!memory.KernelPageMap.IsActive())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before kernel address-space activation" );

            if (!interrupts.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before interrupt infrastructure initialization");

            const PhysicalMemoryStatistics before = memory.Physical.Statistics();
            PhysicalMemoryReclamationResult result{};
            const auto error = memory.Physical.ReclaimBootMemory(result);
            if (error != PhysicalMemoryReclamationError::Success)
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const PhysicalMemoryStatistics after = memory.Physical.Statistics();
            if (result.ReleasedPages != before.DeferredBootPages ||
                after.DeferredBootPages != 0 ||
                after.ManagedPages != before.ManagedPages ||
                after.ConventionalPages != before.ConventionalPages ||
                after.AllocatedPages != before.AllocatedPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages ||
                after.ReservedPages() != before.ReservedPages())
                Diagnostics::Fatal("Memory", "boot-memory reclamation accounting invariant failed");

            if (before.FreePages > ~Uint64{ 0 } - result.ReleasedPages)
                Diagnostics::Fatal("Memory", "boot-memory reclamation free-page accounting overflowed");

            if (after.FreePages != before.FreePages + result.ReleasedPages)
                Diagnostics::Fatal("Memory", "reclaimed boot pages were not added to free memory");

            if (!memory.Physical.IsBootMemoryReclaimed())
                Diagnostics::Fatal("Memory", "boot-memory reclamation did not enter completed state");

            Diagnostics::Write("[zOS/Memory] Reclaimed boot/loader memory: ");
            Diagnostics::WriteDecimal(result.ReleasedPages);
            Diagnostics::Write(" pages (");
            Diagnostics::WriteDecimal(result.ReleasedBytes());
            Diagnostics::Write(" bytes).\n");

            Diagnostics::Write("[zOS/Memory] Free memory after reclamation: ");
            Diagnostics::WriteDecimal(after.FreeBytes());
            Diagnostics::Write(" bytes.\n");

            SelfTest::VerifyBootMemoryReclamation(memory);
        }

        void PreparePrimaryStack(State& memory) noexcept {
            const auto error = memory.PrimaryStack.Initialize(memory.Physical, memory.KernelAddresses, memory.KernelPageMap, PrimaryKernelStackPages);
            if (error != KernelStackInitializationError::Success)
                Diagnostics::Fatal("Stack", KernelStack::Describe(error));

            const VirtualSpan span = memory.PrimaryStack.UsableSpan();
            Diagnostics::Write("[zOS/Stack] Permanent kernel stack: ");
            Diagnostics::WriteHex(span.Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(span.SizeBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Stack] Lower guard: ");
            Diagnostics::WriteHex(memory.PrimaryStack.LowerGuard().Value());
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Stack] Upper guard: ");
            Diagnostics::WriteHex(memory.PrimaryStack.UpperGuard().Value());
            Diagnostics::Write("\n");
        }

        void VerifyPrimaryStackActive(State& memory) noexcept {
            Uint64 rsp{};

            __asm__ volatile(
                "mov %%rsp, %0"
                : "=r"(rsp)
            );

            if (!memory.PrimaryStack.Contains(VirtualAddress(rsp)))
                Diagnostics::Fatal("Stack", "RSP did not transition to the permanent kernel stack");

            if (memory.KernelPageMap.IsMapped(memory.PrimaryStack.LowerGuard()) ||
                memory.KernelPageMap.IsMapped(memory.PrimaryStack.UpperGuard()))
                Diagnostics::Fatal("Stack", "permanent kernel stack guard page is mapped");

            Diagnostics::Write("[zOS/Stack] Permanent kernel stack active.\n");
        }

        void ReleaseBootstrapResources(State& memory, Boot::Context& boot) noexcept {
            if (!boot.Initialized)
                Diagnostics::Fatal("Startup", "boot context is not initialized");

            UnmapIdentitySpan(memory.KernelPageMap, boot.BootstrapStack);
            UnmapIdentitySpan(memory.KernelPageMap, boot.EnvironmentStorage);
            UnmapIdentitySpan(memory.KernelPageMap, boot.MemoryMapStorage);

            const PhysicalMemoryStatistics before = memory.Physical.Statistics();
            BootstrapResourceReleaseResult result{};
            const auto error = memory.Physical.ReleaseBootstrapResources(result );
            if (error != BootstrapResourceReleaseError::Success)
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const PhysicalMemoryStatistics after = memory.Physical.Statistics();
            if (before.FreePages > ~Uint64{ 0 } - result.ReleasedPages ||
                after.FreePages != before.FreePages + result.ReleasedPages ||
                after.AllocatedPages != before.AllocatedPages ||
                after.DeferredBootPages != before.DeferredBootPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages ||
                before.ReservedPages() < result.ReleasedPages ||
                after.ReservedPages() != before.ReservedPages() - result.ReleasedPages)
                Diagnostics::Fatal("Memory", "bootstrap release accounting invariant failed");

            if (!memory.Physical.AreBootstrapResourcesReleased())
                Diagnostics::Fatal("Memory", "bootstrap resources did not enter release state");

            boot.BootstrapStack = {};
            boot.EnvironmentStorage = {};
            boot.MemoryMapStorage = {};

            Diagnostics::Write("[zOS/Memory] Released bootstrap resources: ");
            Diagnostics::WriteDecimal(result.ReleasedPages);
            Diagnostics::Write(" pages (");
            Diagnostics::WriteDecimal(result.ReleasedBytes());
            Diagnostics::Write(" bytes).\n");
        }

        void InitializeHeap(State& memory) noexcept {
            const auto error = memory.Heap.Initialize(memory.Physical, memory.KernelAddresses, memory.KernelPageMap);
            if (error != KernelHeapError::Success)
                Diagnostics::Fatal("Heap", KernelHeap::Describe(error));

            if (!memory.Heap.Validate())
                Diagnostics::Fatal("Heap", "initial kernel heap validation failed");

            const auto& statistics = memory.Heap.Statistics();

            Diagnostics::Write("[zOS/Heap] Permanent kernel heap initialized.\n");
            Diagnostics::Write("[zOS/Heap] Arena: ");
            Diagnostics::WriteHex(memory.Heap.ArenaSpan().Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(statistics.ReservedBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Heap] Initial committed memory: ");
            Diagnostics::WriteDecimal(statistics.CommittedBytes());
            Diagnostics::Write(" bytes\n");

            SelfTest::VerifyKernelHeap(memory);
        }

        void BindCxxAllocation(State& memory) noexcept {
            if (!memory.Heap.IsInitialized() || !memory.Heap.Validate())
                Diagnostics::Fatal("C++ Runtime", "cannot bind C++ allocation before permanent heap validation");

            if (!::Zos::Kernel::Runtime::BindKernelHeap(memory.Heap))
                Diagnostics::Fatal("C++ Runtime", "failed to bind permanent heap to C++ allocation runtime");

            if (!::Zos::Kernel::Runtime::IsKernelHeapBound())
                Diagnostics::Fatal("C++ Runtime", "C++ allocation runtime did not enter bound state");

            Diagnostics::Write("[zOS/Runtime] C++ allocation operators bound to permanent kernel heap.\n");

            SelfTest::VerifyCxxAllocation(memory);
        }

        void PromoteVirtualMetadata(State& memory) {
            auto& addresses = memory.KernelAddresses;
            auto& mmio_addresses = memory.MmioAddresses;
            auto& heap = memory.Heap;

            if (!addresses.IsInitialized() || !addresses.Validate() ||
                !mmio_addresses.IsInitialized() || !mmio_addresses.Validate() ||
                !heap.IsInitialized() || !heap.Validate())
                Diagnostics::Fatal("VMM", "VAA metadata promotion dependencies are invalid");

            if (addresses.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "VAA metadata was promoted more than once");

            const VirtualAddressAllocatorStatistics baseline = addresses.Statistics();
            VirtualReservation crossing_reservation{};
            VirtualAllocationConstraints constraints{
                .Alignment = 64 * 1024,
                .Preference = VirtualAllocationPreference::LowAddresses,
            };

            const auto reserve_error = addresses.Reserve(3, crossing_reservation,constraints );
            if (reserve_error != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(reserve_error));

            const auto promotion_error = addresses.PromoteMetadata(heap);
            if (promotion_error != VirtualAddressMetadataPromotionError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(promotion_error));

            if (!addresses.IsMetadataPromoted() || !addresses.Validate())
                Diagnostics::Fatal("VMM", "VAA did not enter valid permanent metadata state");

            const auto mmio_promotion_error = mmio_addresses.PromoteMetadata(heap);
            if (mmio_promotion_error != VirtualAddressMetadataPromotionError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(mmio_promotion_error));

            if (!mmio_addresses.IsMetadataPromoted() || !mmio_addresses.Validate())
                Diagnostics::Fatal("VMM", "MMIO VAA did not enter valid permanent metadata state");

            if (addresses.Release(crossing_reservation) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "pre-promotion virtual reservation did not survive metadata migration");

            VirtualReservation permanent_first{};
            VirtualReservation permanent_second{};

            const Uint64 heap_allocations_before = heap.Statistics().AllocationCount;
            if (addresses.Reserve(5, permanent_first, constraints) != VirtualAllocationError::Success ||
                addresses.Reserve(7, permanent_second, constraints) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "heap-backed VAA reservation failed");

            if (!addresses.Validate())
                Diagnostics::Fatal("VMM", "heap-backed VAA metadata failed validation");

            if (heap.Statistics().AllocationCount <= heap_allocations_before)
                Diagnostics::Fatal("VMM", "post-promotion VAA did not allocate permanent metadata");

            if (addresses.Release(permanent_second) != VirtualAllocationError::Success ||
                addresses.Release(permanent_first) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "heap-backed VAA reservation release failed");

            const auto& final = addresses.Statistics();
            if (final.ManagedPages != baseline.ManagedPages ||
                final.FreePages != baseline.FreePages ||
                final.ReservedPages != baseline.ReservedPages ||
                final.FreeExtentCount != baseline.FreeExtentCount ||
                final.ActiveReservations != baseline.ActiveReservations)
                Diagnostics::Fatal("VMM", "VAA ownership accounting changed during metadata promotion");

            Diagnostics::Write("[zOS/VMM] VAA metadata promoted to permanent kernel heap.\n");
            Diagnostics::Write("[zOS/VMM] Reservation continuity and permanent-metadata self-test passed.\n");
        }

        void PromotePageMapMetadata(State& memory) noexcept {
            PageMap& page_map = memory.KernelPageMap;
            BootstrapMetadataArena& bootstrap_metadata = memory.BootstrapMetadata;
            if (!page_map.IsInitialized() || !page_map.IsActive() || !page_map.Validate())
                Diagnostics::Fatal("VMM", "PageMap metadata promotion dependencies are invalid");

            if (page_map.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "PageMap metadata was promoted more than once");

            const PhysicalAddress root_before = page_map.RootTable();
            const PhysicalAddress cr3_before = PageMap::CurrentRootTable();
            const PageMapStatistics statistics_before = page_map.Statistics();
            const Uint64 bootstrap_bytes_before = bootstrap_metadata.Statistics().BytesRequested;
            const auto error = page_map.PromoteMetadata();
            if (error != PageMapMetadataPromotionError::Success)
                Diagnostics::Fatal("VMM", PageMap::Describe(error));

            if (!page_map.IsMetadataPromoted() || !page_map.Validate())
                Diagnostics::Fatal("VMM", "PageMap did not enter valid permanent metadata state");

            if (page_map.RootTable() != root_before ||
                PageMap::CurrentRootTable() != cr3_before ||
                page_map.Statistics().TablePages != statistics_before.TablePages ||
                page_map.Statistics().MappedPages != statistics_before.MappedPages)
                Diagnostics::Fatal("VMM", "PageMap topology changed during metadata promotion");

            if (bootstrap_metadata.Statistics().BytesRequested != bootstrap_bytes_before)
                Diagnostics::Fatal("VMM", "PageMap promotion allocated bootstrap metadata");

            Diagnostics::Write("[zOS/VMM] PageMap metadata promoted to permanent storage.\n");
        }

        void RetireBootstrapMetadataMappings(State& memory) noexcept {
            BootstrapMetadataArena& arena = memory.BootstrapMetadata;
            PageMap& page_map = memory.KernelPageMap;

            if (!arena.IsInitialized() || !arena.UsesDirectMapAccess() ||
                !page_map.IsActive() || !page_map.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "bootstrap metadata mapping retirement dependencies are invalid");

            const Uint64 page_count = arena.BackingPageCount();
            if (page_count == 0)
                Diagnostics::Fatal("VMM", "bootstrap metadata arena has no backing pages");

            for (Uint64 i = 0; i < page_count; ++i) {
                const PhysicalAddress physical = arena.BackingPage(i);
                if (physical.IsNull())
                    Diagnostics::Fatal("VMM", "bootstrap metadata backing chain is invalid");

                const VirtualAddress direct = Layout::DirectMapAddress(physical);
                if (direct.IsNull())
                    Diagnostics::Fatal("VMM", "bootstrap metadata lies outside the direct map");

                const auto direct_before = page_map.Translate(direct);
                if (!direct_before.Mapped || direct_before.Physical != physical)
                    Diagnostics::Fatal("VMM", "bootstrap metadata lost direct-map coverage");

                const VirtualAddress identity{ physical.Value() };
                const auto identity_before = page_map.Translate(identity);
                if (identity_before.Mapped) {
                    if (identity_before.Physical != physical)
                        Diagnostics::Fatal("VMM", "bootstrap metadata identity alias maps the wrong physical page");

                    const auto error = page_map.UnmapPage(identity);
                    if (error != MappingError::Success)
                        Diagnostics::Fatal("VMM", PageMap::Describe(error));

                    if (page_map.IsMapped(identity))
                        Diagnostics::Fatal("VMM", "bootstrap metadata identity alias survived retirement");
                }

                const auto direct_after = page_map.Translate(direct);
                if (!direct_after.Mapped || direct_after.Physical != physical)
                    Diagnostics::Fatal("VMM", "bootstrap metadata lost direct-map coverage during identity retirement");
            }

            Diagnostics::Write("[zOS/VMM] Bootstrap metadata identity aliases retired.\n");
        }

        void RetireBootstrapMetadata(State& memory) noexcept {
            BootstrapMetadataArena& arena = memory.BootstrapMetadata;
            PhysicalMemoryManager& physical_memory = memory.Physical;
            if (!memory.KernelAddresses.IsMetadataPromoted() || !memory.MmioAddresses.IsMetadataPromoted() || !memory.KernelPageMap.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "bootstrap metadata still has permanent consumers");

            if (!arena.IsInitialized() || arena.IsRetired())
                Diagnostics::Fatal("VMM", "bootstrap metadata arena is not in a retireable state");

            RetireBootstrapMetadataMappings(memory);

            const PhysicalMemoryStatistics before = physical_memory.Statistics();
            const Uint64 expected_pages = arena.BackingPageCount();
            if (expected_pages == 0 ||
                before.AllocatedPages < expected_pages ||
                before.FreePages > MaximumValue - expected_pages)
                Diagnostics::Fatal("Memory", "bootstrap metadata retirement accounting preflight failed");

            MetadataArenaRetirementResult result{};

            const auto error = arena.Retire(result);
            if (error != MetadataArenaRetirementError::Success)
                Diagnostics::Fatal("Memory", BootstrapMetadataArena::Describe(error));

            const PhysicalMemoryStatistics after = physical_memory.Statistics();
            if (result.ReleasedPages != expected_pages ||
                after.FreePages != before.FreePages + expected_pages ||
                after.AllocatedPages != before.AllocatedPages - expected_pages ||
                after.ManagedPages != before.ManagedPages ||
                after.ConventionalPages != before.ConventionalPages ||
                after.DeferredBootPages != before.DeferredBootPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages ||
                after.MetadataBytes != before.MetadataBytes ||
                after.ManagedRegionCount != before.ManagedRegionCount ||
                after.ReservedPages() != before.ReservedPages())
                Diagnostics::Fatal("Memory", "bootstrap metadata retirement accounting invariant failed");

            if (!arena.IsRetired() || arena.IsInitialized() || arena.BackingPageCount() != 0)
                Diagnostics::Fatal("Memory", "bootstrap metadata arena did not enter retired state");

            Diagnostics::Write("[zOS/VMM] Bootstrap metadata arena retired: ");
            Diagnostics::WriteDecimal(result.ReleasedPages);
            Diagnostics::Write(" pages (");
            Diagnostics::WriteDecimal(result.ReleasedBytes());
            Diagnostics::Write(" bytes).\n");
        }
    }

    void InitializeBootstrap(State& memory, const ::Zos::Boot::BootEnvironment& environment) noexcept {
        InitializePhysical(memory, environment);

        InitializeVirtual(memory);

        ActivateAddressSpace(memory, environment);

        PromotePhysicalMetadata(memory);
    }

    void PreparePermanentRuntime(State& memory, const InterruptManager& interrupts) noexcept {
        ReclaimBootMemory(memory, interrupts);

        PreparePrimaryStack(memory);
    }

    [[noreturn]] void SwitchToPermanentStack(State& memory, void(*continuation)(void*) noexcept, void* context) noexcept {
        Diagnostics::Write("[zOS/Stack] Switching away from loader-provided stack.\n");

        ::Zos::Kernel::SwitchToPermanentKernelStack(memory.PrimaryStack.Top().Value(), continuation, context);

        __builtin_unreachable();
    }

    void InitializePermanent(State& memory, Boot::Context& boot) noexcept {
        VerifyPrimaryStackActive(memory);

        ReleaseBootstrapResources(memory, boot);

        InitializeHeap(memory);
        BindCxxAllocation(memory);

        PromoteVirtualMetadata(memory);
        PromotePageMapMetadata(memory);
        RetireBootstrapMetadata(memory);

        SelfTest::VerifyPermanentMemory(memory);

        Diagnostics::Write("[zOS/Startup] Bootstrap memory infrastructure complete.\n");
    }
}
