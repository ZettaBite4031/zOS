#include <Kernel/Memory/BootstrapMetadata.hpp>

#include <Kernel/Memory/Layout.hpp>

#include <Kernel/Runtime/New.hpp>

extern "C" void* memset(void* dst, int v, unsigned long long n) noexcept;

namespace Zos::Kernel::Memory {
    bool BootstrapMetadataArena::IsPowerOfTwo(Uint64 value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool BootstrapMetadataArena::TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
        const Uint64 mask = alignment - 1;
        if (value > MaximumValue - mask) return false;
        result = (value + mask) & ~mask;
        return true;
    }

    Uint64 BootstrapMetadataArena::ReservedOwnershipOffset() noexcept {
        constexpr Uint64 alignment = alignof(PhysicalAllocation);
        return (PageSize - sizeof(PhysicalAllocation)) & ~(alignment - 1);
    }

    void* BootstrapMetadataArena::PhysicalPointer(PhysicalAddress address) const noexcept {
        if (!m_DirectMapAccess) return reinterpret_cast<void*>(address.Value());
        if (!Layout::IsDirectMappable(address)) return nullptr;
        return reinterpret_cast<void*>(Layout::DirectMapAddress(address).Value());
    }

    void BootstrapMetadataArena::InitializePage(PageHeader* page) noexcept {
        memset(page, 0, PageSize);
        page->Offset = sizeof(PageHeader);
    }

    void BootstrapMetadataArena::MoveTokenInto(PhysicalAllocation& destination, PhysicalAllocation& source) noexcept {
        destination.~PhysicalAllocation();
        new (&destination) PhysicalAllocation(static_cast<PhysicalAllocation&&>(source));
    }

    MetadataArenaInitializationError BootstrapMetadataArena::Initialize(PhysicalMemoryManager& physical_memory) noexcept {
        if (m_Retired) return MetadataArenaInitializationError::Retired;
        if (IsInitialized()) return MetadataArenaInitializationError::AlreadyInitialized;
        if (!physical_memory.IsInitialized()) return MetadataArenaInitializationError::PhysicalAllocationFailed;

        PhysicalAllocation page{};
        const PhysicalAllocationError allocation_error = physical_memory.AllocatePage(page);
        if (allocation_error != PhysicalAllocationError::Success) return MetadataArenaInitializationError::PhysicalAllocationFailed;

        if (page.Base().IsNull()) {
            (void)physical_memory.Release(page);
            return MetadataArenaInitializationError::AddressUnavailable;
        }

        auto* header = static_cast<PageHeader*>(PhysicalPointer(page.Base()));
        if (header == nullptr) {
            (void)physical_memory.Release(page);
            return MetadataArenaInitializationError::AddressUnavailable;
        }

        InitializePage(header);
        MoveTokenInto(m_FirstPageAllocation, page);

        m_PhysicalMemory = &physical_memory;
        m_FirstPage = header;
        m_CurrentPage = header;
        m_Statistics.PageCount = 1;
        return MetadataArenaInitializationError::Success;
    }

    bool BootstrapMetadataArena::Grow() noexcept {
        if (!IsInitialized() || m_CurrentPage == nullptr) return false;

        PhysicalAllocation page{};
        const PhysicalAllocationError error = m_PhysicalMemory->AllocatePage(page);
        if (error != PhysicalAllocationError::Success) return false;

        if (page.Base().IsNull()) {
            (void)m_PhysicalMemory->Release(page);
            return false;
        }

        auto* new_page = static_cast<PageHeader*>(PhysicalPointer(page.Base()));
        if (new_page == nullptr) {
            (void)m_PhysicalMemory->Release(page);
            return false;
        }

        InitializePage(new_page);

        const PhysicalAddress new_page_address = page.Base();
        auto* ownership = reinterpret_cast<PhysicalAllocation*>(reinterpret_cast<Uint8*>(m_CurrentPage) + ReservedOwnershipOffset());

        new (ownership) PhysicalAllocation(static_cast<PhysicalAllocation&&>(page));
        m_CurrentPage->Next = new_page_address;
        m_CurrentPage = new_page;
        m_Statistics.PageCount++;
        return true;
    }

    void* BootstrapMetadataArena::TryAllocateFromCurrent(Uint64 size, Uint64 alignment) noexcept {
        if (m_CurrentPage == nullptr) return nullptr;

        Uint64 aligned_offset = 0;
        if (!TryAlignUp(m_CurrentPage->Offset, alignment, aligned_offset)) return nullptr;

        const Uint64 usable_end = ReservedOwnershipOffset();
        if (aligned_offset > usable_end || size > usable_end - aligned_offset) return nullptr;

        auto* result = reinterpret_cast<Uint8*>(m_CurrentPage) + aligned_offset;
        m_CurrentPage->Offset = aligned_offset + size;
        m_Statistics.BytesRequested += size;
        memset(result, 0, size);
        return result;
    }

    void* BootstrapMetadataArena::Allocate(Uint64 size, Uint64 alignment) noexcept {
        if (m_Retired || !IsInitialized() || size == 0 || !IsPowerOfTwo(alignment)) return nullptr;
        if (alignment > PageSize || size > ReservedOwnershipOffset() - sizeof(PageHeader)) return nullptr;
        if (void* allocation = TryAllocateFromCurrent(size, alignment); allocation != nullptr) return allocation;
        if (!Grow()) return nullptr;
        return TryAllocateFromCurrent(size, alignment);
    }

    PhysicalAllocation* BootstrapMetadataArena::Retain(PhysicalAllocation&& allocation) noexcept {
        if (m_Retired || !allocation.IsValid()) return nullptr;

        void* storage = Allocate(sizeof(PhysicalAllocation), alignof(PhysicalAllocation));
        if (storage == nullptr) return nullptr;
        return new (storage) PhysicalAllocation(static_cast<PhysicalAllocation&&>(allocation));
    }

    PhysicalAddress BootstrapMetadataArena::BackingPage(Uint64 index) const noexcept {
        if (!IsInitialized() || index >= m_Statistics.PageCount) return {};

        PhysicalAddress address = m_FirstPageAllocation.Base();
        for (Uint64 current = 0; current < index; current++) {
            const auto* header = static_cast<const PageHeader*>(PhysicalPointer(address));
            if (header == nullptr || header->Next.IsNull()) return {};
            address = header->Next;
        }
        return address;
    }

    MetadataArenaRetirementError BootstrapMetadataArena::Retire(MetadataArenaRetirementResult& result) noexcept {
        result = {};

        if (m_Retired) return MetadataArenaRetirementError::AlreadyRetired;
        if (!IsInitialized()) return MetadataArenaRetirementError::NotInitialized;
        if (!m_FirstPageAllocation.IsValid() || m_FirstPageAllocation.PageCount() != 1 || m_Statistics.PageCount == 0)
            return MetadataArenaRetirementError::CorruptState;

        const Uint64 original_page_count = m_Statistics.PageCount;

        /*
         * Every page after the first is owned by a PhysicalAllocation token
         * stored inside its predecessor, so the chain must be released from
         * the tail backwards.
         */
        while (m_Statistics.PageCount > 1) {
            PhysicalAddress current_address = m_FirstPageAllocation.Base();
            auto* current = static_cast<PageHeader*>(PhysicalPointer(current_address));
            if (current == nullptr) return MetadataArenaRetirementError::CorruptState;

            Uint64 visited = 1;
            for (;;) {
                const PhysicalAddress next_address = current->Next;
                if (next_address.IsNull()) return MetadataArenaRetirementError::CorruptState;

                auto* next = static_cast<PageHeader*>(PhysicalPointer(next_address));
                if (next == nullptr) return MetadataArenaRetirementError::CorruptState;

                if (next->Next.IsNull()) {
                    auto* ownership = reinterpret_cast<PhysicalAllocation*>(reinterpret_cast<Uint8*>(current) + ReservedOwnershipOffset());
                    if (!ownership->IsValid() || ownership->Base() != next_address || ownership->PageCount() != 1)
                        return MetadataArenaRetirementError::CorruptState;

                    if (m_PhysicalMemory->Release(*ownership) != PhysicalAllocationError::Success)
                        return MetadataArenaRetirementError::PhysicalReleaseFailed;

                    current->Next = {};
                    m_CurrentPage = current;
                    m_Statistics.PageCount--;
                    result.ReleasedPages++;
                    break;
                }

                current_address = next_address;
                current = next;
                visited++;
                if (visited >= m_Statistics.PageCount) return MetadataArenaRetirementError::CorruptState;
            }
        }

        if (result.ReleasedPages != original_page_count - 1) return MetadataArenaRetirementError::CorruptState;
        if (m_PhysicalMemory->Release(m_FirstPageAllocation) != PhysicalAllocationError::Success)
            return MetadataArenaRetirementError::PhysicalReleaseFailed;

        result.ReleasedPages++;
        m_FirstPage = nullptr;
        m_CurrentPage = nullptr;
        m_PhysicalMemory = nullptr;
        m_Statistics = {};
        m_DirectMapAccess = false;
        m_Retired = true;
        return MetadataArenaRetirementError::Success;
    }

    const char* BootstrapMetadataArena::Describe(MetadataArenaInitializationError error) noexcept {
        switch (error) {
        case MetadataArenaInitializationError::Success: return "success";
        case MetadataArenaInitializationError::AlreadyInitialized: return "metadata arena is already initialized";
        case MetadataArenaInitializationError::Retired: return "metadata arena has already been retired";
        case MetadataArenaInitializationError::PhysicalAllocationFailed: return "failed to allocate metadata storage";
        case MetadataArenaInitializationError::AddressUnavailable: return "metadata physical address is unavailable to bootstrap code";
        default: return "unknown metadata arena initialization error";
        }
    }

    const char* BootstrapMetadataArena::Describe(MetadataArenaRetirementError error) noexcept {
        switch (error) {
        case MetadataArenaRetirementError::Success: return "success";
        case MetadataArenaRetirementError::NotInitialized: return "metadata arena is not initialized";
        case MetadataArenaRetirementError::AlreadyRetired: return "metadata arena is already retired";
        case MetadataArenaRetirementError::CorruptState: return "metadata arena ownership chain is corrupt";
        case MetadataArenaRetirementError::PhysicalReleaseFailed: return "failed to release metadata arena backing memory";
        default: return "unknown metadata arena retirement error";
        }
    }
}
