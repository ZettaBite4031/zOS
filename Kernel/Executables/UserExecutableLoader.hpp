#pragma once

#include <Kernel/Architecture/AMD64/Paging.hpp>
#include <Kernel/Memory/PhysicalMemory.hpp>

namespace Zos::Kernel::Processes {
    class Process;
}

namespace Zos::Kernel::Executables {
    inline constexpr Memory::Uint64 MaximumStaticLoadSegments{ 8 };

    enum class UserExecutableLoadError : Memory::Uint32 {
        Success,
        InvalidDependency,
        OutputAlreadyLoaded,
        ImageTooSmall,
        InvalidMagic,
        UnsupportedClass,
        UnsupportedEndianness,
        UnsupportedVersion,
        UnsupportedFileType,
        UnsupportedMachine,
        InvalidHeader,
        InvalidProgramHeaderTable,
        DynamicImageUnsupported,
        TooManyLoadSegments,
        InvalidSegment,
        UnsupportedSegmentPermissions,
        SegmentOverlap,
        AddressAlreadyMapped,
        InvalidEntryPoint,
        InvalidStack,
        AllocationFailed,
        DirectMapUnavailable,
        MappingFailed,
        RollbackFailed,
    };

    struct UserExecutableLoadOptions final {
        Memory::VirtualAddress StackBase{};
        Memory::Uint64 StackPageCount{};
    };

    struct LoadedUserSegment final {
        Memory::VirtualSpan Virtual{};
        Memory::PhysicalAllocation Backing{};
        Memory::MappingOptions Mapping{};
        Memory::Uint64 FileSize{};
        Memory::Uint64 MemorySize{};
    };

    /*
     * Owns the physical backing retained for one loaded user image.
     * Process teardown/unload does not exist yet, so Step 18 keeps this object
     * alive for the lifetime of the bring-up process.
     */
    class LoadedUserImage final {
    public:
        constexpr LoadedUserImage() noexcept = default;
        LoadedUserImage(const LoadedUserImage&) = delete;
        LoadedUserImage& operator=(const LoadedUserImage&) = delete;

        [[nodiscard]] bool IsLoaded() const noexcept { return m_Loaded; }
        [[nodiscard]] Memory::VirtualAddress EntryPoint() const noexcept { return m_EntryPoint; }
        [[nodiscard]] Memory::VirtualAddress StackGuard() const noexcept { return m_StackGuard; }
        [[nodiscard]] Memory::VirtualSpan Stack() const noexcept { return m_Stack; }
        [[nodiscard]] Memory::VirtualAddress StackTop() const noexcept {
            return m_Stack.IsEmpty() ? Memory::VirtualAddress{} : m_Stack.Base + m_Stack.SizeBytes();
        }
        [[nodiscard]] Memory::Uint64 SegmentCount() const noexcept { return m_SegmentCount; }
        [[nodiscard]] const LoadedUserSegment& Segment(Memory::Uint64 index) const noexcept {
            return m_Segments[index];
        }

        [[nodiscard]] bool ContainsExecutableAddress(Memory::VirtualAddress address) const noexcept;

    private:
        friend class UserExecutableLoader;

        LoadedUserSegment m_Segments[MaximumStaticLoadSegments]{};
        Memory::PhysicalAllocation m_StackBacking{};
        Memory::VirtualAddress m_EntryPoint{};
        Memory::VirtualAddress m_StackGuard{};
        Memory::VirtualSpan m_Stack{};
        Memory::Uint64 m_SegmentCount{};
        bool m_Loaded{};
    };

    class UserExecutableLoader final {
    public:
        UserExecutableLoader() = delete;

        /*
         * Loads a static ELF into an isolated, non-current process address
         * space. Dynamic linking and loading into the current process are
         * intentionally outside the Step 18 contract.
         */
        [[nodiscard]] static UserExecutableLoadError LoadStaticElf(
            const Memory::Uint8* image,
            Memory::Uint64 image_size,
            Processes::Process& process,
            Memory::PhysicalMemoryManager& physical_memory,
            const UserExecutableLoadOptions& options,
            LoadedUserImage& output
        ) noexcept;

        [[nodiscard]] static const char* Describe(UserExecutableLoadError error) noexcept;
    };
}