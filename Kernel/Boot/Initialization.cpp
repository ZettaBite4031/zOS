#include <Kernel/Boot/Initialization.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Boot {
    namespace {
        using namespace Memory;

        [[nodiscard]] bool IsPageRange(const ::Zos::Boot::PhysicalRange& range) noexcept {
            return range.Base != 0 && range.Size != 0 && (range.Base & (PageSize - 1)) == 0 && (range.Size & (PageSize - 1)) == 0;
        }

        [[nodiscard]] bool ConvertRange(const ::Zos::Boot::PhysicalRange& source, PhysicalSpan& destination) noexcept {
            if (!IsPageRange(source)) return false;

            destination = PhysicalSpan{ PhysicalAddress(source.Base), source.Size / PageSize };

            return !destination.IsEmpty();
        }

        void PrintSignature(::Zos::Boot::Uint64 signature) noexcept {
            for (::Zos::Boot::Uint32 i = 0; i < 8; ++i) {
                const auto byte = static_cast<::Zos::Boot::Uint8>((signature >> (i * 8)) & 0xFF);

                if (byte == '\0') break;

                Diagnostics::WriteChar(static_cast<char>(byte));
            }
        }

        void Validate(const ::Zos::Boot::BootEnvironment& environment) noexcept {
            if (environment.Signature != ::Zos::Boot::EnvironmentSignature)
                Diagnostics::Fatal("Startup", "boot environment signature mismatch");

            if (environment.Version != ::Zos::Boot::ProtocolVersion)
                Diagnostics::Fatal("Startup", "unsupported boot protocol version");

            if (environment.Size < sizeof(::Zos::Boot::BootEnvironment))
                Diagnostics::Fatal("Startup", "boot environment structure is too small");

            if (!IsPageRange(environment.KernelImage) || !IsPageRange(environment.KernelStack) ||
                !IsPageRange(environment.EnvironmentStorage) || !IsPageRange(environment.MemoryMapStorage))
                Diagnostics::Fatal("Startup", "boot-owned physical ranges are not page aligned");

            if (environment.MemoryMapStorage.Base == 0 || environment.MemoryMapSize == 0 ||
                environment.MemoryMapDescriptorSize == 0 || environment.MemoryMapSize > environment.MemoryMapStorage.Size ||
                (environment.MemoryMapSize % environment.MemoryMapDescriptorSize) != 0)
                Diagnostics::Fatal("Startup", "firmware memory-map metadata is inconsistent");

            if (environment.AcpiRsdp == 0)
                Diagnostics::Fatal("Startup", "ACPI RSDP was not supplied");

            const Uint64 environment_address = reinterpret_cast<Uint64>(&environment);
            if (environment_address < environment.EnvironmentStorage.Base ||
                environment_address - environment.EnvironmentStorage.Base > environment.EnvironmentStorage.Size - sizeof(::Zos::Boot::BootEnvironment))
                Diagnostics::Fatal("Startup", "boot environment lies outside its declared storage");
        }

        void Internalize(Context& context, const ::Zos::Boot::BootEnvironment& environment) noexcept {
            if (!ConvertRange(environment.KernelImage, context.KernelImage) ||
                !ConvertRange(environment.KernelStack, context.BootstrapStack) ||
                !ConvertRange(environment.EnvironmentStorage, context.EnvironmentStorage) ||
                !ConvertRange(environment.MemoryMapStorage, context.MemoryMapStorage))
                Diagnostics::Fatal("Startup", "failed to internalize boot-owned physical ranges");

            context.AcpiRsdp = PhysicalAddress(environment.AcpiRsdp);
            if (context.AcpiRsdp.IsNull())
                Diagnostics::Fatal("Startup", "internalized ACPI RSDP is null");

            context.Initialized = true;
        }

        void Print(const ::Zos::Boot::BootEnvironment& environment, const Context& context) noexcept {
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

            Diagnostics::Write("[zOS/Startup] Firmware handoff validated and internalized.\n");
            Diagnostics::Write("[zOS/Startup] ACPI RSDP: ");
            Diagnostics::WriteHex(context.AcpiRsdp.Value());
            Diagnostics::Write("\n");
        }
    }

    void Initialize(Context& context, const ::Zos::Boot::BootEnvironment& environment) noexcept {
        if (context.Initialized)
            Diagnostics::Fatal("Startup", "boot context was initialized more than once");

        Validate(environment);
        Internalize(context, environment);
        Print(environment, context);
    }
}
