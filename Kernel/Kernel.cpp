#include <Boot/Protocol.hpp>

#include <Kernel/Memory/PhysicalMemory.hpp>

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

    WriteDebug("[zOS/Kernel] Physical memory ownership established");

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}