// SPDX-License-Identifier: GPL-2.0-only
// Minimal UEFI definitions (UEFI 2.x spec), just what the loader needs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void* EFI_HANDLE;
typedef void* EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

constexpr EFI_STATUS EFI_SUCCESS = 0;
constexpr EFI_STATUS EFI_ERR = 0x8000000000000000ull;
constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL = EFI_ERR | 5;
constexpr EFI_STATUS EFI_NOT_READY = EFI_ERR | 6;
constexpr EFI_STATUS EFI_NOT_FOUND = EFI_ERR | 14;

struct EFI_GUID {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};

struct EFI_TABLE_HEADER {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
};

struct EFI_INPUT_KEY {
    uint16_t ScanCode;
    CHAR16 UnicodeChar;
};

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS(EFIAPI* Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, bool);
    EFI_STATUS(EFIAPI* ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
    EFI_EVENT WaitForKey;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI* Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, bool);
    EFI_STATUS(EFIAPI* OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const CHAR16*);
    void* TestString;
    void* QueryMode;
    void* SetMode;
    EFI_STATUS(EFIAPI* SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, uint64_t);
    EFI_STATUS(EFIAPI* ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
    EFI_STATUS(EFIAPI* SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, uint64_t, uint64_t);
    EFI_STATUS(EFIAPI* EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, bool);
    void* Mode;
};

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
};

enum EFI_MEMORY_TYPE : uint32_t {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
    EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace, EfiPalCode,
    EfiPersistentMemory, EfiUnacceptedMemory,
};
// OS-loader private memory types (0x80000000+ is reserved for OS loaders).
constexpr uint32_t MEM_KERNEL = 0x80000001;      // kernel image + modules
constexpr uint32_t MEM_RECLAIMABLE = 0x80000002; // page tables, responses, stack

enum EFI_ALLOCATE_TYPE : uint32_t { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL;
    void* RestoreTPL;
    EFI_STATUS(EFIAPI* AllocatePages)(EFI_ALLOCATE_TYPE, uint32_t, uint64_t, EFI_PHYSICAL_ADDRESS*);
    EFI_STATUS(EFIAPI* FreePages)(EFI_PHYSICAL_ADDRESS, uint64_t);
    EFI_STATUS(EFIAPI* GetMemoryMap)(uint64_t*, EFI_MEMORY_DESCRIPTOR*, uint64_t*, uint64_t*, uint32_t*);
    EFI_STATUS(EFIAPI* AllocatePool)(uint32_t, uint64_t, void**);
    EFI_STATUS(EFIAPI* FreePool)(void*);
    void* CreateEvent;
    void* SetTimer;
    void* WaitForEvent;
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS(EFIAPI* HandleProtocol)(EFI_HANDLE, const EFI_GUID*, void**);
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;
    EFI_STATUS(EFIAPI* LoadImage)(bool, EFI_HANDLE, void*, void*, uint64_t, EFI_HANDLE*);
    EFI_STATUS(EFIAPI* StartImage)(EFI_HANDLE, uint64_t*, CHAR16**);
    void* Exit;
    void* UnloadImage;
    EFI_STATUS(EFIAPI* ExitBootServices)(EFI_HANDLE, uint64_t);
    void* GetNextMonotonicCount;
    EFI_STATUS(EFIAPI* Stall)(uint64_t);
    EFI_STATUS(EFIAPI* SetWatchdogTimer)(uint64_t, uint64_t, uint64_t, CHAR16*);
    void* ConnectController;
    void* DisconnectController;
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;
    void* ProtocolsPerHandle;
    EFI_STATUS(EFIAPI* LocateHandleBuffer)(uint32_t, const EFI_GUID*, void*, uint64_t*, EFI_HANDLE**);
    EFI_STATUS(EFIAPI* LocateProtocol)(const EFI_GUID*, void*, void**);
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;
    void* CalculateCrc32;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

struct EFI_CONFIGURATION_TABLE {
    EFI_GUID VendorGuid;
    void* VendorTable;
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16* FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;
};

struct EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    uint32_t LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType, ImageDataType;
    void* Unload;
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    uint32_t Version;
    uint32_t HorizontalResolution, VerticalResolution;
    uint32_t PixelFormat;  // 0 RGBX, 1 BGRX, 2 bitmask, 3 blt only
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
    uint32_t PixelsPerScanLine;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    uint32_t MaxMode, Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    uint64_t SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    uint64_t FrameBufferSize;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI* QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t, uint64_t*, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**);
    EFI_STATUS(EFIAPI* SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t);
    void* Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
};

struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS(EFIAPI* Open)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, const CHAR16*, uint64_t, uint64_t);
    EFI_STATUS(EFIAPI* Close)(EFI_FILE_PROTOCOL*);
    void* Delete;
    EFI_STATUS(EFIAPI* Read)(EFI_FILE_PROTOCOL*, uint64_t*, void*);
    void* Write;
    void* GetPosition;
    void* SetPosition;
    EFI_STATUS(EFIAPI* GetInfo)(EFI_FILE_PROTOCOL*, const EFI_GUID*, uint64_t*, void*);
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS(EFIAPI* OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
};

struct EFI_TIME {
    uint16_t Year;
    uint8_t Month, Day, Hour, Minute, Second, Pad1;
    uint32_t Nanosecond;
    int16_t TimeZone;
    uint8_t Daylight, Pad2;
};

struct EFI_FILE_INFO {
    uint64_t Size, FileSize, PhysicalSize;
    EFI_TIME CreateTime, LastAccessTime, ModificationTime;
    uint64_t Attribute;
    CHAR16 FileName[1];
};

inline constexpr EFI_GUID LOADED_IMAGE_GUID = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID SIMPLE_FS_GUID = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID FILE_INFO_GUID = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID GOP_GUID = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
inline constexpr EFI_GUID ACPI20_GUID = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
inline constexpr EFI_GUID ACPI10_GUID = {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
inline constexpr EFI_GUID SMBIOS3_GUID = {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};
inline constexpr EFI_GUID SMBIOS_GUID = {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

constexpr uint64_t EFI_FILE_MODE_READ = 1;
constexpr uint32_t ByProtocol = 2;
