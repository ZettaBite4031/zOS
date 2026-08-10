#pragma once

#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Memory {
    namespace Layout {
        inline constexpr VirtualAddress NullGuardBase{ 0x0000000000000000ULL };
        inline constexpr Uint64 NullGuardSize{ 64 * 1024 };

        inline constexpr VirtualAddress DirectMapBase{ 0xFFFF800000000000ULL };
        inline constexpr Uint64 DirectMapSize{ 64ULL * 1024 * 1024 * 1024 * 1024 };

        inline constexpr VirtualAddress KernelDynamicBase{ 0xFFFFC00000000000ULL };
        inline constexpr Uint64 KernelDynamicSize{ 32ULL * 1024 * 1024 * 1024 * 1024 };

        inline constexpr VirtualAddress KernelMmioBase{ 0xFFFFE00000000000ULL };
        inline constexpr Uint64 KernelMmioSize{ 16ULL * 1024 * 1024 * 1024 * 1024 };

        inline constexpr VirtualAddress KernelSpecialBase{0xFFFFF00000000000ULL  };
        inline constexpr VirtualAddress FutureKernelImageBase{ 0xFFFFFFFF80000000ULL };

        [[nodiscard]] constexpr VirtualSpan KernelDynamicSpan() noexcept {
            return VirtualSpan{ KernelDynamicBase, KernelDynamicSize / PageSize };
        }

        [[nodiscard]] constexpr VirtualSpan KernelMmioSpan() noexcept {
            return VirtualSpan{ KernelMmioBase, KernelMmioSize / PageSize };
        }

        static_assert((DirectMapBase.Value() & (PageSize - 1)) == 0);
        static_assert((KernelDynamicBase.Value() & (PageSize - 1)) == 0);
        static_assert((KernelMmioBase.Value() & (PageSize - 1)) == 0);
        static_assert((KernelSpecialBase.Value() & (PageSize - 1)) == 0);
        static_assert((FutureKernelImageBase.Value() & (PageSize - 1)) == 0);
        static_assert(DirectMapBase.Value() + DirectMapSize == KernelDynamicBase.Value());
        static_assert(KernelDynamicBase.Value() + KernelDynamicSize == KernelMmioBase.Value());
        static_assert(KernelMmioBase.Value() + KernelMmioSize == KernelSpecialBase.Value());
        static_assert(KernelSpecialBase < FutureKernelImageBase);
    }

    enum class MetadataArenaInitializationError : Uint32 {
        Success,
        AlreadyInitialized,
        PhysicalAllocationFailed,
        AddressUnavailable,
    };

    struct MetadataArenaStatistics final {
        Uint64 PageCount{};
        Uint64 BytesRequested{};
    };

    class BootstrapMetadataArena final {
    public:
        BootstrapMetadataArena() noexcept = default;
        BootstrapMetadataArena(const BootstrapMetadataArena&) = delete;
        BootstrapMetadataArena& operator=(const BootstrapMetadataArena&) = delete;

        [[nodiscard]] MetadataArenaInitializationError Initialize(PhysicalMemoryManager& physical_memory) noexcept;
        [[nodiscard]] void* Allocate(Uint64 size, Uint64 alignment) noexcept;
        [[nodiscard]] PhysicalAllocation* Retain(PhysicalAllocation&& allocation) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_PhysicalMemory != nullptr; }
        [[nodiscard]] const MetadataArenaStatistics& Statistics() const noexcept { return m_Statistics; }
        [[nodiscard]] PhysicalAddress FirstPage() const noexcept { return m_FirstPageAllocation.Base(); }

        [[nodiscard]] static const char* Describe(MetadataArenaInitializationError error) noexcept;

    private:
        struct PageHeader final {
            PageHeader* Next{};
            Uint64 Offset{};
        };

        [[nodiscard]] static bool IsPowerOfTwo(Uint64 value) noexcept;
        [[nodiscard]] static bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept;
        [[nodiscard]] static Uint64 ReservedOwnershipOffset() noexcept;
        [[nodiscard]] static void* BootstrapPointer(PhysicalAddress address) noexcept;

        [[nodiscard]] bool Grow() noexcept;
        [[nodiscard]] void* TryAllocateFromCurrent(Uint64 size, Uint64 alignment) noexcept;
        void InitializePage(PageHeader* page) noexcept;
        void MoveTokenInto(PhysicalAllocation& destination, PhysicalAllocation& source) noexcept;

        PhysicalMemoryManager* m_PhysicalMemory{};
        PhysicalAllocation m_FirstPageAllocation{};
        PageHeader* m_FirstPage{};
        PageHeader* m_CurrentPage{};
        MetadataArenaStatistics m_Statistics{};
    };

    enum class VirtualAllocationPreference : Uint32 {
        LowAddresses,
        HighAddresses,
    };

    struct VirtualAllocationConstraints final {
        Uint64 Alignment{ PageSize };
        VirtualAllocationPreference Preference{ VirtualAllocationPreference::LowAddresses };
    };

    enum class VirtualAllocationError : Uint32 {
        Success,
        AlreadyInitialized,
        NotInitialized,
        InvalidRequest,
        OutputAlreadyOwnsRange,
        OutOfAddressSpace,
        OutOfMetadata,
        WrongOwner,
        CorruptReservation,
    };

    struct VirtualAddressAllocatorStatistics final {
        Uint64 ManagedPages{};
        Uint64 FreePages{};
        Uint64 ReservedPages{};
        Uint64 FreeExtentCount{};
    };

    class VirtualAddressAllocator;

    class VirtualReservation final {
    public:
        constexpr VirtualReservation() noexcept = default;
        VirtualReservation(const VirtualReservation&) = delete;
        VirtualReservation& operator=(const VirtualReservation&) = delete;

        VirtualReservation(VirtualReservation&& other) noexcept;
        VirtualReservation& operator=(VirtualReservation&&) = delete;

        [[nodiscard]] bool IsValid() const noexcept { return m_Owner != nullptr && !m_Span.IsEmpty(); }
        [[nodiscard]] VirtualAddress Base() const noexcept { return m_Span.Base; }
        [[nodiscard]] Uint64 PageCount() const noexcept { return m_Span.PageCount; }
        [[nodiscard]] Uint64 SizeBytes() const noexcept { return m_Span.SizeBytes(); }
        [[nodiscard]] VirtualSpan Span() const noexcept { return m_Span; }

    private:
        friend class VirtualAddressAllocator;

        void Invalidate() noexcept {
            m_Owner = nullptr;
            m_Span = {};
            m_ReleaseExtent = nullptr;
        }

        VirtualAddressAllocator* m_Owner{};
        VirtualSpan m_Span{};
        void* m_ReleaseExtent{};
    };

    class VirtualAddressAllocator final {
    public:
        VirtualAddressAllocator() noexcept = default;
        VirtualAddressAllocator(const VirtualAddressAllocator&) = delete;
        VirtualAddressAllocator& operator=(const VirtualAddressAllocator&) = delete;

        [[nodiscard]] VirtualAllocationError Initialize(VirtualSpan managed_range, BootstrapMetadataArena& metadata) noexcept;

        [[nodiscard]] VirtualAllocationError Reserve(Uint64 page_count, VirtualReservation& output, VirtualAllocationConstraints constraints = {}) noexcept;

        [[nodiscard]] VirtualAllocationError Release(VirtualReservation& reservation) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Metadata != nullptr; }
        [[nodiscard]] VirtualSpan ManagedRange() const noexcept { return m_ManagedRange; }
        [[nodiscard]] const VirtualAddressAllocatorStatistics& Statistics() const noexcept { return m_Statistics; }
        
        [[nodiscard]] static const char* Describe(VirtualAllocationError error) noexcept;

    private:
        struct FreeExtent final {
            Uint64 Base{};
            Uint64 PageCount{};
            FreeExtent* Previous{};
            FreeExtent* Next{};
        };

[[nodiscard]] static bool IsPowerOfTwo(Uint64 value) noexcept;
        [[nodiscard]] static bool TryRangeEnd(Uint64 base, Uint64 size, Uint64& end) noexcept;
        [[nodiscard]] static bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept;
        [[nodiscard]] static Uint64 AlignDown(Uint64 value, Uint64 alignment) noexcept;

        [[nodiscard]] FreeExtent* CreateExtentStorage() noexcept;
        [[nodiscard]] FreeExtent* AcquireExtent(Uint64 base, Uint64 pageCount) noexcept;
        void RecycleExtent(FreeExtent& extent) noexcept;
        void RemoveExtent(FreeExtent& extent) noexcept;
        void InsertBefore(FreeExtent* position, FreeExtent& extent) noexcept;
        void InsertAfter(FreeExtent* position, FreeExtent& extent) noexcept;
        [[nodiscard]] VirtualAllocationError ReserveFromExtent(FreeExtent& extent, Uint64 allocationBase, Uint64 pageCount, VirtualReservation& output) noexcept;
        [[nodiscard]] bool FindLowCandidate(const FreeExtent& extent, Uint64 pageCount, Uint64 alignment, Uint64& candidate) const noexcept;
        [[nodiscard]] bool FindHighCandidate(const FreeExtent& extent, Uint64 pageCount, Uint64 alignment, Uint64& candidate) const noexcept;
        [[nodiscard]] bool ReservationInsideManagedRange(VirtualSpan span) const noexcept;

        BootstrapMetadataArena* m_Metadata{};
        FreeExtent* m_FreeHead{};
        FreeExtent* m_FreeTail{};
        FreeExtent* m_RecycledExtents{};
        VirtualSpan m_ManagedRange{};
        VirtualAddressAllocatorStatistics m_Statistics{};
    };
}