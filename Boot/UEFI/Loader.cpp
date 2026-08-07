#include <Boot/Protocol.hpp>

#include <Boot/UEFI/UEFI.hpp>
#include <Boot/UEFI/ELF.hpp>

extern "C" [[noreturn]] void TransferControl(
    Zos::Boot::UEFI::PhysicalAddress entryPoint,
    Zos::Boot::UEFI::PhysicalAddress stackTop,
    const Zos::Boot::BootEnvironment_V1* environment
) noexcept;

namespace {
    using namespace Zos::Boot;

    class TextWriter final {
    public:
        explicit TextWriter(UEFI::SystemTable& st) noexcept
            : m_ConsoleOutput(st.ConsoleOutput) {}

        void Write(const char* msg) noexcept {
            while (*msg != '\0') {
                WriteChar(*msg);
                msg++;
            }
        }

        void WriteLine(const char* msg) noexcept {
            Write(msg);
            WriteChar('\n');
        }

        void WriteDecimal(UEFI::Uint64 value) noexcept {
            char buffer[21];
            UEFI::UintN position = sizeof(buffer);

            do {
                const auto digit = static_cast<UEFI::Uint8>(value % 10);
                buffer[--position] = static_cast<char>('0' + digit);
                value /= 10;
            } while (value != 0);

            while (position < sizeof(buffer)) {
                WriteChar(buffer[position]);
                ++position;
            }
        }

        void DisableConsole() noexcept {
            m_ConsoleOutput = nullptr;
        }

        void WriteHex(UEFI::Uint64 value) noexcept {
            static constexpr char HexDigits[] = "0123456789ABCDEF";
            Write("0x");

            bool emitted_digit = false;
            for (int shift = 60; shift >= 0; shift -= 4) {
                const auto digit = static_cast<UEFI::Uint8>((value >> shift) & 0xF);
                if (digit != 0 || emitted_digit || shift == 0) {
                    WriteChar(HexDigits[digit]);
                    emitted_digit = true;
                }
            }
        }

    private:
        static void WriteDebugChar(char value) noexcept {
            constexpr unsigned short DebugPort = 0xE9;
            __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(DebugPort));
        }

        void WriteConsoleChar(char value) noexcept {
            if (m_ConsoleOutput == nullptr || m_ConsoleOutput->OutputString == nullptr)
                return;

            UEFI::Char16 character[2]{ static_cast<UEFI::Char16>(static_cast<unsigned char>(value)), u'\0' };
            m_ConsoleOutput->OutputString(m_ConsoleOutput, character);
        }

        void WriteChar(char value) noexcept {
            WriteDebugChar(value);

            if (value == '\n')
                WriteConsoleChar('\r');
            WriteConsoleChar(value);
        }

        UEFI::SimpleTextOutputProtocol* m_ConsoleOutput;
    };

    class BootLoader final {
    public:
        BootLoader(UEFI::Handle image_handle, UEFI::SystemTable& st) noexcept  
            : m_ImageHandle(image_handle), m_BootServices(st.pBootServices), m_Output(st) {}
        
        ~BootLoader() { ReleaseResources(); }

        BootLoader(const BootLoader&) = delete;
        BootLoader& operator=(const BootLoader&) = delete;

        UEFI::Status Run() noexcept {
            m_Output.WriteLine("[zOS/Boot] UEFI Application entered.");

            if (m_BootServices == nullptr) 
                return ReportFailure("locate UEFI boot services", UEFI::InvalidParameter);
            
            if (m_BootServices->AllocatePages == nullptr ||
                m_BootServices->FreePages == nullptr ||
                m_BootServices->GetMemoryMap == nullptr ||
                m_BootServices->AllocatePool == nullptr ||
                m_BootServices->FreePool == nullptr ||
                m_BootServices->ExitBootServices == nullptr ||
                m_BootServices->OpenProtocol == nullptr ||
                m_BootServices->CloseProtocol == nullptr ||
                m_BootServices->CopyMem == nullptr ||
                m_BootServices->SetMem == nullptr)
                return ReportFailure("validate required boot services", UEFI::Unsupported);

            UEFI::Status status = OpenBootVolume();
            if (UEFI::IsError(status)) return status;

            status = OpenKernelFile();
            if (UEFI::IsError(status)) return status;

            status = ReadKernelImage();
            if (UEFI::IsError(status)) return status;

            status = ValidateKernelImage();
            if (UEFI::IsError(status)) return status;

            m_Output.Write("[zOS/Boot] Kernel size: ");
            m_Output.WriteDecimal(m_KernelImageSize);
            m_Output.WriteLine(" bytes");

            m_Output.Write("[zOS/Boot] Entry point: ");
            m_Output.WriteHex(m_EntryPoint);
            m_Output.WriteLine("");

            m_Output.Write("[zOS/Boot] Load segments: ");
            m_Output.WriteDecimal(m_LoadSegmentCount);
            m_Output.WriteLine("");
            
            m_Output.WriteLine("[zOS/Boot] ELF64 AMD64 executable verified");
            m_Output.WriteLine("[zOS/Boot] Kernel image validation complete");

            status = LoadKernelSegments();
            if (UEFI::IsError(status)) return status;

            status = PrepareHandoff();
            if (UEFI::IsError(status)) return status;
            
            CompleteHandoff();
        }

    private:
        static bool RangeIsInside(UEFI::Uint64 offset, UEFI::Uint64 length, UEFI::Uint64 total) noexcept {
            return offset <= total && length <= total - offset;
        }

        static constexpr UEFI::Uint64 PageSize{ 4096 };
        static constexpr UEFI::UintN KernelStackPageCount{ 16 };
        static constexpr UEFI::UintN MemoryMapSlackDescriptorCount{ 64 };
        static constexpr UEFI::UintN ExitBootServicesAttemptCount{ 4 };
        static constexpr UEFI::Uint64 MaximumAddress{ ~UEFI::Uint64{ 0 } };

        static bool IsPowerOfTwo(UEFI::Uint64 value) noexcept {
            return value != 0 && (value & (value - 1)) == 0;
        }

        static UEFI::Uint64 AlignDownToPage(UEFI::Uint64 value) noexcept {
            return value & ~(PageSize - 1);
        }

        static bool TryAlignUpToPage(UEFI::Uint64 value, UEFI::Uint64& aligned) noexcept {
            if (value > MaximumAddress - (PageSize - 1)) return false;
            aligned = (value + PageSize - 1) & ~(PageSize - 1);
            return true;
        }

        static bool RangesOverlap(UEFI::Uint64 first_base, UEFI::Uint64 first_size, UEFI::Uint64 second_base, UEFI::Uint64 second_size) noexcept {
            if (first_size == 0 || second_size == 0) return false;


            const UEFI::Uint64 first_end = first_base + first_size;
            const UEFI::Uint64 second_end = second_base + second_size;
            return first_base < second_end && second_base < first_end;
        }

        UEFI::Status OpenBootVolume() noexcept {
            UEFI::Status status = m_BootServices->OpenProtocol(m_ImageHandle, &UEFI::LoadedImageProtocolGuid, reinterpret_cast<void**>(&m_LoadedImage), m_ImageHandle, nullptr, UEFI::OpenProtocolGetProtocol);
            if (UEFI::IsError(status)) 
                return ReportFailure("open Load Image Protocol", status);

            m_LoadedImageProtocolOpen = true;

            if (m_LoadedImage == nullptr || m_LoadedImage->DeviceHandle == nullptr)
                return ReportFailure("resolve loader device handle", UEFI::NotFound);

            m_BootDeviceHandle = m_LoadedImage->DeviceHandle;

            status = m_BootServices->OpenProtocol(m_BootDeviceHandle, &UEFI::SimpleFileSystemProtocolGuid, reinterpret_cast<void**>(&m_FileSystem), m_ImageHandle, nullptr, UEFI::OpenProtocolGetProtocol);
            if (UEFI::IsError(status))
                return ReportFailure("open Simple File System Protocol", status);

            m_FileSystemProtocolOpen = true;

            if (m_FileSystem == nullptr || m_FileSystem->OpenVolume == nullptr)
                return ReportFailure("validate Simple File System Protocol", UEFI::Unsupported);

            status = m_FileSystem->OpenVolume(m_FileSystem, &m_RootDirectory);
            if (UEFI::IsError(status))
                return ReportFailure("open boot volume", status);

            if (m_RootDirectory == nullptr) 
                return ReportFailure("validate boot volume root", UEFI::DeviceError);

            m_Output.WriteLine("[zOS/Boot] Boot volume opened.");
            return UEFI::Success;
        }

        UEFI::Status OpenKernelFile() noexcept {
            if (m_RootDirectory->Open == nullptr)
                return ReportFailure("validate root file protocol", UEFI::Unsupported);
            
            static UEFI::Char16 KernelPath[] = u"\\zOS\\Kernel.elf";

            const UEFI::Status status = m_RootDirectory->Open(m_RootDirectory, &m_KernelFile, KernelPath, UEFI::FileModeRead, 0);
            if (UEFI::IsError(status))
                return ReportFailure("open \\zOS\\Kernel.elf", status);

            if (m_KernelFile == nullptr)
                return ReportFailure("validate kernel file handle", UEFI::DeviceError);

            m_Output.WriteLine("[zOS/Boot] Opened \\zOS\\Kernel.elf");
            return UEFI::Success;
        }

        UEFI::Status ReadKernelImage() noexcept {
            if (m_KernelFile->GetInfo == nullptr || m_KernelFile->Read == nullptr) 
                return ReportFailure("validate kernel file protocol", UEFI::Unsupported);

            UEFI::UintN information_size = 0;
            UEFI::Status status = m_KernelFile->GetInfo(m_KernelFile, &UEFI::FileInfoGuid, &information_size, nullptr);
            if (status != UEFI::BufferTooSmall) {
                const UEFI::Status failure_status = UEFI::IsError(status) ? status : UEFI::LoadError;
                return ReportFailure("query kernel file information size", failure_status);
            }

            if (information_size < __builtin_offsetof(UEFI::FileInfo, FileName)) 
                return ReportFailure("validate kernel file information size", UEFI::VolumeCorrupted);

            status = m_BootServices->AllocatePool(UEFI::MemoryType::LoaderData, information_size, &m_FileInformationBuffer);
            if (UEFI::IsError(status))
                return ReportFailure("allocate kernel file information", status);

            status = m_KernelFile->GetInfo(m_KernelFile, &UEFI::FileInfoGuid, &information_size, m_FileInformationBuffer);
            if (UEFI::IsError(status)) 
                return ReportFailure("read kernel file information", status);

            const auto* information = static_cast<const UEFI::FileInfo*>(m_FileInformationBuffer);
            if (information->FileSize == 0)
                return ReportFailure("validate nonempty kernel image", UEFI::LoadError);

            m_KernelImageSize = information->FileSize;

            status = m_BootServices->FreePool(m_FileInformationBuffer);
            if (UEFI::IsError(status))
                return ReportFailure("release kernel file information", status);

            m_FileInformationBuffer = nullptr;

            status = m_BootServices->AllocatePool(UEFI::MemoryType::LoaderData, m_KernelImageSize, &m_KernelImage);
            if (UEFI::IsError(status))
                return ReportFailure("allocate kernel image buffer", status);

            UEFI::UintN bytes_read = m_KernelImageSize;
            status = m_KernelFile->Read(m_KernelFile, &bytes_read, m_KernelImage);
            if (UEFI::IsError(status))
                return ReportFailure("read kernel image", status);
            
            if (bytes_read != m_KernelImageSize)
                return ReportFailure("read complete kernel image", UEFI::DeviceError);

            m_Output.WriteLine("[zOS/Boot] Kernel image read into memory.");
            return UEFI::Success;
        }

        UEFI::Status ValidateKernelImage() noexcept {
            using namespace ELF;

            if (m_KernelImageSize < sizeof(Header))
                return ReportValidationFailure("kernel file is smaller than an ELF64 header");

            const auto* header = static_cast<const Header*>(m_KernelImage);
            const auto* identification = header->Identification;

            if (identification[0] != Magic0 ||
                identification[1] != Magic1 ||
                identification[2] != Magic2 ||
                identification[3] != Magic3) {
                return ReportValidationFailure("invalid ELF magic");
            }

            if (identification[4] != Class64)
                return ReportValidationFailure("kernel is not ELF64");

            if (identification[5] != LittleEndian)
                return ReportValidationFailure("kernel is not little-endian");

            if (identification[6] != CurrentVersion || header->Version != CurrentVersion)
                return ReportValidationFailure("unsupported ELF version");

            if (header->TypeValue != Type::Executable)
                return ReportValidationFailure("kernel is not an executable ELF image");

            if (header->MachineValue != Machine::AMD64)
                return ReportValidationFailure("kernel does not target AMD64");

            if (header->HeaderSize != sizeof(Header))
                return ReportValidationFailure("unexpected ELF header size");

            if (header->ProgramHeaderEntrySize != sizeof(ProgramHeader))
                return ReportValidationFailure("unexpected ELF program-header size");

            if (header->ProgramHeaderCount == 0)
                return ReportValidationFailure("kernel has no program headers");

            if (header->Entry == 0)
                return ReportValidationFailure("kernel entry point is null");

            if (header->ProgramHeaderOffset > m_KernelImageSize)
                return ReportValidationFailure("program-header table starts outside the kernel file");

            if ((header->ProgramHeaderOffset % alignof(ProgramHeader)) != 0)
                return ReportValidationFailure("program-header table is improperly aligned");

            const UEFI::Uint64 remainingBytes = m_KernelImageSize - header->ProgramHeaderOffset;
            if (header->ProgramHeaderCount > remainingBytes / sizeof(ProgramHeader))
                return ReportValidationFailure("program-header table extends beyond the kernel file");

            const auto* imageBytes = static_cast<const UEFI::Uint8*>(m_KernelImage);
            const auto* programHeaders = reinterpret_cast<const ProgramHeader*>(
                imageBytes + header->ProgramHeaderOffset
            );

            bool foundExecutableLoadSegment = false;
            bool entryPointIsExecutable = false;
            UEFI::UintN loadSegmentCount = 0;

            for (UEFI::UintN index = 0; index < header->ProgramHeaderCount; ++index) {
                const ProgramHeader& programHeader = programHeaders[index];

                if (programHeader.TypeValue != ProgramType::Load)
                    continue;

                ++loadSegmentCount;

                if (!RangeIsInside(
                        programHeader.Offset,
                        programHeader.FileSize,
                        m_KernelImageSize)) {
                    return ReportValidationFailure("a load segment extends beyond the kernel file");
                }

                if (programHeader.MemorySize < programHeader.FileSize)
                    return ReportValidationFailure("a load segment is smaller in memory than in the file");

                if (programHeader.Alignment > 1) {
                    if (!IsPowerOfTwo(programHeader.Alignment))
                        return ReportValidationFailure("a load segment has invalid alignment");

                    if ((programHeader.VirtAddress % programHeader.Alignment) !=
                        (programHeader.Offset % programHeader.Alignment)) {
                        return ReportValidationFailure("a load segment violates ELF alignment rules");
                    }
                }

                constexpr UEFI::Uint64 MaximumAddress = ~UEFI::Uint64{ 0 };
                if (programHeader.MemorySize > MaximumAddress - programHeader.VirtAddress)
                    return ReportValidationFailure("a load segment virtual range overflows");

                if (programHeader.MemorySize > MaximumAddress - programHeader.PhysAddress)
                    return ReportValidationFailure("a load segment physical range overflows");

                if ((programHeader.Flags & ProgramFlags::Execute) == 0)
                    continue;

                foundExecutableLoadSegment = true;

                if (programHeader.MemorySize != 0 &&
                    header->Entry >= programHeader.VirtAddress &&
                    header->Entry - programHeader.VirtAddress < programHeader.MemorySize) {
                    entryPointIsExecutable = true;
                }
            }

            if (loadSegmentCount == 0)
                return ReportValidationFailure("kernel has no loadable segments");

            if (!foundExecutableLoadSegment)
                return ReportValidationFailure("kernel has no executable load segment");

            if (!entryPointIsExecutable)
                return ReportValidationFailure("kernel entry point is not inside an executable segment");

            m_EntryPoint = header->Entry;
            m_LoadSegmentCount = loadSegmentCount;
            return UEFI::Success;
        }

        UEFI::Status LoadKernelSegments() noexcept {
            using namespace ELF;

            const auto* header = static_cast<const Header*>(m_KernelImage);
            const auto* image_bytes = static_cast<const UEFI::Uint8*>(m_KernelImage);
            const auto* program_headers = reinterpret_cast<const ProgramHeader*>(image_bytes + header->ProgramHeaderOffset);

            UEFI::Uint64 minimum_address = MaximumAddress;
            UEFI::Uint64 maximum_address = 0;
            bool found_nonempty_segment = false;

            for (UEFI::UintN i = 0; i < header->ProgramHeaderCount; i++) {
                const ProgramHeader& segment = program_headers[i];

                if (segment.TypeValue != ProgramType::Load || segment.MemorySize == 0) continue;
                if (segment.PhysAddress != segment.VirtAddress)
                    return ReportValidationFailure("the current kernel must use identifical physical and virtual load addresses");

                for (UEFI::UintN prev_i = 0; prev_i < i; prev_i++) {
                    const ProgramHeader& prev = program_headers[prev_i];
                    if (prev.TypeValue != ProgramType::Load || prev.MemorySize == 0) continue;
                    if (RangesOverlap(prev.PhysAddress, prev.MemorySize, segment.PhysAddress, segment.MemorySize))
                        return ReportValidationFailure("kernel load segments overlap in physical memory");
                }

                const UEFI::Uint64 segment_start = AlignDownToPage(segment.PhysAddress);
                const UEFI::Uint64 segment_end = segment.PhysAddress + segment.MemorySize;
                UEFI::Uint64 aligned_segment_end = 0;

                if (!TryAlignUpToPage(segment_end, aligned_segment_end))
                    return ReportValidationFailure("a kernel load segment cannot be page-aligned safely!");
                
                if (segment_start < minimum_address)
                    minimum_address = segment_start;

                if (aligned_segment_end > maximum_address)
                    maximum_address = aligned_segment_end;

                found_nonempty_segment = true;
            }

            if (!found_nonempty_segment || maximum_address <= minimum_address) 
                return ReportValidationFailure("kernel has no nonempty loadable memory range");

            const UEFI::Uint64 allocation_size = maximum_address - minimum_address;
            if ((allocation_size % PageSize) != 0) 
                return ReportValidationFailure("kernel allocation span is not page-aligned");
            
            const UEFI::Uint64 page_count = allocation_size / PageSize;
            if (page_count == 0) 
                return ReportValidationFailure("kernel allocation requires zero pages");

            UEFI::PhysicalAddress allocation_address = minimum_address;
            UEFI::Status status = m_BootServices->AllocatePages(UEFI::AllocateType::Address, UEFI::MemoryType::LoaderData, page_count, &allocation_address);
            if (UEFI::IsError(status))
                return ReportFailure("allocate the kernel's physical memory span", status);
            
            m_LoadedKernelBase = allocation_address;
            m_LoadedKernelSize = allocation_size;
            m_LoadedKernelPageCount = page_count;
            m_KernelPagesAllocated = true;

            if (allocation_address != minimum_address)
                return ReportValidationFailure("Firmware returned an unexpected address for an exact kernel allocation");

            auto* allocation = reinterpret_cast<void*>(static_cast<UEFI::UintN>(m_LoadedKernelBase));
            m_BootServices->SetMem(allocation, m_LoadedKernelSize, 0);
        
            UEFI::UintN loaded_segment_index = 0;
            for (UEFI::UintN i = 0; i < header->ProgramHeaderCount; i++) {
                const ProgramHeader& segment = program_headers[i];
                if (segment.TypeValue != ProgramType::Load) continue;
                if (segment.FileSize != 0) {
                    auto* destination = reinterpret_cast<void*>(static_cast<UEFI::UintN>(segment.PhysAddress));
                    const void* source = image_bytes + segment.Offset;
                    m_BootServices->CopyMem(destination, source, segment.FileSize);
                }

                m_Output.Write("[zOS/Boot] Loaded segment ");
                m_Output.WriteDecimal(loaded_segment_index);
                m_Output.Write(" at ");
                m_Output.WriteHex(segment.PhysAddress);
                m_Output.WriteLine("");
                loaded_segment_index++;
            }

            status = VerifyLoadedKernel();
            if (UEFI::IsError(status)) return status;

            m_Output.Write("[zOS/Boot] Kernel allocation base: ");
            m_Output.WriteHex(m_LoadedKernelBase);
            m_Output.WriteLine("");

            m_Output.Write("[zOS/Boot] Kernel allocation size: ");
            m_Output.WriteDecimal(m_LoadedKernelSize);
            m_Output.WriteLine(" bytes");

            m_Output.WriteLine("[zOS/Boot] Kernel segments loaded and verified.");

            return UEFI::Success;
        }

        UEFI::Status VerifyLoadedKernel() noexcept {
            using namespace ELF;

            const auto* header = static_cast<const Header*>(m_KernelImage);
            const auto* imageBytes = static_cast<const UEFI::Uint8*>(m_KernelImage);
            const auto* programHeaders = reinterpret_cast<const ProgramHeader*>(
                imageBytes + header->ProgramHeaderOffset
            );

            const UEFI::Uint64 allocationEnd = m_LoadedKernelBase + m_LoadedKernelSize;
            if (allocationEnd < m_LoadedKernelBase)
                return ReportLoadFailure("loaded kernel allocation range overflows");

            if (m_EntryPoint < m_LoadedKernelBase || m_EntryPoint >= allocationEnd)
                return ReportLoadFailure("kernel entry point lies outside the loaded allocation");

            for (UEFI::UintN index = 0; index < header->ProgramHeaderCount; ++index) {
                const ProgramHeader& segment = programHeaders[index];

                if (segment.TypeValue != ProgramType::Load)
                    continue;

                if (segment.PhysAddress < m_LoadedKernelBase ||
                    !RangeIsInside(
                        segment.PhysAddress - m_LoadedKernelBase,
                        segment.MemorySize,
                        m_LoadedKernelSize)) {
                    return ReportLoadFailure("a loaded segment lies outside the kernel allocation");
                }

                const auto* source = imageBytes + segment.Offset;
                const auto* destination = reinterpret_cast<const UEFI::Uint8*>(
                    static_cast<UEFI::UintN>(segment.PhysAddress)
                );

                for (UEFI::Uint64 byte = 0; byte < segment.FileSize; ++byte) {
                    if (destination[byte] != source[byte])
                        return ReportLoadFailure("a loaded segment differs from its ELF file data");
                }

                for (UEFI::Uint64 byte = segment.FileSize; byte < segment.MemorySize; ++byte) {
                    if (destination[byte] != 0)
                        return ReportLoadFailure("a loaded segment's zero-fill region is not zero");
                }
            }

            return UEFI::Success;
        }

        UEFI::Status ReleaseKernelPages() noexcept {
            if (!m_KernelPagesAllocated) return UEFI::Success;

            const UEFI::Status status = m_BootServices->FreePages(m_LoadedKernelBase, m_LoadedKernelPageCount);
            if (UEFI::IsError(status))
                return ReportFailure("releasde the test kernel allocation", status);

            m_LoadedKernelBase = 0;
            m_LoadedKernelSize = 0;
            m_LoadedKernelPageCount = 0;
            m_KernelPagesAllocated = false;
            return UEFI::Success;
        }

        UEFI::Status CloseBootInputResources() noexcept {
            if (m_KernelFile != nullptr) {
                if (m_KernelFile->Close == nullptr)
                    return ReportFailure("validate kernel file close operation", UEFI::Unsupported);
                
                const UEFI::Status status = m_KernelFile->Close(m_KernelFile);
                if (UEFI::IsError(status))
                    return ReportFailure("close the kernel file", status);

                m_KernelFile = nullptr;
            }

            if (m_RootDirectory != nullptr) {
                if (m_RootDirectory->Close == nullptr)
                    return ReportFailure("validate root directory close operation", UEFI::Unsupported);

                const UEFI::Status status = m_RootDirectory->Close(m_RootDirectory);
                if (UEFI::IsError(status))
                    return ReportFailure("close the boot volume", status);

                m_RootDirectory = nullptr;
            }

            if (m_KernelImage != nullptr) {
                const UEFI::Status status = m_BootServices->FreePool(m_KernelImage);
                if (UEFI::IsError(status))
                    return ReportFailure("release the temporary kernel file buffer", status);

                m_KernelImage = nullptr;
                m_KernelImageSize = 0;
            }

            if (m_FileInformationBuffer != nullptr) {
                const UEFI::Status status = m_BootServices->FreePool(m_FileInformationBuffer);
                if (UEFI::IsError(status))
                    return ReportFailure("release kernel file information", status);

                m_FileInformationBuffer = nullptr;
            }

            if (m_FileSystemProtocolOpen) {
                const UEFI::Status status = m_BootServices->CloseProtocol(
                    m_BootDeviceHandle,
                    &UEFI::SimpleFileSystemProtocolGuid,
                    m_ImageHandle,
                    nullptr
                );

                if (UEFI::IsError(status))
                    return ReportFailure("close Simple File System Protocol", status);

                m_FileSystemProtocolOpen = false;
                m_FileSystem = nullptr;
            }

            if (m_LoadedImageProtocolOpen) {
                const UEFI::Status status = m_BootServices->CloseProtocol(
                    m_ImageHandle,
                    &UEFI::LoadedImageProtocolGuid,
                    m_ImageHandle,
                    nullptr
                );

                if (UEFI::IsError(status))
                    return ReportFailure("close Loaded Image Protocol", status);

                m_LoadedImageProtocolOpen = false;
                m_LoadedImage = nullptr;
            }

            return UEFI::Success;
        }

        UEFI::Status AllocateKernelStack() noexcept {
            UEFI::PhysicalAddress stack_base = 0;
            const UEFI::Status status = m_BootServices->AllocatePages(UEFI::AllocateType::AnyPages, UEFI::MemoryType::LoaderData, KernelStackPageCount, &stack_base);
            if (UEFI::IsError(status))
                return ReportFailure("allocate the kernel stack", status);

            m_KernelStackBase = stack_base;
            m_KernelStackPageCount = KernelStackPageCount;
            m_KernelStackAllocated = true;

            const UEFI::Uint64 stack_size = KernelStackPageCount * PageSize;
            if (stack_base > MaximumAddress - stack_size)
                return ReportValidationFailure("kernel stack range overflows");

            m_KernelStackSize = stack_size;
            m_KernelStackTop = stack_base + stack_size;
            
            m_BootServices->SetMem(reinterpret_cast<void*>(static_cast<UEFI::UintN>(stack_base)), stack_size, 0);

            return UEFI::Success;
        }

        UEFI::Status AllocateBootEnvironment() noexcept {
            UEFI::PhysicalAddress environment_base = 0;
            const UEFI::Status status = m_BootServices->AllocatePages(UEFI::AllocateType::AnyPages, UEFI::MemoryType::LoaderData, 1, &environment_base);
            if (UEFI::IsError(status))
                return ReportFailure("allocate the boot environment", status);

            m_BootEnvironmentBase = environment_base;
            m_BootEnvironmentPageCount = 1;
            m_BootEnvironmentAllocated = true;
            m_BootEnvironment = reinterpret_cast<BootEnvironment_V1*>(static_cast<UEFI::UintN>(environment_base));

            m_BootServices->SetMem(m_BootEnvironment, PageSize, 0);
            return UEFI::Success;
        }

        UEFI::Status ResizeMemoryMapStorage(UEFI::UintN required_size, UEFI::UintN descriptor_size) noexcept {
            if (descriptor_size == 0) 
                descriptor_size = sizeof(UEFI::MemoryDescriptor);
            
            if (descriptor_size > MaximumAddress / MemoryMapSlackDescriptorCount)
                return ReportFailure("calculate memory map slack", UEFI::OutOfResources);

            const UEFI::Uint64 slack = static_cast<UEFI::Uint64>(descriptor_size) * MemoryMapSlackDescriptorCount;
            if (required_size > MaximumAddress - slack)
                return ReportFailure("calculate memory map storage size", UEFI::OutOfResources);

            UEFI::Uint64 requested_size = required_size + slack;
            UEFI::Uint64 aligned_size = 0;
            if (!TryAlignUpToPage(requested_size, aligned_size))
                return ReportFailure("page-align memory map storage", UEFI::OutOfResources);

            const UEFI::UintN page_count = aligned_size / PageSize;
            if (page_count == 0) 
                return ReportFailure("calculate memory map page count", UEFI::OutOfResources);

            if (m_MemoryMapAllocated) {
                const UEFI::Status release_status = m_BootServices->FreePages(m_MemoryMapBase, m_MemoryMapPageCount);
                if (UEFI::IsError(release_status))
                    return ReportFailure("resize the UEFI memory map storage", release_status);
                
                m_MemoryMapAllocated = false;
                m_MemoryMapBase = 0;
                m_MemoryMapPageCount = 0;
                m_MemoryMapCapacity = 0;
            }

            UEFI::PhysicalAddress map_base = 0;
            const UEFI::Status allocate_status = m_BootServices->AllocatePages(UEFI::AllocateType::AnyPages, UEFI::MemoryType::LoaderData, page_count, &map_base);
            if (UEFI::IsError(allocate_status))
                return ReportFailure("allocate UEFI memory map storage", allocate_status);

            m_MemoryMapBase = map_base;
            m_MemoryMapPageCount = page_count;
            m_MemoryMapCapacity = page_count * PageSize;
            m_MemoryMapAllocated = true;

            if (m_BootEnvironment != nullptr) {
                m_BootEnvironment->MemoryMapStorage.Base = m_MemoryMapBase;
                m_BootEnvironment->MemoryMapStorage.Size = m_MemoryMapCapacity;
            }

            return UEFI::Success;
        }

        UEFI::Status PrepareMemoryMapStorage() noexcept {
            UEFI::UintN required_size = 0;
            UEFI::UintN map_key = 0;
            UEFI::UintN descriptor_size = 0;
            UEFI::Uint32 descriptor_version = 0;

            const UEFI::Status status = m_BootServices->GetMemoryMap(&required_size, nullptr, &map_key, &descriptor_size, &descriptor_version);
            if (status != UEFI::BufferTooSmall) {
                const UEFI::Status failure_status = UEFI::IsError(status) ? status : UEFI::LoadError;
                return ReportFailure("query the UEFI memory map size", failure_status);
            }

            if (descriptor_size < sizeof(UEFI::MemoryDescriptor))
                return ReportFailure("validate the UEFI memory descriptor size", UEFI::Unsupported);

            return ResizeMemoryMapStorage(required_size, descriptor_size);
        }

        void PopulateBootEnvironment() noexcept {
            m_BootEnvironment->Signature = EnvironmentSignature;
            m_BootEnvironment->Version = ProtocolVersion;
            m_BootEnvironment->Size = sizeof(BootEnvironment_V1);
            m_BootEnvironment->KernelEntryPoint = m_EntryPoint;
            m_BootEnvironment->KernelImage.Base = m_LoadedKernelBase;
            m_BootEnvironment->KernelImage.Size = m_LoadedKernelSize;
            m_BootEnvironment->KernelStack.Base = m_KernelStackBase;
            m_BootEnvironment->KernelStack.Size = m_KernelStackSize;
            m_BootEnvironment->EnvironmentStorage.Base = m_BootEnvironmentBase;
            m_BootEnvironment->EnvironmentStorage.Size = PageSize;
            m_BootEnvironment->MemoryMapStorage.Base = m_MemoryMapBase;
            m_BootEnvironment->MemoryMapStorage.Size = m_MemoryMapCapacity;
            m_BootEnvironment->FirmwareSystemTable = reinterpret_cast<UEFI::UintN>(m_SystemTable);
        }

        UEFI::Status PrepareHandoff() noexcept {
            UEFI::Status status = CloseBootInputResources();
            if (UEFI::IsError(status)) return status;

            status = AllocateKernelStack();
            if (UEFI::IsError(status)) return status;

            status = AllocateBootEnvironment();
            if (UEFI::IsError(status)) return status;

            status = PrepareMemoryMapStorage();
            if (UEFI::IsError(status)) return status;

            PopulateBootEnvironment();

            m_Output.Write("[zOS/Boot] Kernel stack: ");
            m_Output.WriteHex(m_KernelStackBase);
            m_Output.Write(" - ");
            m_Output.WriteHex(m_KernelStackTop);
            m_Output.WriteLine("");

            m_Output.Write("[zOS/Boot] Boot environment: ");
            m_Output.WriteHex(m_BootEnvironmentBase);
            m_Output.WriteLine("");

            m_Output.Write("[zOS/Boot] Memory map capacity: ");
            m_Output.WriteDecimal(m_MemoryMapCapacity);
            m_Output.WriteLine(" bytes");

            return UEFI::Success;
        }

        UEFI::Status AcquireFinalMemoryMap(bool allow_resize) noexcept {
            for (;;) {
                UEFI::UintN memory_map_size = m_MemoryMapCapacity;
                UEFI::UintN map_key = 0;
                UEFI::UintN descriptor_size = 0;
                UEFI::Uint32 descriptor_version = 0;

                const UEFI::Status status = m_BootServices->GetMemoryMap(&memory_map_size, reinterpret_cast<UEFI::MemoryDescriptor*>(static_cast<UEFI::UintN>(m_MemoryMapBase)), &map_key, &descriptor_size, &descriptor_version);
                if (status == UEFI::BufferTooSmall && allow_resize) {
                    const UEFI::Status resize_status = ResizeMemoryMapStorage(memory_map_size, descriptor_size);
                    if (UEFI::IsError(resize_status)) return resize_status;
                    continue;
                }

                if (UEFI::IsError(status)) return status;

                if (descriptor_size < sizeof(UEFI::MemoryDescriptor) ||
                    descriptor_size == 0 ||
                    (memory_map_size % descriptor_size) != 0) {
                    return UEFI::LoadError;
                }

                m_MemoryMapSize = memory_map_size;
                m_MemoryMapKey = map_key;
                m_MemoryDescriptorSize = descriptor_size;
                m_MemoryDescriptorVersion = descriptor_version;

                m_BootEnvironment->MemoryMapStorage.Base = m_MemoryMapBase;
                m_BootEnvironment->MemoryMapStorage.Size = m_MemoryMapCapacity;
                m_BootEnvironment->MemoryMapSize = memory_map_size;
                m_BootEnvironment->MemoryMapDescriptorSize = descriptor_size;
                m_BootEnvironment->MemoryDescriptorVersion = descriptor_version;
                return UEFI::Success;
            }
        }

        [[noreturn]] void FatalHandoffFailure(const char* operation, UEFI::Status status) noexcept {
            m_Output.Write("[zOS/Boot] FATAL: Failed to ");
            m_Output.Write(operation);
            m_Output.Write(" (status ");
            m_Output.WriteHex(status);
            m_Output.WriteLine(").");

            for (;;) {
                __asm__ volatile("cli; hlt");
            }
        }

        [[noreturn]] void CompleteHandoff() noexcept {
            m_Output.WriteLine("[zOS/Boot] Acquiring final UEFI memory map.");
            m_Output.WriteLine("[zOS/Boot] Firmware console disabled for final handoff.");
            m_Output.DisableConsole();

            bool exit_attempted = false;

            for (UEFI::UintN attempt = 0; attempt < ExitBootServicesAttemptCount; attempt++) {
                const UEFI::Status map_status = AcquireFinalMemoryMap(!exit_attempted);
                if (UEFI::IsError(map_status))
                    FatalHandoffFailure("acquire the final UEFI mempry map", map_status);

                const UEFI::Status exit_status = m_BootServices->ExitBootServices(m_ImageHandle, m_MemoryMapKey);
                exit_attempted = true;
                if (exit_status == UEFI::Success) {
                    m_BootServicesExited = true;
                    m_Output.WriteLine("[zOS/Boot] UEFI boot services exited.");
                    m_Output.WriteLine("[zOS/Boot] Entering kernel.");

                    TransferControl(m_EntryPoint, m_KernelStackTop, m_BootEnvironment);
                }

                if (exit_status != UEFI::InvalidParameter)
                    FatalHandoffFailure("exit UEFI boot services", exit_status);

                m_Output.WriteLine("[zOS/Boot] Memory map changed; retrying ExitBootServices.");
            }

            FatalHandoffFailure("exit UEFI boot services after retries", UEFI::InvalidParameter);
        }

        UEFI::Status ReportLoadFailure(const char* reason) noexcept {
            m_Output.Write("[zOS/Boot] ERROR: Loaded kernel verification failed: ");
            m_Output.Write(reason);
            m_Output.WriteLine(".");
            return UEFI::LoadError;
        }


        UEFI::Status ReportFailure(const char* operation, UEFI::Status status) noexcept {
            m_Output.Write("[zOS/Boot] ERROR: Failed to ");
            m_Output.Write(operation);
            m_Output.Write(" (status ");
            m_Output.WriteHex(status);
            m_Output.WriteLine(").");
            return status;
        }

        UEFI::Status ReportValidationFailure(const char* reason) noexcept {
            m_Output.Write("[zOS/Boot] ERROR: Kernel validation failed: ");
            m_Output.Write(reason);
            m_Output.WriteLine(".");
            return UEFI::LoadError;
        }

        void ReleaseResources() noexcept {
            if (m_KernelPagesAllocated &&
                m_BootServices != nullptr &&
                m_BootServices->FreePages != nullptr) {
                ReleaseKernelPages();
            }

            if (m_KernelFile != nullptr && m_KernelFile->Close != nullptr) {
                m_KernelFile->Close(m_KernelFile);
                m_KernelFile = nullptr;
            }

            if (m_RootDirectory != nullptr && m_RootDirectory->Close != nullptr) {
                m_RootDirectory->Close(m_RootDirectory);
                m_RootDirectory = nullptr;
            }

            if (m_BootServices != nullptr && m_BootServices->FreePool != nullptr) {
                if (m_KernelImage != nullptr) {
                    m_BootServices->FreePool(m_KernelImage);
                    m_KernelImage = nullptr;
                }

                if (m_FileInformationBuffer != nullptr) {
                    m_BootServices->FreePool(m_FileInformationBuffer);
                    m_FileInformationBuffer = nullptr;
                }
            }

            if (m_BootServices != nullptr && m_BootServices->CloseProtocol != nullptr) {
                if (m_FileSystemProtocolOpen) {
                    m_BootServices->CloseProtocol(
                        m_BootDeviceHandle,
                        &UEFI::SimpleFileSystemProtocolGuid,
                        m_ImageHandle,
                        nullptr
                    );
                    m_FileSystemProtocolOpen = false;
                }

                if (m_LoadedImageProtocolOpen) {
                    m_BootServices->CloseProtocol(
                        m_ImageHandle,
                        &UEFI::LoadedImageProtocolGuid,
                        m_ImageHandle,
                        nullptr
                    );
                    m_LoadedImageProtocolOpen = false;
                }
            }
        }


        UEFI::Handle m_ImageHandle;
        UEFI::SystemTable* m_SystemTable;
        UEFI::BootServices* m_BootServices;
        TextWriter m_Output;

        UEFI::LoadedImageProtocol* m_LoadedImage{};
        UEFI::SimpleFileSystemProtocol* m_FileSystem{};
        UEFI::FileProtocol* m_RootDirectory{};
        UEFI::FileProtocol* m_KernelFile{};
        UEFI::Handle m_BootDeviceHandle{};

        void* m_FileInformationBuffer{};
        void* m_KernelImage{};
        UEFI::UintN m_KernelImageSize{};
        UEFI::Uint64 m_EntryPoint{};
        UEFI::UintN m_LoadSegmentCount{};

        UEFI::PhysicalAddress m_LoadedKernelBase{};
        UEFI::Uint64 m_LoadedKernelSize{};
        UEFI::UintN m_LoadedKernelPageCount{};

        UEFI::PhysicalAddress m_KernelStackBase{};
        UEFI::PhysicalAddress m_KernelStackTop{};
        UEFI::Uint64 m_KernelStackSize{};
        UEFI::UintN m_KernelStackPageCount{};

        UEFI::PhysicalAddress m_BootEnvironmentBase{};
        UEFI::UintN m_BootEnvironmentPageCount{};
        BootEnvironment_V1* m_BootEnvironment{};

        UEFI::PhysicalAddress m_MemoryMapBase{};
        UEFI::UintN m_MemoryMapPageCount{};
        UEFI::UintN m_MemoryMapCapacity{};
        UEFI::UintN m_MemoryMapSize{};
        UEFI::UintN m_MemoryMapKey{};
        UEFI::UintN m_MemoryDescriptorSize{};
        UEFI::Uint32 m_MemoryDescriptorVersion{};

        bool m_BootServicesExited{};
        bool m_MemoryMapAllocated{};
        bool m_BootEnvironmentAllocated{};
        bool m_KernelStackAllocated{};
        bool m_KernelPagesAllocated{};
        bool m_LoadedImageProtocolOpen{};
        bool m_FileSystemProtocolOpen{};
    };
}

extern "C" Zos::Boot::UEFI::Status EfiMain(Zos::Boot::UEFI::Handle ImageHandle, Zos::Boot::UEFI::SystemTable* SystemTable) {
    if (SystemTable == nullptr)
        return Zos::Boot::UEFI::InvalidParameter;

    BootLoader loader(ImageHandle, *SystemTable);
    return loader.Run();
}