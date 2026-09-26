#include <Kernel/Initialization/Initialization.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

#include <Kernel/Runtime/New.hpp>

#include <Kernel/Architecture/AMD64/Stack.hpp>

namespace Zos::Kernel {
    void RunPhysicalMemorySelfTest(Memory::PhysicalMemoryManager& manager) noexcept;
    void RunVirtualMemorySelfTest(KernelRuntime& runtime);
    void RunBootMemoryReclamationSelfTest(KernelRuntime& runtime) noexcept;
    void RunKernelHeapSelfTest(KernelRuntime& runtime) noexcept;
    void RunCxxAllocationSelfTest(KernelRuntime& runtime) noexcept;
    void RunPermanentMemorySelfTest(KernelRuntime& runtime) noexcept;
}

namespace Zos::Kernel::Initialization {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

        inline constexpr Uint64 PrimaryKernelStackPages{ 16 };
        static_assert(PrimaryKernelStackPages * PageSize == 64 * 1024);

        void AdvancePhase(KernelRuntime& runtime, KernelPhase expected, KernelPhase next) noexcept {
            if (runtime.Phase != expected) 
                Diagnostics::Fatal("Startup", "kernel initialization phase ordering was violated");
            runtime.Phase = next;
        }

        void PrintSignature(Boot::Uint64 signature) noexcept {
            for (Boot::Uint32 i = 0; i < 8; i++) {
                const Boot::Uint8 byte = static_cast<Boot::Uint8>((signature >> (i * 8)) & 0xFF);
                if (byte == '\0') break;
                Diagnostics::WriteChar(static_cast<char>(byte));
            }
        }

        [[nodiscard]] bool IsPageRange(const Boot::PhysicalRange& range) noexcept {
            return range.Base != 0 && range.Size != 0 && (range.Base & (PageSize - 1)) == 0 && (range.Size & (PageSize - 1)) == 0;
        }

        [[nodiscard]] bool SamePhysicalMemoryStatistics(const PhysicalMemoryStatistics& left, const PhysicalMemoryStatistics& right) noexcept {
            return left.ManagedPages == right.ManagedPages
                && left.ConventionalPages == right.ConventionalPages
                && left.FreePages == right.FreePages
                && left.AllocatedPages == right.AllocatedPages
                && left.DeferredBootPages == right.DeferredBootPages
                && left.DeferredAcpiPages == right.DeferredAcpiPages
                && left.MetadataBytes == right.MetadataBytes
                && left.ManagedRegionCount == right.ManagedRegionCount;
        }

        void ValidateBootEnvironment(const Boot::BootEnvironment& environment) noexcept {
            if (environment.Signature != Boot::EnvironmentSignature) 
                Diagnostics::Fatal("Startup", "boot environment signature mismatch");

            if (environment.Version != Boot::ProtocolVersion) 
                Diagnostics::Fatal("Startup", "unsupported boot protocol version");

            if (environment.Size < sizeof(Boot::BootEnvironment)) 
                Diagnostics::Fatal("Startup", "boot environment structure is too small");

            if (!IsPageRange(environment.KernelImage) ||
                !IsPageRange(environment.KernelStack) ||
                !IsPageRange(environment.EnvironmentStorage) ||
                !IsPageRange(environment.MemoryMapStorage)) 
                Diagnostics::Fatal("Startup", "boot-owned physical ranges are not page aligned");

            if (environment.MemoryMapStorage.Base == 0 || environment.MemoryMapSize == 0 ||
                environment.MemoryMapDescriptorSize == 0 || environment.MemoryMapSize > environment.MemoryMapStorage.Size ||
                (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0) 
                Diagnostics::Fatal("Startup", "firmware memory-map metadata is inconsistent");

            if (environment.AcpiRsdp == 0) 
                Diagnostics::Fatal("Startup", "ACPI RSDP was not supplied");

            /*
             * Prove that this structure actually lives inside the
             * storage allocation the loader told us about;
             */
            const Uint64 environment_address = reinterpret_cast<Uint64>(&environment);
            if (environment_address < environment. EnvironmentStorage.Base || 
                environment_address - environment.EnvironmentStorage.Base > environment. EnvironmentStorage.Size - sizeof(Boot::BootEnvironment)) 
                Diagnostics::Fatal("Startup", "boot environment lies outside its declared storage");
        }

        void PrintBootEnvironment(const Boot::BootEnvironment& environment) noexcept {
            Diagnostics::Write("[zOS/Startup] Boot protocol signature: ");
            PrintSignature(environment.Signature);
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Startup] Boot protocol version: ");
            Diagnostics::WriteDecimal(environment.Version);
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Startup] Kernel image: ");
            Diagnostics::WriteHex(environment.KernelImage.Base);
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(environment.KernelImage.Size);
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Startup] Bootstrap stack: ");
            Diagnostics::WriteHex(environment.KernelStack.Base);
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(environment.KernelStack.Size);
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Startup] Firmware memory map: ");
            Diagnostics::WriteDecimal(environment.MemoryMapSize);
            Diagnostics::Write(" bytes, descriptor size ");
            Diagnostics::WriteDecimal(environment.MemoryMapDescriptorSize);
            Diagnostics::Write("\n");
        }

        void InitializePhysicalMemory(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept {
            const auto error = runtime.PhysicalMemory.Initialize(environment);
            if (error != PhysicalMemoryInitializationError::Success) 
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const auto& statistics = runtime.PhysicalMemory.Statistics();
            const PhysicalSpan metadata = runtime.PhysicalMemory.MetadataSpan();

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

            RunPhysicalMemorySelfTest(runtime.PhysicalMemory);

            Diagnostics::Write("[zOS/Startup] Physical memory ownership established.\n");
        }

        void InitializeVirtualMemory(KernelRuntime& runtime) noexcept {
            const auto metadata_error = runtime.BootstrapMetadata.Initialize(runtime.PhysicalMemory);
            if (metadata_error != MetadataArenaInitializationError::Success)
                Diagnostics::Fatal("VMM", BootstrapMetadataArena::Describe(metadata_error));

            const auto virtual_error = runtime.KernelAddresses.Initialize(Layout::KernelDynamicSpan(), runtime.BootstrapMetadata);
            if (virtual_error != VirtualAllocationError::Success) 
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(virtual_error));

            const auto page_map_error = runtime.KernelPageMap.Initialize(runtime.PhysicalMemory, runtime.BootstrapMetadata);
            if (page_map_error != PageMapInitializationError::Success) 
                Diagnostics::Fatal("VMM", PageMap::Describe(page_map_error));
        }

        void ActivateKernelAddressSpace(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept {
            /*
             * Deliberately startup-local.
             * 
             * The persistent obejct is KernelPageMap. KernelAddressSpace
             * contains policy required only to construct and activate it.
             */
            KernelAddressSpace address_space{ runtime.PhysicalMemory, runtime.BootstrapMetadata, runtime.KernelPageMap };
            const auto build_error = address_space.Build(environment);
            if (build_error != KernelAddressSpaceError::Success) 
                Diagnostics::Fatal("VMM", KernelAddressSpace::Describe(build_error));

            Diagnostics::Write("[zOS/VMM] Kernel address space constructed.\n");
            Diagnostics::Write("[zOS/VMM] New CR3 root: ");
            Diagnostics::WriteHex(runtime.KernelPageMap.RootTable().Value());
            Diagnostics::Write("\n");

            const auto activation_error = address_space.Activate();
            if (activation_error != KernelAddressSpaceError::Success) 
                Diagnostics::Fatal("VMM", KernelAddressSpace::Describe(activation_error));

            Diagnostics::Write("[zOS/VMM] Kernel-owned address space active.\n");
            Diagnostics::Write("[zOS/VMM] Active CR3: ");
            Diagnostics::WriteHex(PageMap::CurrentRootTable().Value());
            Diagnostics::Write("\n");
        }

        void InitializeInterrupts(KernelRuntime& runtime) noexcept {
            const auto error = runtime.Interrupts.Initialize(runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap);
            if (error != InterruptInitializationError::Success)
                Diagnostics::Fatal("Interrupt", InterruptManager::Describe(error));
            
            Diagnostics::Write("[zOS/Interrupt] GDT, TSS, and IDT established.\n");

            if (!runtime.Interrupts.RunBreakpointSelfTest()) 
                Diagnostics::Fatal("Interrupt", "INT3 round-trip self-test failed");
            
            Diagnostics::Write("[zOS/Interrupt] INT3 dispatch and IRETQ self-test passed.\n");
        }

        void ReclaimBootMemory(KernelRuntime& runtime) noexcept {
            PhysicalMemoryManager& physical_memory = runtime.PhysicalMemory;
            if (!physical_memory.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before PMM intialization");
            if (!runtime.KernelPageMap.IsActive())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before kernel address-space activation");
            if (!runtime.Interrupts.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot reclaim boot memory before interrupt infrastructure initialization");
            
            /*
             * Snapshot statistics by value. The reference returned by
             * Statistics() changes as reclamation proceeds.
             */
            const PhysicalMemoryStatistics before = physical_memory.Statistics();
            PhysicalMemoryReclamationResult result{};
            const PhysicalMemoryReclamationError error = physical_memory.ReclaimBootMemory(result);
            if (error != PhysicalMemoryReclamationError::Success)
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const PhysicalMemoryStatistics after = physical_memory.Statistics();
            
            /*
             * Every page previously marked DeferredBoot must have become
             * Free, and no other accounting category may change.
             */
            if (result.ReleasedPages != before.DeferredBootPages ||
                after.DeferredBootPages != 0 || after.ManagedPages != before.ManagedPages || 
                after.ConventionalPages != before.ConventionalPages ||
                after.AllocatedPages != before.AllocatedPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages ||
                after.ReservedPages() != before.ReservedPages()) 
                Diagnostics::Fatal("Memory", "boot-memory reclamation accounting invariant failed");

            if (before.FreePages > ~Uint64{ 0 } - result.ReleasedPages) 
                Diagnostics::Fatal("Memory", "boot-memory reclamation free-page accounting overflowed");

            if (after.FreePages != before.FreePages + result.ReleasedPages) 
                Diagnostics::Fatal("Memory", "reclaimed boot pages were not added to free memory");

            if (!physical_memory.IsBootMemoryReclaimed())
                Diagnostics::Fatal("Memory", "boot-memory reclamation did not enter the completed state");

            Diagnostics::Write("[zOS/Memory] Reclaimed boot/loader memory: ");
            Diagnostics::WriteDecimal(result.ReleasedPages);
            Diagnostics::Write(" pages (");
            Diagnostics::WriteDecimal(result.ReleasedBytes());
            Diagnostics::Write(" bytes).\n");

            Diagnostics::Write("[zOS/Memory] Free memory after reclamation: ");
            Diagnostics::WriteDecimal(after.FreeBytes());
            Diagnostics::Write(" bytes.\n");
        }

        [[nodiscard]] bool ConvertBootRange(const Boot::PhysicalRange& source, PhysicalSpan& destination) noexcept {
            if (!IsPageRange(source)) return false;
            destination = PhysicalSpan{ PhysicalAddress(source.Base), source.Size / PageSize };
            return !destination.IsEmpty();
        }

        void PreparePermanentKernelStack(KernelRuntime& runtime) noexcept {
            const auto error = runtime.PrimaryStack.Initialize(runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap, PrimaryKernelStackPages);
            if (error != KernelStackInitializationError::Success) 
                Diagnostics::Fatal("Stack", KernelStack::Describe(error));

            const VirtualSpan span = runtime.PrimaryStack.UsableSpan();
            Diagnostics::Write("[zOS/Stack] Permanent kernel stack: ");
            Diagnostics::WriteHex(span.Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(span.SizeBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Stack] Lower guard: ");
            Diagnostics::WriteHex(runtime.PrimaryStack.LowerGuard().Value());
            Diagnostics::Write("\n");

            Diagnostics::Write("[zOS/Stack] Upper guard: ");
            Diagnostics::WriteHex(runtime.PrimaryStack.UpperGuard().Value());
            Diagnostics::Write("\n");
        }

        void UnmapIdentitySpan(Architecture::AMD64::PageMap& page_map, PhysicalSpan span) noexcept {
            if (span.IsEmpty()) 
                Diagnostics::Fatal("VMM", "attempted to retire an empty identity span");

            for (Uint64 page = 0; page < span.PageCount; page++) {
                const PhysicalAddress physical = span.Base + page * PageSize;
                const VirtualAddress identity{ physical.Value() };
                const auto translation = page_map.Translate(identity);
                if (!translation.Mapped || translation.Physical != physical) 
                    Diagnostics::Fatal("VMM", "identity mapping is inconsistent");

                const auto error = page_map.UnmapPage(identity);
                if (error != Architecture::AMD64::MappingError::Success) 
                    Diagnostics::Fatal("VMM", Architecture::AMD64::PageMap::Describe(error));

                if (page_map.IsMapped(identity))
                    Diagnostics::Fatal("VMM", "retired identity page still maps");

                /*
                 * Retirement of the identity alias must never remove the
                 * authoritative direct-map alias.
                 */
                const VirtualAddress direct = Layout::DirectMapAddress(physical);
                if (direct.IsNull()) 
                    Diagnostics::Fatal("VMM", "physical page is outside the direct map");

                const auto direct_translation = page_map.Translate(direct);
                if (!direct_translation.Mapped || direct_translation.Physical != physical) 
                    Diagnostics::Fatal("VMM", "physical page lost direct-map coverage");
            }
        }

        void PromotePhysicalMemoryMetadata(KernelRuntime& runtime) noexcept {
            PhysicalMemoryManager& physical_memory = runtime.PhysicalMemory;
            PageMap& page_map = runtime.KernelPageMap;

            if (!physical_memory.IsInitialized())
                Diagnostics::Fatal("Memory", "cannot promote PMM metadata before initialization");
            
            if (!page_map.IsActive()) 
                Diagnostics::Fatal("Memory", "cannot promote PMM metadata before address-space activation");

            if (physical_memory.IsMetadataAccessPromoted())
                Diagnostics::Fatal("Memory", "PMM metadata was promoted more than once");

            const PhysicalSpan metadata = physical_memory.MetadataSpan();
            if (metadata.IsEmpty()) Diagnostics::Fatal("Memory", "PMM metadata span is empty");

            const VirtualAddress direct_base = Layout::DirectMapAddress(metadata.Base);
            if (direct_base.IsNull())
                Diagnostics::Fatal("Memory", "PMM metadata lies outside the direct map");

            const MappingOptions expected{
                .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
                .Cache = CachePolicy::WriteBack,
            };

            /*
             * Validate every direct-map page before giving that mapping to
             * the PMM as its permanent metadata view.
             */
            for (Uint64 page = 0; page < metadata.PageCount; page++) {
                const PhysicalAddress physical = metadata.Base + page * PageSize;
                const VirtualAddress direct = Layout::DirectMapAddress(physical);
                if (direct.IsNull()) Diagnostics::Fatal("Memory", "PMM metadata direct-map address is unavailable");

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

            if (!physical_memory.IsMetadataAccessPromoted() || physical_memory.MetadataAccessBase() != direct_base) 
                Diagnostics::Fatal("Memory", "PMM metadata promotion state is inconsistent");

            /*
             * Merely rebasing the metadata pointers must not change physical
             * ownership or allocator accounting.
             */
            const PhysicalMemoryStatistics after_promotion = physical_memory.Statistics();
            if (!SamePhysicalMemoryStatistics(before, after_promotion))
                Diagnostics::Fatal("Memory", "PMM metadatapromotion modified allocator accounting");
            
            Diagnostics::Write("[zOS/Memory] PMM metadata promoted to direct map at ");
            Diagnostics::WriteHex(direct_base.Value());
            Diagnostics::Write("\n");

            /*
             * From this point onward, m_Regions and m_PageStates both point
             * into the higher-half direct map, so the lower aliases is no longer
             * a PMM dependency.
             * 
             * UnmapPage() may reclaim now-empty page-table pages. Those
             * release themselves go through the newly-promoted PMM and
             * therefore provide an immediate real-world test of the new
             * metadata view. 
             */
            UnmapIdentitySpan(page_map, metadata);

            /*
             * Perform an explicit allocator round trip after the identity
             * alias is gone. If any persistent PMM pointer still referenced
             * the low mapping, this operating will fail.
             */
            const PhysicalMemoryStatistics baseline = physical_memory.Statistics();
            PhysicalAllocation probe{};
            const PhysicalAllocationError allocation_error = physical_memory.AllocatePage(probe);
            if (allocation_error != PhysicalAllocationError::Success) 
                Diagnostics::Fatal("Memory", "post-promotion PMM allocation failed");

            if (physical_memory.Release(probe) != PhysicalAllocationError::Success) 
                Diagnostics::Fatal("Memory", "post-promotion PMM release failed");

            const PhysicalMemoryStatistics final = physical_memory.Statistics();
            if (!SamePhysicalMemoryStatistics(baseline, final)) 
                Diagnostics::Fatal("Memory", "post-promotion PMM accounting did not return to baseline");

            Diagnostics::Write("[zOS/Memory] PMM identity alias retired.\n");
            Diagnostics::Write("[zOS/Memory] PMM direct-map self-test passed.\n");
        }

        void ReleaseBootstrapResources(KernelRuntime& runtime) noexcept {
            if (!runtime.Boot.Initialized) 
                Diagnostics::Fatal("Startup", "boot context is not initialized");

            /*
             * No CPU state, persistent object, or C++ frame may refer to
             * these identity aliases after this point.
             */
            UnmapIdentitySpan(runtime.KernelPageMap, runtime.Boot.BootstrapStack);
            UnmapIdentitySpan(runtime.KernelPageMap, runtime.Boot.EnvironmentStorage);
            UnmapIdentitySpan(runtime.KernelPageMap, runtime.Boot.MemoryMapStorage);

            /*
             * Page-table reclamation during UnmapPage() may itself modify
             * PMM Free/Allocated counts, so establish the accounting
             * baseline only after all three unmaps are complete
             */
            const PhysicalMemoryStatistics before = runtime.PhysicalMemory.Statistics();
            BootstrapResourceReleaseResult result{};
            const auto error = runtime.PhysicalMemory.ReleaseBootstrapResources(result);
            if (error != BootstrapResourceReleaseError::Success) 
                Diagnostics::Fatal("Memory", PhysicalMemoryManager::Describe(error));

            const PhysicalMemoryStatistics after = runtime.PhysicalMemory.Statistics();
            if (before.FreePages > ~Uint64{ 0 } - result.ReleasedPages) 
                Diagnostics::Fatal("Memory", "bootstrap release accounting overflowed");

            if (after.FreePages != before.FreePages + result.ReleasedPages ||
                after.AllocatedPages != before.AllocatedPages ||
                after.DeferredBootPages != before.DeferredBootPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages ||
                before.ReservedPages() < result.ReleasedPages ||
                after.ReservedPages() != before.ReservedPages() - result.ReleasedPages) 
                Diagnostics::Fatal("Memory", "bootstrap release accounting invariant failed");
            
            if (!runtime.PhysicalMemory.AreBootstrapResourcesReleased())
                Diagnostics::Fatal("Memory", "bootstrap resources did not enter release state");

            /*
             * Prevent later code from treating the old physical ranges as
             * live kernel-owened resources.
             */
            runtime.Boot.BootstrapStack = {};
            runtime.Boot.EnvironmentStorage = {};
            runtime.Boot.MemoryMapStorage = {};

            Diagnostics::Write("[zOS/Memory] Released bootstrap resources: ");
            Diagnostics::WriteDecimal(result.ReleasedPages);
            Diagnostics::Write(" pages (");
            Diagnostics::WriteDecimal(result.ReleasedBytes());
            Diagnostics::Write(" bytes).\n");
        }

        void ValidatePermanentStackActive(KernelRuntime& runtime) noexcept {
            Uint64 rsp = 0;
            __asm__ volatile(
                "mov %%rsp, %0"
                : "=r"(rsp)
            );

            if (!runtime.PrimaryStack.Contains(VirtualAddress(rsp))) 
                Diagnostics::Fatal("Stack", "RSP did not transition to the permanent kernel stack");

            if (runtime.KernelPageMap.IsMapped(runtime.PrimaryStack.LowerGuard()) || 
                runtime.KernelPageMap.IsMapped(runtime.PrimaryStack.UpperGuard())) 
                Diagnostics::Fatal("Stack", "permanent kernel stack guard page is mapped");
        }

        void InternalizeBootContext(KernelRuntime& runtime, const Boot::BootEnvironment& environment) noexcept {
            if (runtime.Boot.Initialized) 
                Diagnostics::Fatal("Startup", "boot context was internalized more than once");
            
            BootContext context{};
            if (!ConvertBootRange(environment.KernelImage, context.KernelImage) ||
                !ConvertBootRange(environment.KernelStack, context.BootstrapStack) ||
                !ConvertBootRange(environment.EnvironmentStorage, context.EnvironmentStorage) ||
                !ConvertBootRange(environment.MemoryMapStorage, context.MemoryMapStorage)) 
                Diagnostics::Fatal("Startup", "failed to internalize boot-owned physical ranges");
            
            context.AcpiRsdp = PhysicalAddress(environment.AcpiRsdp);
            if (context.AcpiRsdp.IsNull())
                Diagnostics::Fatal("Startup", "internalized ACPI RSDP is null");

            context.Initialized = true;
            runtime.Boot = context;

            /*
            * Verify all copied state while the original handoff is
            * still available.
            */
            if (runtime.Boot.KernelImage.Base != PhysicalAddress(environment.KernelImage.Base) ||
                runtime.Boot.KernelImage.SizeBytes() != environment.KernelImage.Size ||
                runtime.Boot.BootstrapStack.Base != PhysicalAddress(environment.KernelStack.Base) ||
                runtime.Boot.BootstrapStack.SizeBytes() != environment.KernelStack.Size ||
                runtime.Boot.EnvironmentStorage.Base != PhysicalAddress(environment.EnvironmentStorage.Base) ||
                runtime.Boot.EnvironmentStorage.SizeBytes() != environment.EnvironmentStorage.Size ||
                runtime.Boot.MemoryMapStorage.Base != PhysicalAddress(environment.MemoryMapStorage.Base) ||
                runtime.Boot.MemoryMapStorage.SizeBytes() != environment.MemoryMapStorage.Size ||
                runtime.Boot.AcpiRsdp != PhysicalAddress(environment.AcpiRsdp)) 
                Diagnostics::Fatal("Startup", "internalized boot context does not match the loader handoff");

            Diagnostics::Write("[zOS/Startup] Boot context internalized.\n");
            Diagnostics::Write("[zOS/Startup] ACPI RSDP: ");
            Diagnostics::WriteHex(runtime.Boot.AcpiRsdp.Value());
            Diagnostics::Write("\n");
        }

        void InitializeKernelHeap(KernelRuntime& runtime) noexcept {
            const KernelHeapError error = runtime.Heap.Initialize(runtime.PhysicalMemory, runtime.KernelAddresses, runtime.KernelPageMap);
            if (error != KernelHeapError::Success) 
                Diagnostics::Fatal("Heap", KernelHeap::Describe(error));
            if (!runtime.Heap.Validate())
                Diagnostics::Fatal("Heap", "initial kernel heap validation failed");

            const KernelHeapStatistics& statistics = runtime.Heap.Statistics();
            Diagnostics::Write("[zOS/Heap] Permanent kernel heap initialized.\n");
            Diagnostics::Write("[zOS/Heap] Arena: ");
            Diagnostics::WriteHex(runtime.Heap.ArenaSpan().Base.Value());
            Diagnostics::Write(" + ");
            Diagnostics::WriteDecimal(statistics.ReservedBytes());
            Diagnostics::Write(" bytes\n");

            Diagnostics::Write("[zOS/Heap] Initial committed memory: ");
            Diagnostics::WriteDecimal(statistics.CommittedBytes());
            Diagnostics::Write(" bytes\n");
        }

        void BindCxxAllocationRuntime(KernelRuntime& runtime) noexcept {
            if (!runtime.Heap.IsInitialized() || !runtime.Heap.Validate())
                Diagnostics::Fatal("C++ Runtime", "cannot bind C++ allocation before permanent heap validation");

            if (!Runtime::BindKernelHeap(runtime.Heap))
                Diagnostics::Fatal("C++ Runtime", "failed to bind permanent heap to C++ allocation runtime");

            if (!Runtime::IsKernelHeapBound())
                Diagnostics::Fatal("C++ Runtime", "C++ allocation runtime did not enter the bound state");

            Diagnostics::Write("[zOS/Runtime] C++ allocation operators bound to permanent kernel heap.\n");
        }

        void PromoteVirtualAddressMetadata(KernelRuntime& runtime) {
            auto& addresses = runtime.KernelAddresses;
            auto& heap = runtime.Heap;

            if (!addresses.IsInitialized() || 
                !addresses.Validate() || 
                !heap.IsInitialized() || 
                !heap.Validate()) 
                Diagnostics::Fatal("VMM", "VAA metadata promotion dependencies are invalid");

            if (addresses.IsMetadataPromoted()) 
                Diagnostics::Fatal("VMM", "VAA metadata was promoted more than once");

            const VirtualAddressAllocatorStatistics baseline = addresses.Statistics();

            /*
            * This token is deliberately created while the reservation record
            * still resides in BootstrapMetadataArena.
            *
            * It must remain valid after its record is replaced by the
            * heap-backed equivalent.
            */
            VirtualReservation crossing_reservation{};

            VirtualAllocationConstraints constraints{
                .Alignment = 64 * 1024,
                .Preference = VirtualAllocationPreference::LowAddresses,
            };

            const VirtualAllocationError reserve_error = addresses.Reserve(3, crossing_reservation, constraints);
            if (reserve_error != VirtualAllocationError::Success) 
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(reserve_error));

            const VirtualAddressMetadataPromotionError promotion_error = addresses.PromoteMetadata(heap);
            if (promotion_error != VirtualAddressMetadataPromotionError::Success)
                Diagnostics::Fatal("VMM", VirtualAddressAllocator::Describe(promotion_error));

            if (!addresses.IsMetadataPromoted() || !addresses.Validate()) 
                Diagnostics::Fatal("VMM","VAA did not enter valid permanent metadata state");

            /*
            * The pre-promotion token must now resolve to its cloned
            * heap-resident ReservationRecord by stable ID.
            */
            if (addresses.Release(crossing_reservation) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "pre-promotion virtual reservation did not survive metadata migration");

            VirtualReservation permanent_first{};
            VirtualReservation permanent_second{};

            const Uint64 heap_allocations_before = heap.Statistics().AllocationCount;
            if (addresses.Reserve(5, permanent_first, constraints) != VirtualAllocationError::Success) 
                Diagnostics::Fatal("VMM", "first heap-backed VAA reservation failed");

            /*
            * The crossing reservation left at most one recycled
            * ReservationRecord. Keeping the first reservation live means the
            * second reservation must acquire another record from the permanent
            * metadata backend.
            */
            if (addresses.Reserve(7, permanent_second, constraints) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "second heap-backed VAA reservation failed");

            if (!addresses.Validate())
                Diagnostics::Fatal("VMM", "heap-backed VAA metadata failed validation");

            if (heap.Statistics().AllocationCount <= heap_allocations_before)
                Diagnostics::Fatal("VMM", "post-promotion VAA did not allocate permanent metadata");

            if (addresses.Release(permanent_second) != VirtualAllocationError::Success || 
                addresses.Release(permanent_first) != VirtualAllocationError::Success)
                Diagnostics::Fatal("VMM", "heap-backed VAA reservation release failed");

            const auto& final = addresses.Statistics();

            /*
            * The temporary test reservations must leave the actual virtual
            * address ownership topology unchanged.
            */
            if (final.ManagedPages != baseline.ManagedPages || 
                final.FreePages != baseline.FreePages || 
                final.ReservedPages != baseline.ReservedPages || 
                final.FreeExtentCount != baseline.FreeExtentCount || 
                final.ActiveReservations != baseline.ActiveReservations)
                Diagnostics::Fatal("VMM", "VAA ownership accounting changed during metadata promotion");

            Diagnostics::Write("[zOS/VMM] VAA metadata promoted to permanent kernel heap.\n");
            Diagnostics::Write("[zOS/VMM] Reservation continuity and permanent-metadata self-test passed.\n");
        }

        void PromotePageMapMetadata(KernelRuntime& runtime) noexcept {
            PageMap& page_map = runtime.KernelPageMap;
            BootstrapMetadataArena& bootstrap_metadata = runtime.BootstrapMetadata;

            if (!page_map.IsInitialized() || !page_map.IsActive() || !page_map.Validate())
                Diagnostics::Fatal("VMM", "PageMap metadata promotion dependencies are invalid");
            if (page_map.IsMetadataPromoted()) Diagnostics::Fatal("VMM", "PageMap metadata was promoted more than once");

            const PhysicalAddress root_before = page_map.RootTable();
            const PhysicalAddress cr3_before = PageMap::CurrentRootTable();
            const PageMapStatistics statistics_before = page_map.Statistics();
            const Uint64 bootstrap_bytes_before = bootstrap_metadata.Statistics().BytesRequested;

            const PageMapMetadataPromotionError error = page_map.PromoteMetadata();
            if (error != PageMapMetadataPromotionError::Success) Diagnostics::Fatal("VMM", PageMap::Describe(error));
            if (!page_map.IsMetadataPromoted() || !page_map.Validate())
                Diagnostics::Fatal("VMM", "PageMap did not enter valid permanent metadata state");

            if (page_map.RootTable() != root_before || PageMap::CurrentRootTable() != cr3_before ||
                page_map.Statistics().TablePages != statistics_before.TablePages || page_map.Statistics().MappedPages != statistics_before.MappedPages)
                Diagnostics::Fatal("VMM", "PageMap topology changed during metadata promotion");

            if (bootstrap_metadata.Statistics().BytesRequested != bootstrap_bytes_before)
                Diagnostics::Fatal("VMM", "PageMap promotion allocated bootstrap metadata");

            Diagnostics::Write("[zOS/VMM] PageMap metadata promoted to permanent storage.\n");
        }

        void RetireBootstrapMetadataIdentityMappings(KernelRuntime& runtime) noexcept {
            BootstrapMetadataArena& arena = runtime.BootstrapMetadata;
            PageMap& page_map = runtime.KernelPageMap;

            if (!arena.IsInitialized() || !arena.UsesDirectMapAccess() || !page_map.IsActive() || !page_map.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "bootstrap metadata mapping retirement dependencies are invalid");

            const Uint64 page_count = arena.BackingPageCount();
            if (page_count == 0) Diagnostics::Fatal("VMM", "bootstrap metadata arena has no backing pages");

            for (Uint64 i = 0; i < page_count; i++) {
                const PhysicalAddress physical = arena.BackingPage(i);
                if (physical.IsNull()) Diagnostics::Fatal("VMM", "bootstrap metadata backing chain is invalid");

                const VirtualAddress direct = Layout::DirectMapAddress(physical);
                if (direct.IsNull()) Diagnostics::Fatal("VMM", "bootstrap metadata lies outside the direct map");

                const TranslationResult direct_before = page_map.Translate(direct);
                if (!direct_before.Mapped || direct_before.Physical != physical)
                    Diagnostics::Fatal("VMM", "bootstrap metadata lost direct-map coverage");

                const VirtualAddress identity{ physical.Value() };
                const TranslationResult identity_before = page_map.Translate(identity);
                if (identity_before.Mapped) {
                    if (identity_before.Physical != physical)
                        Diagnostics::Fatal("VMM", "bootstrap metadata identity alias maps the wrong physical page");

                    const MappingError error = page_map.UnmapPage(identity);
                    if (error != MappingError::Success) Diagnostics::Fatal("VMM", PageMap::Describe(error));
                    if (page_map.IsMapped(identity)) Diagnostics::Fatal("VMM", "bootstrap metadata identity alias survived retirement");
                }

                const TranslationResult direct_after = page_map.Translate(direct);
                if (!direct_after.Mapped || direct_after.Physical != physical)
                    Diagnostics::Fatal("VMM", "bootstrap metadata lost direct-map coverage during identity retirement");
            }

            Diagnostics::Write("[zOS/VMM] Bootstrap metadata identity aliases retired.\n");
        }

        void RetireBootstrapMetadata(KernelRuntime& runtime) noexcept {
            BootstrapMetadataArena& arena = runtime.BootstrapMetadata;
            PhysicalMemoryManager& physical_memory = runtime.PhysicalMemory;

            if (!runtime.KernelAddresses.IsMetadataPromoted() || !runtime.KernelPageMap.IsMetadataPromoted())
                Diagnostics::Fatal("VMM", "bootstrap metadata still has permanent consumers");
            if (!arena.IsInitialized() || arena.IsRetired())
                Diagnostics::Fatal("VMM", "bootstrap metadata arena is not in a retireable state");

            RetireBootstrapMetadataIdentityMappings(runtime);

            const PhysicalMemoryStatistics before = physical_memory.Statistics();
            const Uint64 expected_pages = arena.BackingPageCount();
            if (expected_pages == 0 || before.AllocatedPages < expected_pages || before.FreePages > MaximumValue - expected_pages)
                Diagnostics::Fatal("Memory", "bootstrap metadata retirement accounting preflight failed");

            MetadataArenaRetirementResult result{};
            const MetadataArenaRetirementError error = arena.Retire(result);
            if (error != MetadataArenaRetirementError::Success) Diagnostics::Fatal("Memory", BootstrapMetadataArena::Describe(error));

            const PhysicalMemoryStatistics after = physical_memory.Statistics();
            if (result.ReleasedPages != expected_pages || after.FreePages != before.FreePages + expected_pages ||
                after.AllocatedPages != before.AllocatedPages - expected_pages || after.ManagedPages != before.ManagedPages ||
                after.ConventionalPages != before.ConventionalPages || after.DeferredBootPages != before.DeferredBootPages ||
                after.DeferredAcpiPages != before.DeferredAcpiPages || after.MetadataBytes != before.MetadataBytes ||
                after.ManagedRegionCount != before.ManagedRegionCount || after.ReservedPages() != before.ReservedPages())
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

    [[noreturn]] void EnterRuntime(KernelRuntime& runtime) noexcept {
        AdvancePhase(runtime, KernelPhase::BootstrapComplete, KernelPhase::Runtime);

        Diagnostics::Write("[zOS/Kernel] Permanent kernel runtime entered.\n");

        /*
         * No scheduler exists yet.
         * 
         * Maskable interrupts remain disabled until APIC/IOAPIC
         * initialization establishes extern interrupt routing.
         */
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    [[noreturn]] void ContinueBootstrapOnPermanentStack(void* context) noexcept {
        if (context == nullptr) 
            Diagnostics::Fatal("Stack", "permanent-stack continuation context is null");

        auto& runtime = *static_cast<KernelRuntime*>(context);
        if (runtime.Phase != KernelPhase::PermanentStackPrepared)
            Diagnostics::Fatal("Startup", "permanent-state continuation phase is invalid");

        ValidatePermanentStackActive(runtime);

        AdvancePhase(runtime, KernelPhase::PermanentStackPrepared, KernelPhase::PermanentStackActive);
        Diagnostics::Write("[zOS/Stack] Permanent kernel stack active.\n");

        /*
         * We are now executing entirely from the new stack and use only
         * the internalized BootContext. The original BootEnvironment,
         * loader stack, and firmware-map storage are no longer live.
         */
        ReleaseBootstrapResources(runtime);
        if (!runtime.PhysicalMemory.AreBootstrapResourcesReleased())
            Diagnostics::Fatal("Memory", "bootstrap resources remain reserved");

        if (!runtime.Boot.BootstrapStack.IsEmpty() ||
            !runtime.Boot.EnvironmentStorage.IsEmpty() ||
            !runtime.Boot.MemoryMapStorage.IsEmpty()) 
            Diagnostics::Fatal("Startup", "released bootstrap ranges remain in BootContext");

        AdvancePhase(runtime, KernelPhase::PermanentStackActive, KernelPhase::BootstrapResourcesReleased);

        InitializeKernelHeap(runtime);
        RunKernelHeapSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::BootstrapResourcesReleased, KernelPhase::KernelHeapReady);
        
        /*
         * The raw heap is independently proven before the language runtime is
         * allowed to depend on it.
         */
        BindCxxAllocationRuntime(runtime);
        RunCxxAllocationSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::KernelHeapReady, KernelPhase::CxxAllocationReady);

        PromoteVirtualAddressMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::CxxAllocationReady, KernelPhase::VirtualAddressMetadataPromoted);

        PromotePageMapMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::VirtualAddressMetadataPromoted, KernelPhase::PageMapMetadataPromoted);

        RetireBootstrapMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::PageMapMetadataPromoted, KernelPhase::BootstrapMetadataRetired);

        RunPermanentMemorySelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::BootstrapMetadataRetired, KernelPhase::BootstrapComplete);
        Diagnostics::Write("[zOS/Startup] Bootstrap infrastructure complete.\n");

        EnterRuntime(runtime);
    }

    void Bootstrap(const Boot::BootEnvironment& environment, KernelRuntime& runtime) noexcept {
        if (runtime.Phase != KernelPhase::Entry) 
            Diagnostics::Fatal("Startup", "Kernel bootstrap was entered more than once");

        ValidateBootEnvironment(environment);
        PrintBootEnvironment(environment);

        AdvancePhase(runtime, KernelPhase::Entry, KernelPhase::BootEnvironmentValidated);
        Diagnostics::Write("[zOS/Startup] Firmware handoff validated.\n");

        InitializePhysicalMemory(runtime, environment);
        AdvancePhase(runtime, KernelPhase::BootEnvironmentValidated, KernelPhase::PhysicalMemoryReady);

        InitializeVirtualMemory(runtime);
        RunVirtualMemorySelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::PhysicalMemoryReady, KernelPhase::VirtualMemoryReady);
        Diagnostics::Write("[zOS/Startup] Virtual-memory infrastructure established.\n");

        ActivateKernelAddressSpace(runtime, environment);
        AdvancePhase(runtime, KernelPhase::VirtualMemoryReady, KernelPhase::AddressSpaceActive);
        
        /*
         * The first zOS-owned page map intentionally carried the PMM's low
         * metadata alias through CR3 activation. The permanent direct map is
         * now authoritative, so detach the PMM from that bootstrap mapping
         * before bringing up additional runtime infrastructure.
         */
        PromotePhysicalMemoryMetadata(runtime);
        AdvancePhase(runtime, KernelPhase::AddressSpaceActive, KernelPhase::PhysicalMemoryMetadataPromoted);

        InitializeInterrupts(runtime);
        AdvancePhase(runtime, KernelPhase::PhysicalMemoryMetadataPromoted, KernelPhase::InterruptsReady);

        /*
         * ExitBootServices has already occurred in the loader and zOS now
         * owns its page tabels and exception infrastructure.
         * 
         * Pages still classified DeferredBoot are no longer required by
         * any current bootstrap dependency. Explicitly reserved handoff
         * ranges remain Reserved and are therefore unaffected.
         */
        ReclaimBootMemory(runtime);
        RunBootMemoryReclamationSelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::InterruptsReady, KernelPhase::BootMemoryReclaimed);

        /*
         * Copy every remaining handoff fact into permanent
         * kernel-owned storage.
         * 
         * After this call no persistent subsystem has any reason
         * to retain BootEnvironment itself.
         */
        InternalizeBootContext(runtime, environment);
        AdvancePhase(runtime, KernelPhase::BootMemoryReclaimed, KernelPhase::BootContextInternalized);

        /*
         * Everything needed from BootEnvironment now exists in
         * KernelRuntime::Boot.
         * 
         * Prepare the permanent VMM-owned stack while the loader stack is
         * still valid.
         */
        PreparePermanentKernelStack(runtime);
        AdvancePhase(runtime, KernelPhase::BootContextInternalized, KernelPhase::PermanentStackPrepared);
        Diagnostics::Write("[zOS/Stack] Switching away from loader-provided stack.\n");

        /*
         * This call NEVER returns.
         * 
         * Returning would require touching the caller frame on the loader
         * stack, which the continuation will unmap and return to this PMM.
         */
        SwitchToPermanentKernelStack(runtime.PrimaryStack.Top().Value(), &ContinueBootstrapOnPermanentStack, &runtime);

        __builtin_unreachable();
    }
}