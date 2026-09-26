#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Memory {
    enum class MetadataArenaInitializationError : Uint32 {
        Success,
        AlreadyInitialized,
        Retired,
        PhysicalAllocationFailed,
        AddressUnavailable,
    };

    enum class MetadataArenaRetirementError : Uint32 {
        Success,
        NotInitialized,
        AlreadyRetired,
        CorruptState,
        PhysicalReleaseFailed,
    };

    struct MetadataArenaStatistics final {
        Uint64 PageCount{};
        Uint64 BytesRequested{};
    };

    struct MetadataArenaRetirementResult final {
        Uint64 ReleasedPages{};
        [[nodiscard]] constexpr Uint64 ReleasedBytes() const noexcept { return ReleasedPages * PageSize; }
    };

    class BootstrapMetadataArena final {
    public:
        constexpr BootstrapMetadataArena() noexcept = default;
        BootstrapMetadataArena(const BootstrapMetadataArena&) = delete;
        BootstrapMetadataArena& operator=(const BootstrapMetadataArena&) = delete;

        [[nodiscard]] MetadataArenaInitializationError Initialize(PhysicalMemoryManager& physical_memory) noexcept;
        [[nodiscard]] MetadataArenaRetirementError Retire(MetadataArenaRetirementResult& result) noexcept;
        [[nodiscard]] void* Allocate(Uint64 size, Uint64 alignment) noexcept;
        [[nodiscard]] PhysicalAllocation* Retain(PhysicalAllocation&& allocation) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_PhysicalMemory != nullptr && !m_Retired; }
        [[nodiscard]] bool IsRetired() const noexcept { return m_Retired; }
        [[nodiscard]] const MetadataArenaStatistics& Statistics() const noexcept { return m_Statistics; }
        [[nodiscard]] PhysicalAddress FirstPage() const noexcept { return m_FirstPageAllocation.Base(); }
        [[nodiscard]] Uint64 BackingPageCount() const noexcept { return m_Statistics.PageCount; }

        void EnableDirectMapAccess() noexcept { if (IsInitialized()) m_DirectMapAccess = true; }
        [[nodiscard]] bool UsesDirectMapAccess() const noexcept { return m_DirectMapAccess; }

        [[nodiscard]] PhysicalAddress BackingPage(Uint64 index) const noexcept;

        [[nodiscard]] static const char* Describe(MetadataArenaInitializationError error) noexcept;
        [[nodiscard]] static const char* Describe(MetadataArenaRetirementError error) noexcept;

    private:
        struct PageHeader final {
            PhysicalAddress Next{};
            Uint64 Offset{};
        };

        [[nodiscard]] static bool IsPowerOfTwo(Uint64 value) noexcept;
        [[nodiscard]] static bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept;
        [[nodiscard]] static Uint64 ReservedOwnershipOffset() noexcept;
        [[nodiscard]] void* PhysicalPointer(PhysicalAddress address) const noexcept;
        [[nodiscard]] bool Grow() noexcept;
        [[nodiscard]] void* TryAllocateFromCurrent(Uint64 size, Uint64 alignment) noexcept;
        void InitializePage(PageHeader* page) noexcept;
        void MoveTokenInto(PhysicalAllocation& destination, PhysicalAllocation& source) noexcept;

        bool m_DirectMapAccess{};
        bool m_Retired{};
        PhysicalMemoryManager* m_PhysicalMemory{};
        PhysicalAllocation m_FirstPageAllocation{};
        PageHeader* m_FirstPage{};
        PageHeader* m_CurrentPage{};
        MetadataArenaStatistics m_Statistics{};
    };
}
