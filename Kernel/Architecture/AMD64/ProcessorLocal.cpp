#include <Kernel/Architecture/AMD64/ProcessorLocal.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    namespace {
        using Memory::Uint64;

        inline constexpr Memory::Uint32 GsBaseMsr{ 0xC0000101u };
        inline constexpr Memory::Uint32 KernelGsBaseMsr{ 0xC0000102u };

        [[nodiscard]] constexpr bool IsCanonical48BitAddress(Uint64 address) noexcept {
            const Uint64 upper = address >> 48;
            const bool sign = ((address >> 47) & 1ULL) != 0;
            return sign ? upper == 0xFFFFULL : upper == 0;
        }
    }

    ProcessorLocalInitializationError ProcessorLocal::Initialize(Memory::Uint32 processor_index, const LocalApic& apic, bool bootstrap_processor) noexcept {
        if (Initialized) return ProcessorLocalInitializationError::AlreadyInitialized;
        if (!apic.IsInitialized()) return ProcessorLocalInitializationError::InvalidDependency;

        const Uint64 address = reinterpret_cast<Uint64>(this);
        if (address == 0 || !IsCanonical48BitAddress(address))
            return ProcessorLocalInitializationError::InvalidAddress;

        Self = this;
        ProcessorIndex = processor_index;
        ApicId = apic.Id();
        BootstrapProcessor = bootstrap_processor;

        /*
         * While zOS is kernel-only, GS points directly at processor-local
         * state. IA32_KERNEL_GS_BASE is kept clear until Ring 3 introduces
         * SWAPGS and a user-visible GS base.
         */
        WriteMsr(GsBaseMsr, address);
        if (ReadMsr(GsBaseMsr) != address)
            return ProcessorLocalInitializationError::GsBaseWriteFailed;

        WriteMsr(KernelGsBaseMsr, 0);
        if (ReadMsr(KernelGsBaseMsr) != 0)
            return ProcessorLocalInitializationError::KernelGsBaseWriteFailed;

        ProcessorLocal* current = CurrentProcessor();
        if (current != this || current->Self != this || current->ProcessorIndex != processor_index ||
            current->ApicId != apic.Id() || current->BootstrapProcessor != bootstrap_processor)
            return ProcessorLocalInitializationError::CurrentProcessorMismatch;

        Initialized = true;
        Print();

        return ProcessorLocalInitializationError::Success;
    }

    ProcessorLocal* CurrentProcessor() noexcept {
        ProcessorLocal* processor{};

        __asm__ volatile(
            "movq %%gs:0, %0"
            : "=r"(processor)
            :
            : "memory"
        );

        return processor;
    }

    void ProcessorLocal::Print() const noexcept {
        Diagnostics::Write("[zOS/CPU] Processor-local state installed.\n");

        Diagnostics::Write("[zOS/CPU] Processor index: ");
        Diagnostics::WriteDecimal(ProcessorIndex);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/CPU] Local APIC ID: ");
        Diagnostics::WriteDecimal(ApicId);
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/CPU] GS base: ");
        Diagnostics::WriteHex(reinterpret_cast<Memory::Uint64>(this));
        Diagnostics::Write("\n");

        Diagnostics::Write("[zOS/CPU] Processor-local access verified.\n");
    }

    const char* ProcessorLocal::Describe(ProcessorLocalInitializationError error) noexcept {
        switch (error) {
        case ProcessorLocalInitializationError::Success: return "success";
        case ProcessorLocalInitializationError::AlreadyInitialized: return "processor-local state is already initialized";
        case ProcessorLocalInitializationError::InvalidDependency: return "processor-local state dependency is invalid";
        case ProcessorLocalInitializationError::InvalidAddress: return "processor-local state does not have a canonical virtual address";
        case ProcessorLocalInitializationError::GsBaseWriteFailed: return "failed to install the processor-local GS base";
        case ProcessorLocalInitializationError::KernelGsBaseWriteFailed: return "failed to initialize the kernel GS swap base";
        case ProcessorLocalInitializationError::CurrentProcessorMismatch: return "GS-relative processor-local access did not resolve to the installed processor";
        default: return "unknown processor-local initialization error";
        }
    }
}
