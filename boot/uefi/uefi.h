#ifndef _EMBLINK_UEFI_H_
#define _EMBLINK_UEFI_H_

/* A DELIBERATELY MINIMAL slice of the UEFI spec -- only the types and services
 * this loader actually calls, hand-written rather than vendored from GNU-EFI.
 * Same own-the-stack stance as the rest of the project (own libc, own exe
 * format, own compiler). If you add a Boot Services call, add its member in the
 * SAME position the spec lists it -- the struct is an ABI, offsets are load-
 * bearing, and the unused slots are `void *` precisely to keep them correct. */

#include <stdint.h>

/* UEFI calls use the Microsoft x64 ABI (args in RCX,RDX,R8,R9), not SysV. */
#define EFIAPI __attribute__((ms_abi))

typedef uint8_t   BOOLEAN;
typedef uint16_t  CHAR16;
typedef uint64_t  UINTN;      /* natural width; 64-bit here */
typedef int64_t   INTN;
typedef void     *EFI_HANDLE;
typedef UINTN     EFI_STATUS;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;

/* Status codes: the high bit marks an error. */
#define EFI_ERROR_BIT        (1ULL << 63)
#define EFI_SUCCESS          0ULL
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND        (EFI_ERROR_BIT | 14)
#define EFI_ERROR(s)         (((EFI_STATUS)(s)) & EFI_ERROR_BIT)

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

/* Graphics Output Protocol GUID {9042A9DE-23DC-4A38-96FB-7ADED080516A} */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* ACPI RSDP lives in the EFI configuration table under one of these GUIDs (2.0
 * preferred -- it points to the XSDT). The legacy BIOS memory scan the kernel
 * uses finds nothing under UEFI, so we hand the RSDP over via the boot protocol. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ---- Memory map ---------------------------------------------------------- */
typedef enum {
    EfiReservedMemoryType,   EfiLoaderCode,          EfiLoaderData,
    EfiBootServicesCode,     EfiBootServicesData,    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,  EfiConventionalMemory,  EfiUnusableMemory,
    EfiACPIReclaimMemory,    EfiACPIMemoryNVS,       EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace, EfiPalCode,          EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t              Type;         /* EFI_MEMORY_TYPE                  */
    uint32_t              Pad;
    EFI_PHYSICAL_ADDRESS  PhysicalStart;
    EFI_VIRTUAL_ADDRESS   VirtualStart;
    uint64_t              NumberOfPages; /* 4 KiB pages                     */
    uint64_t              Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;

/* ---- Simple Text Output (early milestone prints) ------------------------- */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void            *Reset;
    EFI_TEXT_STRING  OutputString;
    /* ... rest unused ... */
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Graphics Output Protocol ------------------------------------------- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,   /* RGB */
    PixelBlueGreenRedReserved8BitPerColor,   /* BGR */
    PixelBitMask, PixelBltOnly, PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t                  Version;
    uint32_t                  HorizontalResolution;
    uint32_t                  VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    struct { uint32_t r, g, b, rsvd; } PixelInformation;
    uint32_t                  PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                              MaxMode;
    uint32_t                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void                              *QueryMode;
    void                              *SetMode;
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- Boot Services (only the calls we use are typed; the rest are void* to
 *      hold their ABI slots). Order is the UEFI spec's, do not reorder. ---- */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
    UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, uint32_t *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, void *Registration, void **Interface);

typedef struct {
    EFI_TABLE_HEADER   Hdr;
    void              *RaiseTPL;
    void              *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    void              *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL  AllocatePool;
    void              *FreePool;
    void              *CreateEvent;
    void              *SetTimer;
    void              *WaitForEvent;
    void              *SignalEvent;
    void              *CloseEvent;
    void              *CheckEvent;
    void              *InstallProtocolInterface;
    void              *ReinstallProtocolInterface;
    void              *UninstallProtocolInterface;
    void              *HandleProtocol;
    void              *Reserved;
    void              *RegisterProtocolNotify;
    void              *LocateHandle;
    void              *LocateDevicePath;
    void              *InstallConfigurationTable;
    void              *LoadImage;
    void              *StartImage;
    void              *Exit;
    void              *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void              *GetNextMonotonicCount;
    void              *Stall;
    void              *SetWatchdogTimer;
    void              *ConnectController;
    void              *DisconnectController;
    void              *OpenProtocol;
    void              *CloseProtocol;
    void              *OpenProtocolInformation;
    void              *ProtocolsPerHandle;
    void              *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    /* ... remaining members unused ... */
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    void                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* _EMBLINK_UEFI_H_ */
