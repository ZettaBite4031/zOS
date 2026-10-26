#pragma once

#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Memory/BootstrapMetadata.hpp>
#include <Kernel/Memory/VirtualAddressAllocator.hpp>
#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class PageMap;
}

namespace Zos::Kernel::Memory {
    enum class KernelAddressSpaceError : Uint32 {
        Success,
        AlreadyBuilt,
        NotBuilt,
        AlreadyActive,
        InvalidDependency,
        InvalidBootEnvironment,
        UnsupportedKernelLoadModel,
        InvalidKernelLayout,
        DirectMapTooSmall,
        MappingFailed,
        ValidationFailed,
        ActivationFailed,
    };

    class KernelAddressSpace final {
    public:
        KernelAddressSpace(PhysicalMemoryManager& physical_memory, BootstrapMetadataArena& metadata, Architecture::AMD64::PageMap& page_map) noexcept
            : m_PhysicalMemory(&physical_memory), m_Metadata(&metadata), m_PageMap(&page_map) {};

        KernelAddressSpace(const KernelAddressSpace&) = delete;
        KernelAddressSpace& operator=(const KernelAddressSpace&) = delete;

        [[nodiscard]] KernelAddressSpaceError Build(const ::Zos::Boot::BootEnvironment& environment) noexcept;

        [[nodiscard]] KernelAddressSpaceError Activate() noexcept;

        [[nodiscard]] bool IsBuilt() const noexcept { return m_Built; }
        [[nodiscard]] bool IsActive() const noexcept;

        [[nodiscard]] VirtualAddress DirectMap(PhysicalAddress address) const noexcept { return Layout::DirectMapAddress(address); }

        [[nodiscard]] static const char* Describe(KernelAddressSpaceError error) noexcept;

    private:
        [[nodiscard]] bool ValidateKernelLayout(const ::Zos::Boot::BootEnvironment& environment) const noexcept;
        [[nodiscard]] KernelAddressSpaceError ValidateDirectMapCoverage(const ::Zos::Boot::BootEnvironment& environment) const noexcept;

        [[nodiscard]] bool MapKernelSection(Uint64 start, Uint64 end, MappingOptions options) noexcept;
        [[nodiscard]] bool MapKernelImage() noexcept;
        [[nodiscard]] bool MapBootstrapRanges(const ::Zos::Boot::BootEnvironment& environment) noexcept;
        
        [[nodiscard]] KernelAddressSpaceError MapDirectMemory(const ::Zos::Boot::BootEnvironment& environment) noexcept;

        [[nodiscard]] bool MapBootstrapMetadata() noexcept;

        [[nodiscard]] bool ValidateKernelSection(Uint64 start, Uint64 end, MappingOptions options) const noexcept;
        [[nodiscard]] bool ValidateMappings(const ::Zos::Boot::BootEnvironment& environment) const noexcept;

        [[nodiscard]] bool MapIdentityBytes(Uint64 base, Uint64 size, MappingOptions options) noexcept;
        
        [[nodiscard]] bool EnsureIdentityPage(PhysicalAddress page, MappingOptions options) noexcept;

        PhysicalMemoryManager* m_PhysicalMemory{};
        BootstrapMetadataArena* m_Metadata{};
        Architecture::AMD64::PageMap* m_PageMap{};
        const ::Zos::Boot::BootEnvironment* m_Environment{};

        bool m_Built{};
    };
}