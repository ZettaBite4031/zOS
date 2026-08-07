#include <Boot/UEFI/UEFI.hpp>
#include <Boot/UEFI/ELF.hpp>

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
            
            if (m_BootServices->OpenProtocol == nullptr
            || m_BootServices->CloseProtocol == nullptr
            || m_BootServices->AllocatePool == nullptr
            || m_BootServices->FreePool == nullptr) 
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

            return UEFI::Success;
        }

    private:
        static bool RangeIsInside(UEFI::Uint64 offset, UEFI::Uint64 length, UEFI::Uint64 total) noexcept {
            return offset <= total && length <= total - offset;
        }

        static bool IsPowerOfTwo(UEFI::Uint64 value) noexcept {
            return value != 0 && (value & (value - 1)) == 0;
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