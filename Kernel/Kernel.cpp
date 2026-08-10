#include <Boot/Protocol.hpp>

#include <Kernel/Architecture/AMD64/Paging.hpp>

#include <Kernel/Memory/PhysicalMemory.hpp>
#include <Kernel/Memory/VirtualMemory.hpp>

namespace {
    using namespace Zos;

    void WriteDebugChar(char value) noexcept {
        constexpr unsigned short DebugPort = 0xE9;
        __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(DebugPort));
    }

    void WriteDebug(const char* msg) noexcept {
        while (*msg != '\0') {
            WriteDebugChar(*msg);
            msg++;
        }
    }

    void WriteHex(Boot::Uint64 value) noexcept {
        static constexpr char Digits[] = "0123456789ABCDEF";
        WriteDebug("0x");

        bool emittedDigit = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto digit = static_cast<Boot::Uint8>((value >> shift) & 0xF);
            if (digit != 0 || emittedDigit || shift == 0) {
                WriteDebugChar(Digits[digit]);
                emittedDigit = true;
            }
        }
    }

    void WriteDecimal(Boot::Uint64 value) noexcept {
        char buffer[21];
        Boot::Uint64 position = sizeof(buffer);

        do {
            const auto digit = static_cast<Boot::Uint8>(value % 10);
            buffer[--position] = static_cast<char>('0' + digit);
            value /= 10;
        } while (value != 0);

        while (position < sizeof(buffer)) {
            WriteDebugChar(buffer[position]);
            ++position;
        }
    }

    [[noreturn]] void Halt() noexcept {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    [[noreturn]] void Fatal(const char* subsystem, const char* reason) noexcept {
        WriteDebug("[zSO/");
        WriteDebug(subsystem);
        WriteDebug("] FATAL: ");
        WriteDebug(reason);
        WriteDebug(".\n");
        Halt();
    }


    [[noreturn]] void RejectBootEnvironment(const char* reason) noexcept {
        Fatal("Kernel", reason);
    }

    void PrintSignature(Boot::Uint64 signature) {
        for (Boot::Uint32 i = 0; i < 8; i++) {
            Boot::Uint8 byte = static_cast<Boot::Uint8>((signature >> (i * 8)) & 0xFF);
            if (byte == '\0') break;
            WriteDebugChar(static_cast<char>(byte));
        }
    }

    void ValidateBootEnvironment(const Boot::BootEnvironment_V1& environment) noexcept {
        if (environment.Signature != Boot::EnvironmentSignature) 
            RejectBootEnvironment("signature mismatch");

        if (environment.Version != Boot::ProtocolVersion)
            RejectBootEnvironment("unsupported protocol version");

        if (environment.Size < sizeof(Boot::BootEnvironment_V1))
            RejectBootEnvironment("structure is too small");

        if (environment.KernelImage.Size == 0) 
            RejectBootEnvironment("kernel image range is empty");

        if (environment.KernelStack.Size == 0) 
            RejectBootEnvironment("kernel stack range is empty");

        if (environment.MemoryMapSize == 0 ||
            environment.MemoryMapDescriptorSize == 0 ||
            environment.MemoryMapSize > environment.MemoryMapStorage.Size ||
            (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0) 
            RejectBootEnvironment("memory map metadata is inconsistent");
    }

    void PrintBootEnvironment(const Boot::BootEnvironment_V1& environment) noexcept {
        WriteDebug("[zOS/Kernel] Boot protocol signature: ");
        PrintSignature(environment.Signature);
        WriteDebug("\n");

        WriteDebug("[zOS/Kernel] Boot protocol version: ");
        WriteDecimal(environment.Version);
        WriteDebug("\n");

        WriteDebug("[zOS/Kernel] Kernel image: ");
        WriteHex(environment.KernelImage.Base);
        WriteDebug(" + ");
        WriteDecimal(environment.KernelImage.Size);
        WriteDebug(" bytes\n");

        WriteDebug("[zOS/Kernel] Kernel stack: ");
        WriteHex(environment.KernelStack.Base);
        WriteDebug(" + ");
        WriteDecimal(environment.KernelStack.Size);
        WriteDebug(" bytes\n");

        WriteDebug("[zOS/Kernel] UEFI memory map: ");
        WriteDecimal(environment.MemoryMapSize);
        WriteDebug(" bytes, descriptor size ");
        WriteDecimal(environment.MemoryMapDescriptorSize);
        WriteDebug("\n");
    }

    void RunPhysicalMemorySelfTest(Kernel::Memory::PhysicalMemoryManager& manager) noexcept {
        using namespace Kernel::Memory;

        const Uint64 free_pages_before = manager.Statistics().FreePages;
        const Uint64 allocated_pages_before = manager.Statistics().AllocatedPages;

        PhysicalAllocation first{};
        PhysicalAllocation second{};
        PhysicalAllocation contiguous{};

        PhysicalAllocationError error = manager.AllocatePage(first);
        if (error != PhysicalAllocationError::Success)
            Fatal("Memory", PhysicalMemoryManager::Describe(error));

        error = manager.AllocatePage(second);
        if (error != PhysicalAllocationError::Success)
            Fatal("Memory", PhysicalMemoryManager::Describe(error));

        if (first.Base() == second.Base() 
         || !first.Base().IsPageAligned()
         || !second.Base().IsPageAligned())
         Fatal("Memory", "single-page allocation invariant failed");

        PhysicalAllocationConstraints dma32 = PhysicalAllocationConstraints::Dma32();
        dma32.Alignment = 64 * 1024;
        error = manager.AllocateContiguous(4, contiguous, dma32);
        if (error != PhysicalAllocationError::Success) 
            Fatal("Memory", PhysicalMemoryManager::Describe(error));
        
        const Uint64 contiguous_end = contiguous.Base().Value() + contiguous.SizeBytes() - 1;
        if (contiguous.Base().Value() > Dma32AddressLimit
         || contiguous_end > Dma32AddressLimit
         || (contiguous.Base().Value() & ((64 * 1024) - 1)) != 0) {
            WriteHex(contiguous.Base().Value());
            Fatal("Memory", "DMA32 allocation constraints were violated");
        }
        

        if (manager.Release(second) != PhysicalAllocationError::Success)
            Fatal("Memory", "single-page release validation failed");

        if (manager.Release(second) != PhysicalAllocationError::CorruptAllocation)
            Fatal("Memory", "double-free protection failed");

        if (manager.Release(first) != PhysicalAllocationError::Success ||
            manager.Release(contiguous) != PhysicalAllocationError::Success) 
            Fatal("Memory", "allocation release validation failed");
        
        if (manager.Statistics().FreePages != free_pages_before ||
            manager.Statistics().AllocatedPages != allocated_pages_before) 
            Fatal("Memory", "allocation accounting did not return to baseline");
        
        WriteDebug("[zOS/Memory] Allocation, DMA32, and double-free self-test passed.\n");
    }

    void RunVirtualMemoryInfrastructureSelfTest(Kernel::Memory::PhysicalMemoryManager& physical_memory, Kernel::Memory::BootstrapMetadataArena& metadata, Kernel::Memory::VirtualAddressAllocator& kernel_addresses, Kernel::Architecture::AMD64::PageMap& page_map) noexcept {
        using namespace Kernel::Memory;
        using Kernel::Architecture::AMD64::MappingError;
        using Kernel::Architecture::AMD64::PageMap;
        using Kernel::Architecture::AMD64::PageMapInitializationError;

        const MetadataArenaInitializationError metadata_error = metadata.Initialize(physical_memory);
        if (metadata_error != MetadataArenaInitializationError::Success) 
            Fatal("VMM194", BootstrapMetadataArena::Describe(metadata_error));

        VirtualAllocationError virtual_error = kernel_addresses.Initialize(Layout::KernelDynamicSpan(), metadata);
        if (virtual_error != VirtualAllocationError::Success) 
            Fatal("VMM198", VirtualAddressAllocator::Describe(virtual_error));

        const Uint64 free_virtual_pages_before = kernel_addresses.Statistics().FreePages;

        VirtualReservation lead_reservation{};
        virtual_error = kernel_addresses.Reserve(1, lead_reservation);
        if (virtual_error != VirtualAllocationError::Success) 
            Fatal("VMM205", VirtualAddressAllocator::Describe(virtual_error));

         VirtualReservation reservation{};
        VirtualAllocationConstraints virtual_constraints{};
        virtual_constraints.Alignment = 64 * 1024;
        virtual_error = kernel_addresses.Reserve(4, reservation, virtual_constraints);
        if (virtual_error != VirtualAllocationError::Success)
            Fatal("VMM212", VirtualAddressAllocator::Describe(virtual_error));

        if (!reservation.Base().IsPageAligned() ||
            (reservation.Base().Value() & ((64 * 1024) - 1)) != 0 ||
            reservation.PageCount() != 4 ||
            reservation.Base() == lead_reservation.Base()) {
            Fatal("VMM218", "virtual reservation alignment invariant failed");
        }

        PhysicalAllocation backing{};
        PhysicalAllocationError physical_error = physical_memory.AllocateContiguous(4, backing);
        if (physical_error != PhysicalAllocationError::Success) 
            Fatal("VMM224", PhysicalMemoryManager::Describe(physical_error));

        const PageMapInitializationError page_map_error = page_map.Initialize(physical_memory, metadata);
        if (page_map_error != PageMapInitializationError::Success) 
            Fatal("VMM228", PageMap::Describe(page_map_error));

        const MappingOptions options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Global,
            .Cache = CachePolicy::WriteBack,
        };

        for (Uint64 page = 0; page < reservation.PageCount(); page++) {
            const VirtualAddress virtual_address{ reservation.Base().Value() + page * PageSize };
            const PhysicalAddress physical_address{ backing.Base().Value() + page * PageSize };
            const MappingError mapping_error = page_map.MapPage(virtual_address, physical_address, options);
            if (mapping_error != MappingError::Success) 
                Fatal("VMM240", PageMap::Describe(mapping_error));
        }

        const VirtualAddress probe_virtual(reservation.Base().Value() + PageSize + 0x2A5);
        const PhysicalAddress expected_physical(backing.Base().Value() + PageSize + 0x2A5);
        const auto translation = page_map.Translate(probe_virtual);
        if (!translation.Mapped ||
            translation.Physical != expected_physical ||
            !HasAccess(translation.Options.Access, PageAccess::Read) ||
            !HasAccess(translation.Options.Access, PageAccess::Write) ||
            !HasAccess(translation.Options.Access, PageAccess::Global) ||
            HasAccess(translation.Options.Access, PageAccess::Execute) ||
            translation.Options.Cache != CachePolicy::WriteBack) {
            Fatal("VMM", "page-table translation or permission decoding failed");
        }

        if (page_map.MapPage(reservation.Base(), backing.Base(), options) != MappingError::AlreadyMapped)
            Fatal("VMM", "duplicate mapping protection failed");

        const MappingOptions invalid_options{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::Execute,
            .Cache = CachePolicy::WriteBack,
        };
        if (page_map.MapPage(VirtualAddress(reservation.Base().Value() + reservation.SizeBytes()), backing.Base(), invalid_options) != MappingError::InvalidPermissions)
            Fatal("VMM", "W^X mapping policy failed");
        
        if (page_map.MapPage(VirtualAddress(0), backing.Base(), options) != MappingError::InvalidVirtualAddress)
            Fatal("VMM", "null-region mapping protection failed");

        for (Uint64 page = 0; page < reservation.PageCount(); page++) {
            const VirtualAddress virtual_address(reservation.Base().Value() + page * PageSize);
            if (page_map.UnmapPage(virtual_address) != MappingError::Success) 
                Fatal("VMM", "page-table unmap failed");
            if (page_map.IsMapped(virtual_address))
                Fatal("VMM", "unmapped virtual page still translates");
        }

        if (page_map.UnmapPage(reservation.Base()) != MappingError::NotMapped)
            Fatal("VMM", "duplicate unmap protection failed");

        if (physical_memory.Release(backing) != PhysicalAllocationError::Success) 
            Fatal("VMM", "physical backing release failed");

        if (kernel_addresses.Release(reservation) != VirtualAllocationError::Success)
            Fatal("VMM", "aligned virtual reservation release failed");

        if (kernel_addresses.Release(lead_reservation) != VirtualAllocationError::Success)
            Fatal("VMM", "lead virtual reservation release failed");

        if (kernel_addresses.Statistics().FreePages != free_virtual_pages_before ||
            kernel_addresses.Statistics().ReservedPages != 0 ||
            kernel_addresses.Statistics().FreeExtentCount != 1) {
            Fatal("VMM", "virtual address accounting did not return to baseline");
        }

        if (page_map.Statistics().MappedPages != 0 || page_map.Statistics().TablePages != 1)
            Fatal("VMM", "page-table cleanup did not return to root-only state");

        WriteDebug("[zOS/VMM] Virtual range allocator initialized.\n");
        WriteDebug("[zOS/VMM] Test page-map root: ");
        WriteHex(page_map.RootTable().Value());
        WriteDebug("\n");
        WriteDebug("[zOS/VMM] Page-table pages retained: ");
        WriteDecimal(page_map.Statistics().TablePages);
        WriteDebug("\n");
        WriteDebug("[zOS/VMM] Bootstrap metadata pages: ");
        WriteDecimal(metadata.Statistics().PageCount);
        WriteDebug("\n");
        WriteDebug("[zOS/VMM] Mapping, translation, W^X, null-guard, and release self-test passed.\n");
        WriteDebug("[zOS/VMM] Test page map remains inactive; CR3 is unchanged.\n");
    }
}

extern "C" [[noreturn]] __attribute__((section(".text.KernelMain"))) void KernelMain(const Zos::Boot::BootEnvironment_V1* environment) noexcept {
    using namespace Zos::Boot;
    
    WriteDebug("\n\n[zOS/Kernel]=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
    WriteDebug("[zOS/Kernel]           Kernel entry reached\n");
    WriteDebug("[zOS/Kernel]=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");

    if (environment == nullptr)
        RejectBootEnvironment("boot environment pointer is null");

    ValidateBootEnvironment(*environment);
    PrintBootEnvironment(*environment);

    WriteDebug("[zOS/Kernel] Firmware handoff validated.\n");

    Kernel::Memory::PhysicalMemoryManager physical_memory{};
    const Kernel::Memory::PhysicalMemoryInitializationError initialization = physical_memory.Initialize(*environment);
    if (initialization != Kernel::Memory::PhysicalMemoryInitializationError::Success) 
        Fatal("Memory", Kernel::Memory::PhysicalMemoryManager::Describe(initialization));

    const Kernel::Memory::PhysicalMemoryStatistics& statistics = physical_memory.Statistics();
    const Kernel::Memory::PhysicalSpan metadata = physical_memory.MetadataSpan();

    WriteDebug("[zOS/Memory] Physical allocator initialized.\n");
    WriteDebug("[zOS/Memory] Conventional memory: ");
    WriteDecimal(statistics.ConventionalBytes());
    WriteDebug(" bytes\n");
    WriteDebug("[zOS/Memory] Free memory: ");
    WriteDecimal(statistics.FreeBytes());
    WriteDebug(" bytes\n");
    WriteDebug("[zOS/Memory] Reserved conventional pages: ");
    WriteDecimal(statistics.ReservedConventionalPages());
    WriteDebug("\n");
    WriteDebug("[zOS/Memory] Deferred boot-service memory: ");
    WriteDecimal(statistics.DeferredBootBytes());
    WriteDebug(" bytes\n");
    WriteDebug("[zOS/Memory] Deferred ACPI reclaim memory: ");
    WriteDecimal(statistics.DeferredAcpiBytes());
    WriteDebug(" bytes\n");
    WriteDebug("[zOS/Memory] Page-state metadata: ");
    WriteHex(metadata.Base.Value());
    WriteDebug(" + ");
    WriteDecimal(metadata.SizeBytes());
    WriteDebug(" bytes (");
    WriteDecimal(statistics.MetadataBytes);
    WriteDebug(" used)\n");

    RunPhysicalMemorySelfTest(physical_memory);
    WriteDebug("[zOS/Kernel] Physical memory ownership established.\n");

    Kernel::Memory::BootstrapMetadataArena virtual_metadata{};
    Kernel::Memory::VirtualAddressAllocator kernel_addresses{};
    Kernel::Architecture::AMD64::PageMap test_page_map{};

    RunVirtualMemoryInfrastructureSelfTest(physical_memory, virtual_metadata, kernel_addresses, test_page_map);
    WriteDebug("[zOS/Kernel] Virtual-Memory infrastructure established.\n");

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}