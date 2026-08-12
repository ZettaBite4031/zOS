#include <Kernel/Initialization/Initialization.hpp>

#include <Kernel/Kernel.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Initialization {
    namespace {
        using namespace Memory;
        using namespace Architecture::AMD64;

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

        void ValidateBootEnvironment(const Boot::BootEnvironment_V1& environment) noexcept {
            if (environment.Signature != Boot::EnvironmentSignature) 
                Diagnostics::Fatal("Startup", "boot environment signature mismatch");

            if (environment.Version != Boot::ProtocolVersion) 
                Diagnostics::Fatal("Startup", "unsupported boot protocol version");

            if (environment.Size < sizeof(Boot::BootEnvironment_V1)) 
                Diagnostics::Fatal("Startup", "boot environment structure is too small");

            if (environment.KernelImage.Size == 0) 
                Diagnostics::Fatal("Startup", "kernel image range is empty");

            if (environment.KernelStack.Size == 0) 
                Diagnostics::Fatal("Startup", "kernel stack range is empty");

            if (environment.MemoryMapStorage.Base == 0 || environment.MemoryMapSize == 0 ||
                environment.MemoryMapDescriptorSize == 0 || environment.MemoryMapSize > environment.MemoryMapStorage.Size ||
                (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0) 
                Diagnostics::Fatal("Startup", "firmware memory-map metadata is inconsistent");
        }

        void PrintBootEnvironment(const Boot::BootEnvironment_V1& environment) noexcept {
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

        void InitializePhysicalMemory(KernelRuntime& runtime, const Boot::BootEnvironment_V1& environment) noexcept {
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

        void ActivateKernelAddressSpace(KernelRuntime& runtime, const Boot::BootEnvironment_V1& environment) noexcept {
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
            
            Diagnostics::Write("[zOSInterrupt] GDT, TSS, and IDT established.\n");

            if (!runtime.Interrupts.RunBreakpointSelfTest()) 
                Diagnostics::Fatal("Interrupt", "INT3 round-trip self-test failed");
            
            Diagnostics::Write("[zOS/Interrupt] INT32 dispatch and IRETQ self-test passed.\n");
        }
    }

    void Bootstrap(const Boot::BootEnvironment_V1& environment, KernelRuntime& runtime) noexcept {
        if (runtime.Phase != KernelPhase::Entry) 
            Diagnostics::Fatal("Startup", "Kernel bootstrap was entered more than once");

        ValidateBootEnvironment(environment);
        PrintBootEnvironment(environment);

        AdvancePhase(runtime, KernelPhase::Entry, KernelPhase::BootEnvironmentValidated);
        Diagnostics::Write("[zOS/Startup] Firmware handoff validate.\n");

        InitializePhysicalMemory(runtime, environment);
        AdvancePhase(runtime, KernelPhase::BootEnvironmentValidated, KernelPhase::PhysicalMemoryReady);

        InitializeVirtualMemory(runtime);
        RunVirtualMemorySelfTest(runtime);
        AdvancePhase(runtime, KernelPhase::PhysicalMemoryReady, KernelPhase::VirtualMemoryReady);
        Diagnostics::Write("[zOS/Startup] Virtual-memory infrastructure established.\n");

        ActivateKernelAddressSpace(runtime, environment);
        AdvancePhase(runtime, KernelPhase::VirtualMemoryReady, KernelPhase::AddressSpaceActive);

        InitializeInterrupts(runtime);
        AdvancePhase(runtime, KernelPhase::AddressSpaceActive, KernelPhase::InterruptsReady);

        /*
         * This is the explicit architectural boundary.
         * 
         * Everything that needs permanent ownership now belongs
         * to KernelRuntime rather than KernelMain's stack frame.
         * 
         * The next roadmap milestone will insert firmware-memory
         * reclamation and boot-context internalizationbefore this
         * becomes Runtime.
         */
        AdvancePhase(runtime, KernelPhase::InterruptsReady, KernelPhase::BootstrapComplete);

        Diagnostics::Write("[zOS/Startup] Boostrap infrastructure complete.\n");
    }

    [[noreturn]] void EnterRuntime(KernelRuntime& runtime) noexcept {
        AdvancePhase(runtime, KernelPhase::BootstrapComplete, KernelPhase::Runtime);

        Diagnostics::Write("[zOS/Kernel] Permanent kernel runtime entered.\n");

        /*
         * No scheduler exists yet.
         * 
         * Maskable interrupts intentionally remain disabled until
         * APIC/IOAPIC initialization is implemented.
         */
        __asm__ volatile("cli");
        for(;;) __asm__ volatile("hlt");
    }
}