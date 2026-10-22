#include <Kernel/Executables/UserExecutableLoader.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Executables/ELF.hpp>
#include <Kernel/Memory/Layout.hpp>
#include <Kernel/Processes/Process.hpp>

namespace Zos::Kernel::Executables {
    namespace {
        using namespace Memory;

        inline constexpr Uint64 MinimumUserAddress{ 64 * 1024 };
        inline constexpr Uint64 UserAddressLimit{ 0x0000800000000000ULL };
        inline constexpr Uint32 KnownProgramFlags{
            ELF::ProgramFlag::Read |
            ELF::ProgramFlag::Write |
            ELF::ProgramFlag::Execute
        };

        struct SegmentPlan final {
            VirtualSpan Virtual{};
            Uint64 FileOffset{};
            Uint64 FileSize{};
            Uint64 MemorySize{};
            MappingOptions Mapping{};
        };

        [[nodiscard]] constexpr bool IsPowerOfTwo(Uint64 value) noexcept {
            return value != 0 && (value & (value - 1)) == 0;
        }

        [[nodiscard]] constexpr bool TryAdd(Uint64 left, Uint64 right, Uint64& result) noexcept {
            if (right > MaximumValue - left) return false;
            result = left + right;
            return true;
        }

        [[nodiscard]] constexpr bool TryAlignUp(Uint64 value, Uint64 alignment, Uint64& result) noexcept {
            if (!IsPowerOfTwo(alignment)) return false;
            const Uint64 mask = alignment - 1;
            if (value > MaximumValue - mask) return false;
            result = (value + mask) & ~mask;
            return true;
        }

        [[nodiscard]] constexpr bool RangeInside(Uint64 offset, Uint64 size, Uint64 total) noexcept {
            return offset <= total && size <= total - offset;
        }

        [[nodiscard]] constexpr bool Overlaps(Uint64 left_base, Uint64 left_end, Uint64 right_base, Uint64 right_end) noexcept {
            return left_base < right_end && right_base < left_end;
        }

        [[nodiscard]] bool IsUserRange(Uint64 base, Uint64 end) noexcept {
            if (base < MinimumUserAddress || end <= base || end > UserAddressLimit)
                return false;

            return Architecture::AMD64::PageMap::IsCanonical(VirtualAddress(base)) &&
                Architecture::AMD64::PageMap::IsCanonical(VirtualAddress(end - 1));
        }

        [[nodiscard]] bool SpanIsUnmapped(const Architecture::AMD64::PageMap& page_map, VirtualSpan span) noexcept {
            for (Uint64 page = 0; page < span.PageCount; page++) {
                if (page_map.IsMapped(span.Base + page * PageSize))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ZeroAllocation(const PhysicalAllocation& allocation) noexcept {
            if (!allocation.IsValid()) return false;

            const VirtualAddress direct = Layout::DirectMapAddress(allocation.Base());
            if (direct.IsNull()) return false;

            auto* destination = reinterpret_cast<Uint8*>(direct.Value());
            for (Uint64 index = 0; index < allocation.SizeBytes(); index++)
                destination[index] = 0;

            return true;
        }
    }

    bool LoadedUserImage::ContainsExecutableAddress(VirtualAddress address) const noexcept {
        if (!m_Loaded || address.IsNull()) return false;

        for (Uint64 index = 0; index < m_SegmentCount; index++) {
            const LoadedUserSegment& segment = m_Segments[index];
            if (!HasAccess(segment.Mapping.Access, PageAccess::Execute)) continue;

            const Uint64 start = segment.Virtual.Base.Value();
            if (address.Value() >= start && address.Value() - start < segment.MemorySize)
                return true;
        }

        return false;
    }

    UserExecutableLoadError UserExecutableLoader::LoadStaticElf(const Uint8* image, Uint64 image_size, Processes::Process& process,
        PhysicalMemoryManager& physical_memory, const UserExecutableLoadOptions& options, LoadedUserImage& output) noexcept {
        using namespace Architecture::AMD64;

        if (image == nullptr || !process.IsInitialized() || !process.OwnsAddressSpace() ||
            process.State() != Processes::ProcessState::Active || process.AddressSpace() == nullptr ||
            process.AddressSpace()->IsActive() || !physical_memory.IsInitialized() ||
            !physical_memory.IsMetadataAccessPromoted())
            return UserExecutableLoadError::InvalidDependency;

        if (output.m_Loaded || output.m_StackBacking.IsValid() || output.m_SegmentCount != 0) {
            return UserExecutableLoadError::OutputAlreadyLoaded;
        }

        for (Uint64 index = 0; index < MaximumStaticLoadSegments; index++) {
            if (output.m_Segments[index].Backing.IsValid())
                return UserExecutableLoadError::OutputAlreadyLoaded;
        }

        if (image_size < sizeof(ELF::Header))
            return UserExecutableLoadError::ImageTooSmall;

        const auto* header = reinterpret_cast<const ELF::Header*>(image);
        const Uint8* identification = header->Identification;

        if (identification[0] != ELF::Magic0 || identification[1] != ELF::Magic1 ||
            identification[2] != ELF::Magic2 || identification[3] != ELF::Magic3)
            return UserExecutableLoadError::InvalidMagic;

        if (identification[4] != ELF::Class64)
            return UserExecutableLoadError::UnsupportedClass;

        if (identification[5] != ELF::LittleEndian)
            return UserExecutableLoadError::UnsupportedEndianness;

        if (identification[6] != ELF::CurrentVersion || header->Version != ELF::CurrentVersion)
            return UserExecutableLoadError::UnsupportedVersion;

        if (header->Type != ELF::FileType::Executable)
            return UserExecutableLoadError::UnsupportedFileType;

        if (header->MachineValue != ELF::Machine::AMD64)
            return UserExecutableLoadError::UnsupportedMachine;

        if (header->HeaderSize != sizeof(ELF::Header) || header->ProgramHeaderEntrySize != sizeof(ELF::ProgramHeader))
            return UserExecutableLoadError::InvalidHeader;

        if (header->ProgramHeaderCount == 0 || header->ProgramHeaderOffset > image_size)
            return UserExecutableLoadError::InvalidProgramHeaderTable;

        const Uint64 program_header_bytes = static_cast<Uint64>(header->ProgramHeaderCount) * sizeof(ELF::ProgramHeader);
        if (!RangeInside(header->ProgramHeaderOffset, program_header_bytes, image_size))
            return UserExecutableLoadError::InvalidProgramHeaderTable;

        const auto* program_headers = reinterpret_cast<const ELF::ProgramHeader*>(image + header->ProgramHeaderOffset);
        SegmentPlan plans[MaximumStaticLoadSegments]{};
        Uint64 load_segment_count{};
        bool entry_is_executable{};

        for (Uint64 index = 0; index < header->ProgramHeaderCount; index++) {
            const ELF::ProgramHeader& program = program_headers[index];

            if (program.Type == ELF::ProgramType::Interpreter || program.Type == ELF::ProgramType::Dynamic)
                return UserExecutableLoadError::DynamicImageUnsupported;

            if (program.Type != ELF::ProgramType::Load || program.MemorySize == 0)
                continue;

            if (load_segment_count >= MaximumStaticLoadSegments)
                return UserExecutableLoadError::TooManyLoadSegments;

            if (program.FileSize > program.MemorySize ||
                !RangeInside(program.Offset, program.FileSize, image_size) ||
                !VirtualAddress(program.VirtualAddress).IsPageAligned() ||
                (program.Offset & (PageSize - 1)) != 0)
                return UserExecutableLoadError::InvalidSegment;

            if (program.Alignment > 1) {
                if (!IsPowerOfTwo(program.Alignment) || program.Alignment < PageSize ||
                    (program.VirtualAddress % program.Alignment) != (program.Offset % program.Alignment))
                    return UserExecutableLoadError::InvalidSegment;
            }

            Uint64 memory_end{};
            if (!TryAdd(program.VirtualAddress, program.MemorySize, memory_end))
                return UserExecutableLoadError::InvalidSegment;

            Uint64 mapped_end{};
            if (!TryAlignUp(memory_end, PageSize, mapped_end) || !IsUserRange(program.VirtualAddress, mapped_end))
                return UserExecutableLoadError::InvalidSegment;

            if ((program.Flags & ~KnownProgramFlags) != 0 ||
                (program.Flags & ELF::ProgramFlag::Read) == 0 ||
                ((program.Flags & ELF::ProgramFlag::Write) != 0 && (program.Flags & ELF::ProgramFlag::Execute) != 0))
                return UserExecutableLoadError::UnsupportedSegmentPermissions;

            PageAccess access = PageAccess::Read | PageAccess::User;
            if ((program.Flags & ELF::ProgramFlag::Write) != 0) access |= PageAccess::Write;
            if ((program.Flags & ELF::ProgramFlag::Execute) != 0) access |= PageAccess::Execute;

            const Uint64 page_count = (mapped_end - program.VirtualAddress) / PageSize;
            if (page_count == 0)
                return UserExecutableLoadError::InvalidSegment;

            for (Uint64 previous = 0; previous < load_segment_count; previous++) {
                const Uint64 previous_start = plans[previous].Virtual.Base.Value();
                const Uint64 previous_end = previous_start + plans[previous].Virtual.SizeBytes();
                if (Overlaps(program.VirtualAddress, mapped_end, previous_start, previous_end))
                    return UserExecutableLoadError::SegmentOverlap;
            }

            plans[load_segment_count] = {
                .Virtual = VirtualSpan{ VirtualAddress(program.VirtualAddress), page_count },
                .FileOffset = program.Offset,
                .FileSize = program.FileSize,
                .MemorySize = program.MemorySize,
                .Mapping = MappingOptions{
                    .Access = access,
                    .Cache = CachePolicy::WriteBack,
                },
            };

            if ((program.Flags & ELF::ProgramFlag::Execute) != 0 &&
                header->Entry >= program.VirtualAddress && header->Entry - program.VirtualAddress < program.MemorySize)
                entry_is_executable = true;

            load_segment_count++;
        }

        if (load_segment_count == 0)
            return UserExecutableLoadError::InvalidSegment;

        const VirtualAddress entry(header->Entry);
        if (!entry_is_executable || entry.IsNull() || !PageMap::IsCanonical(entry) || entry.Value() >= UserAddressLimit)
            return UserExecutableLoadError::InvalidEntryPoint;

        if (options.StackPageCount == 0 || !options.StackBase.IsPageAligned() ||
            options.StackBase.Value() < MinimumUserAddress + PageSize)
            return UserExecutableLoadError::InvalidStack;

        Uint64 stack_size{};
        if (options.StackPageCount > MaximumValue / PageSize)
            return UserExecutableLoadError::InvalidStack;
        stack_size = options.StackPageCount * PageSize;

        Uint64 stack_end{};
        if (!TryAdd(options.StackBase.Value(), stack_size, stack_end) ||
            !IsUserRange(options.StackBase.Value(), stack_end))
            return UserExecutableLoadError::InvalidStack;

        const VirtualAddress stack_guard = options.StackBase - PageSize;
        const VirtualSpan stack_span{ options.StackBase, options.StackPageCount };

        for (Uint64 index = 0; index < load_segment_count; index++) {
            const Uint64 segment_start = plans[index].Virtual.Base.Value();
            const Uint64 segment_end = segment_start + plans[index].Virtual.SizeBytes();

            if (Overlaps(options.StackBase.Value(), stack_end, segment_start, segment_end) ||
                Overlaps(stack_guard.Value(), stack_guard.Value() + PageSize, segment_start, segment_end))
                return UserExecutableLoadError::InvalidStack;
        }

        PageMap& address_space = *process.AddressSpace();
        if (address_space.IsMapped(stack_guard) || !SpanIsUnmapped(address_space, stack_span))
            return UserExecutableLoadError::AddressAlreadyMapped;

        for (Uint64 index = 0; index < load_segment_count; index++) {
            if (!SpanIsUnmapped(address_space, plans[index].Virtual))
                return UserExecutableLoadError::AddressAlreadyMapped;
        }

        output.m_EntryPoint = entry;
        output.m_StackGuard = stack_guard;
        output.m_Stack = stack_span;
        output.m_SegmentCount = load_segment_count;

        for (Uint64 index = 0; index < load_segment_count; index++) {
            output.m_Segments[index].Virtual = plans[index].Virtual;
            output.m_Segments[index].Mapping = plans[index].Mapping;
            output.m_Segments[index].FileSize = plans[index].FileSize;
            output.m_Segments[index].MemorySize = plans[index].MemorySize;
        }

        auto rollback = [&]() noexcept -> bool {
            bool success = true;

            for (Uint64 index = 0; index < output.m_SegmentCount; index++) {
                LoadedUserSegment& segment = output.m_Segments[index];
                bool unmapped = true;

                for (Uint64 page = 0; page < segment.Virtual.PageCount; page++) {
                    const VirtualAddress address = segment.Virtual.Base + page * PageSize;
                    if (!address_space.IsMapped(address)) continue;
                    if (address_space.UnmapPage(address) != MappingError::Success) {
                        success = false;
                        unmapped = false;
                    }
                }

                if (segment.Backing.IsValid() && unmapped &&
                    physical_memory.Release(segment.Backing) != PhysicalAllocationError::Success)
                    success = false;

                segment.Virtual = {};
                segment.Mapping = {};
                segment.FileSize = 0;
                segment.MemorySize = 0;
            }

            bool stack_unmapped = true;
            for (Uint64 page = 0; page < output.m_Stack.PageCount; page++) {
                const VirtualAddress address = output.m_Stack.Base + page * PageSize;
                if (!address_space.IsMapped(address)) continue;
                if (address_space.UnmapPage(address) != MappingError::Success) {
                    success = false;
                    stack_unmapped = false;
                }
            }

            if (output.m_StackBacking.IsValid() && stack_unmapped &&
                physical_memory.Release(output.m_StackBacking) != PhysicalAllocationError::Success)
                success = false;

            output.m_EntryPoint = {};
            output.m_StackGuard = {};
            output.m_Stack = {};
            output.m_SegmentCount = 0;
            output.m_Loaded = false;

            return success;
        };

        auto fail = [&](UserExecutableLoadError error) noexcept -> UserExecutableLoadError {
            return rollback() ? error : UserExecutableLoadError::RollbackFailed;
        };

        for (Uint64 index = 0; index < load_segment_count; index++) {
            LoadedUserSegment& segment = output.m_Segments[index];
            const SegmentPlan& plan = plans[index];

            if (physical_memory.AllocateContiguous(segment.Virtual.PageCount, segment.Backing) != PhysicalAllocationError::Success)
                return fail(UserExecutableLoadError::AllocationFailed);

            if (!ZeroAllocation(segment.Backing))
                return fail(UserExecutableLoadError::DirectMapUnavailable);

            const VirtualAddress direct = Layout::DirectMapAddress(segment.Backing.Base());
            if (direct.IsNull())
                return fail(UserExecutableLoadError::DirectMapUnavailable);

            auto* destination = reinterpret_cast<Uint8*>(direct.Value());
            const Uint8* source = image + plan.FileOffset;
            for (Uint64 byte = 0; byte < plan.FileSize; byte++)
                destination[byte] = source[byte];

            const MappingError mapping_error = address_space.MapRange(
                segment.Virtual.Base,
                segment.Backing.Base(),
                segment.Virtual.PageCount,
                segment.Mapping
            );
            if (mapping_error != MappingError::Success)
                return fail(UserExecutableLoadError::MappingFailed);
        }

        if (physical_memory.AllocateContiguous(options.StackPageCount, output.m_StackBacking) != PhysicalAllocationError::Success)
            return fail(UserExecutableLoadError::AllocationFailed);

        if (!ZeroAllocation(output.m_StackBacking))
            return fail(UserExecutableLoadError::DirectMapUnavailable);

        const MappingOptions stack_mapping{
            .Access = PageAccess::Read | PageAccess::Write | PageAccess::User,
            .Cache = CachePolicy::WriteBack,
        };

        if (address_space.MapRange(options.StackBase, output.m_StackBacking.Base(), options.StackPageCount, stack_mapping) != MappingError::Success)
            return fail(UserExecutableLoadError::MappingFailed);

        if (address_space.IsMapped(stack_guard))
            return fail(UserExecutableLoadError::MappingFailed);

        output.m_Loaded = true;

        Diagnostics::Write("[zOS/ELF] ELF64 AMD64 static executable validated.\n");
        Diagnostics::Write("[zOS/ELF] Load segments: ");
        Diagnostics::WriteDecimal(load_segment_count);
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/ELF] Entry: ");
        Diagnostics::WriteHex(entry.Value());
        Diagnostics::Write("\n");
        Diagnostics::Write("[zOS/ELF] Load segments mapped.\n");
        Diagnostics::Write("[zOS/ELF] User stack created.\n");

        return UserExecutableLoadError::Success;
    }

    const char* UserExecutableLoader::Describe(UserExecutableLoadError error) noexcept {
        switch (error) {
        case UserExecutableLoadError::Success: return "success";
        case UserExecutableLoadError::InvalidDependency: return "user executable loader dependency is invalid";
        case UserExecutableLoadError::OutputAlreadyLoaded: return "user image output already owns executable state";
        case UserExecutableLoadError::ImageTooSmall: return "user executable is smaller than an ELF64 header";
        case UserExecutableLoadError::InvalidMagic: return "user executable has invalid ELF magic";
        case UserExecutableLoadError::UnsupportedClass: return "user executable is not ELF64";
        case UserExecutableLoadError::UnsupportedEndianness: return "user executable is not little-endian";
        case UserExecutableLoadError::UnsupportedVersion: return "user executable uses an unsupported ELF version";
        case UserExecutableLoadError::UnsupportedFileType: return "user executable is not a static ET_EXEC image";
        case UserExecutableLoadError::UnsupportedMachine: return "user executable does not target AMD64";
        case UserExecutableLoadError::InvalidHeader: return "user executable ELF header is invalid";
        case UserExecutableLoadError::InvalidProgramHeaderTable: return "user executable program-header table is invalid";
        case UserExecutableLoadError::DynamicImageUnsupported: return "dynamic or interpreted user executables are not supported yet";
        case UserExecutableLoadError::TooManyLoadSegments: return "user executable contains too many load segments";
        case UserExecutableLoadError::InvalidSegment: return "user executable contains an invalid load segment";
        case UserExecutableLoadError::UnsupportedSegmentPermissions: return "user executable requests unsupported segment permissions";
        case UserExecutableLoadError::SegmentOverlap: return "user executable load segments overlap";
        case UserExecutableLoadError::AddressAlreadyMapped: return "user executable target address is already mapped";
        case UserExecutableLoadError::InvalidEntryPoint: return "user executable entry point is not inside executable user memory";
        case UserExecutableLoadError::InvalidStack: return "user executable stack layout is invalid";
        case UserExecutableLoadError::AllocationFailed: return "failed to allocate physical backing for user executable";
        case UserExecutableLoadError::DirectMapUnavailable: return "user executable backing is outside the kernel direct map";
        case UserExecutableLoadError::MappingFailed: return "failed to map user executable memory";
        case UserExecutableLoadError::RollbackFailed: return "user executable load failed and rollback could not restore a clean address space";
        default: return "unknown user executable load error";
        }
    }
}