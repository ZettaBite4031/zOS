#pragma once

namespace Zos::Boot::UEFI {
    using Boolean = unsigned char;
    using Char16 = char16_t;
    using Handle = void*;
    using Int16 = short;
    using Status = unsigned long long;
    using Uint8 = unsigned char;
    using Uint16 = unsigned short;
    using Uint32 = unsigned int;
    using Uint64 = unsigned long long;
    using UintN = unsigned long long;

    static_assert(sizeof(Boolean) == 1);
    static_assert(sizeof(Char16) == 2);
    static_assert(sizeof(Int16) == 2);
    static_assert(sizeof(Uint16) == 2);
    static_assert(sizeof(Uint32) == 4);
    static_assert(sizeof(Uint64) == 8);
    static_assert(sizeof(UintN) == 8);
    static_assert(sizeof(void*) == 8);

    inline constexpr Status ErrorBit{ Uint64{ 1 } << 63 };

    constexpr Status MakeError(UintN code) noexcept {
        return ErrorBit | code;
    }

    inline constexpr Status Success{ 0 };
    inline constexpr Status LoadError{ MakeError(1) };
    inline constexpr Status InvalidParameter{ MakeError(2) };
    inline constexpr Status Unsupported{ MakeError(3) };
    inline constexpr Status BufferTooSmall{ MakeError(5) };
    inline constexpr Status DeviceError{ MakeError(7) };
    inline constexpr Status OutOfResources{ MakeError(9) };
    inline constexpr Status VolumeCorrupted{ MakeError(10) };
    inline constexpr Status NotFound{ MakeError(14) };

    constexpr bool IsError(Status status) noexcept {
        return (status & ErrorBit) != 0;
    }

    inline constexpr Uint32 OpenProtocolGetProtocol{ 0x00000002 };
    inline constexpr Uint64 FileModeRead{ 0x0000000000000001ULL };

    struct Guid {
        Uint32 Data1;
        Uint16 Data2;
        Uint16 Data3;
        Uint8 Data4[8];
    };

    struct Time {
        Uint16 Year;
        Uint8 Month;
        Uint8 Day;
        Uint8 Hour;
        Uint8 Minute;
        Uint8 Second;
        Uint8 _pad1;
        Uint32 Nanosecond;
        Int16 Timezone;
        Uint8 Daylight;
        Uint8 _pad2;
    };

    enum class MemoryType : Uint32 {
      Reserved,
      LoaderCode,
      LoaderData,
      BootServicesCode,
      BootServicesData,
      RuntimeServicesCode,
      RuntimeServicesData,
      ConventionalMemory,
      UnusableMemory,
      AcpiReclaimMemory,
      AcpiMemoryNvs,
      MemoryMappedIo,
      MemoryMappedIoPortSpace,
      PalCode,
      PersistentMemory,
      UnacceptedMemory,
      MaximumMemoryType,  
    };

    struct TableHeader {
        Uint64 Signature;
        Uint32 Revision;
        Uint32 HeaderSize;
        Uint32 CRC32;
        Uint32 Reserved;
    };

    struct SystemTable;
    struct SimpleTextOutputProtocol;
    struct SimpleFileSystemProtocol;
    struct FileProtocol;

    using fpTextReset = Status(*)(SimpleTextOutputProtocol* self, Boolean extended_verification);
    using fpTextOutputString = Status(*)(SimpleTextOutputProtocol* self, Char16* string);

    struct SimpleTextOutputProtocol {
        fpTextReset Reset;
        fpTextOutputString OutputString;
        void* TestString;
        void* QueryMode;
        void* SetMode;
        void* SetAttribute;
        void* ClearScreen;
        void* SetCursorPosition;
        void* EnableCursor;
        void* Mode;
    };

    using ImageUnload = Status(*)(Handle image_handle);

    struct LoadedImageProtocol {
        Uint32 Revision;
        Handle ParentHandle;
        SystemTable* pSystemTable;
        Handle DeviceHandle;
        void* FilePath;
        void* Reserved;
        Uint32 LoadOptionsSize;
        void* LoadOptions;
        void* ImageBase;
        Uint64 ImageSize;
        MemoryType ImageCodeType;
        MemoryType ImageDataType;
        ImageUnload Unload;
    };

    using fpFileOpen = Status(*)(FileProtocol* self, FileProtocol** new_handle, Char16* file_name, Uint64 open_mode, Uint64 attributes);
    using fpFileClose = Status(*)(FileProtocol* self);
    using fpFileRead = Status(*)(FileProtocol* self, UintN* buffer_size, void* buffer);
    using fpFileGetInfo = Status(*)(FileProtocol* self, const Guid* info_type, UintN* buffer_size, void* buffer);

    struct FileProtocol {
        Uint64 Revision;
        fpFileOpen Open;
        fpFileClose Close;
        void* Delete;
        fpFileRead Read;
        void* Write;
        void* GetPosition;
        void* SetPosition;
        fpFileGetInfo GetInfo;
        void* SetInfo;
        void* Flush;
        void* OpenEx;
        void* ReadEx;
        void* WriteEx;
        void* FlushEx;
    };

    using fpOpenVolume = Status(*)(SimpleFileSystemProtocol* self, FileProtocol** root);

    struct SimpleFileSystemProtocol {
        Uint64 Revision;
        fpOpenVolume OpenVolume;
    };

    struct FileInfo {
        Uint64 Size;
        Uint64 FileSize;
        Uint64 PhysicalSize;
        Time CreateTime;
        Time LastAccessTime;
        Time ModificationTime;
        Uint64 Attribute;
        Char16 FileName[1];
    };

    using fpAllocatePool = Status(*)(MemoryType pool_type, UintN size, void** buffer);
    using fpFreePool = Status(*)(void* buffer);
    using fpOpenProtocol = Status(*)(Handle handle, const Guid* protocol, void** interface, Handle agent_handle, Handle controller_handle, Uint32 attributes);
    using fpCloseProtocol = Status(*)(Handle handle, const Guid* protocol, Handle agent_handle, Handle controller_handle);

    struct BootServices {
        TableHeader Header;
        void* RaiseTpl;
        void* RestoreTpl;
        void* AllocatePages;
        void* FreePages;
        void* GetMemoryMap;
        fpAllocatePool AllocatePool;
        fpFreePool FreePool;
        void* CreateEvent;
        void* SetTimer;
        void* WaitForEvent;
        void* SignalEvent;
        void* CloseEvent;
        void* CheckEvent;
        void* InstallProtocolInterface;
        void* ReinstallProtocolInterface;
        void* UninstallProtocolInterface;
        void* HandleProtocol;
        void* Reserved;
        void* RegisterProtocolNotify;
        void* LocateHandle;
        void* LocateDevicePath;
        void* InstallConfigurationTable;
        void* LoadImage;
        void* StartImage;
        void* Exit;
        void* UnloadImage;
        void* ExitBootServices;
        void* GetNextMonotonicCount;
        void* Stall;
        void* SetWatchdogTimer;
        void* ConnectController;
        void* DisconnectController;
        fpOpenProtocol OpenProtocol;
        fpCloseProtocol CloseProtocol;
        void* OpenProtocolInformation;
        void* ProtocolsPerHandle;
        void* LocateHandleBuffer;
        void* LocateProtocol;
        void* InstallMultipleProtocolInterfaces;
        void* UninstallMultipleProtocolInterfaces;
        void* CalculateCrc32;
        void* CopyMem;
        void* SetMem;
        void* CreateEventEx;
    };

    struct SystemTable {
        TableHeader header;
        Char16* FirmwareVendor;
        Uint32 FirmwareRevision;
        Handle ConsoleInputHandle;
        void* ConsoleInput;
        Handle ConsoleOutputHandle;
        SimpleTextOutputProtocol* ConsoleOutput;
        Handle StandardErrorHandle;
        SimpleTextOutputProtocol* StandardError;
        void* RuntimeServices;
        BootServices* pBootServices;
        UintN COnfigurationTableCount;
        void* ConfigurationTables;
    };

    inline constexpr Guid LoadedImageProtocolGuid{
        0x5B1B31A1,
        0x9562,
        0x11D2,
        { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B },
    };

    inline constexpr Guid SimpleFileSystemProtocolGuid{
        0x964E5B22,
        0x6459,
        0x11D2,
        { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B },
    };

    inline constexpr Guid FileInfoGuid{
        0x09576E92,
        0x6D3F,
        0x11D2,
        { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B },
    };

    static_assert(sizeof(Guid) == 16);
    static_assert(sizeof(Time) == 16);
    static_assert(sizeof(TableHeader) == 24);
    static_assert(sizeof(SimpleTextOutputProtocol) == 80);
    static_assert(sizeof(LoadedImageProtocol) == 96);
    static_assert(sizeof(FileProtocol) == 120);
    static_assert(sizeof(SimpleFileSystemProtocol) == 16);
    static_assert(__builtin_offsetof(FileInfo, FileName) == 80);
    static_assert(sizeof(BootServices) == 376);
    static_assert(__builtin_offsetof(BootServices, AllocatePool) == 64);
    static_assert(__builtin_offsetof(BootServices, OpenProtocol) == 280);
    static_assert(sizeof(SystemTable) == 120);
    static_assert(__builtin_offsetof(SystemTable, ConsoleOutput) == 64);
    static_assert(__builtin_offsetof(SystemTable, pBootServices) == 96);
}