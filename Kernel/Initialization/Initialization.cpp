#include <Kernel/Initialization/Initialization.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

extern "C" [[noreturn]] void SwitchToPermanentKernelStack(Zos::Kernel::Memory::Uint64 stack_top, void(*continuation)(void*) noexcept, void* context) noexcept;

__asm__(
    ".pushsection "
        ".text.SwitchToPermanentKernelStack,"
        "\"ax\",@progbits\n"

    ".global SwitchToPermanentKernelStack\n"
    ".type SwitchToPermanentKernelStack,@function\n"

    "SwitchToPermanentKernelStack:\n"

    /*
     * SysV AMD64:
     * 
     * RDI = new stack top
     * RSI = continuation
     * RDX = continuation context
     */
    "   movq %rdi, %rsp\n"

    /*
     * There is intentionally no frame-chain relationship with the
     * discarded loader stack.
     */
    "   xorq %rbp, %rbp\n"

    /*
     * SysV requires DF clear on function entry.
     */
    "   cld\n"

    /*
     * First continuation argument = context.
     */
    "   movq %rdx, %rdi\n"

    /*
     * CALL intentionally creates a normal SysV entry condition:
     * the callee observes RSP % 16 == 8.
     */
    "   call *%rsi\n"

    /*
     * The continuation is contractually noreturn.
     */
    "   ud2\n"

    ".size SwitchToPermanentKernelStack, "
        ".-SwitchToPermanentKernelStack\n"

    ".popsection\n"
);


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

        void RunPhysicalMemorySelfTest(PhysicalMemoryManager& manager) noexcept {
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

        void RunVirtualMemorySelfTest(KernelRuntime& runtime) {
            auto& physical_memory = runtime.PhysicalMemory;
            auto& metadata = runtime.BootstrapMetadata;
            auto& kernel_addresses = runtime.KernelAddresses;
            auto& page_map = runtime.KernelPageMap;

            if (!physical_memory.IsInitialized() || !metadata.IsInitialized() ||
                !kernel_addresses.IsInitialized() || !page_map.IsInitialized()) 
                Diagnostics::Fatal("VMM", "virtual-memory self-test dependencies are not initialized");
            

            const Uint64 free_virtual_pages_before = kernel_addresses.Statistics(). FreePages;
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
                    Diagnostics::Fatal("VMM", PageMap::Describe(mapping_error));
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

            if (kernel_addresses.Statistics().FreePages != free_virtual_pages_before ||
                kernel_addresses.Statistics().ReservedPages != 0 ||
                kernel_addresses.Statistics().FreeExtentCount != 1) 
                Diagnostics::Fatal("VMM", "virtual address accounting did not return to baseline");

            if (page_map.Statistics().MappedPages != 0 ||
                page_map.Statistics().TablePages != 1) 
                Diagnostics::Fatal("VMM", "page-table cleanup did not return to root-only state");

            Diagnostics::Write("[zOS/VMM] Mapping, translation, W^X, null-guard, and release self-test passed.\n");
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

        void RunBootMemoryReclamationSelfTest(KernelRuntime& runtime) noexcept {
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

        void RunKernelHeapSelfTest(KernelRuntime& runtime) noexcept {
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
        
        

        AdvancePhase(runtime, KernelPhase::KernelHeapReady, KernelPhase::BootstrapComplete);
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