/* Minimal UEFI definitions for the NebOS x86-64 loader.
 *
 * Only the tables and protocols the loader uses are declared, but the fields
 * before them are in their exact UEFI order so the offsets match the
 * firmware's structures.  x86_64-w64-mingw32 uses the Microsoft x64 calling
 * convention, which is the UEFI ABI, so no ms_abi attributes are needed.
 */
#ifndef NEBOS_EFI_H
#define NEBOS_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef UINT64             EFI_VIRTUAL_ADDRESS;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;

#define EFI_SUCCESS 0

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} __attribute__((packed)) EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
	UINT32 Type;
	UINT32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS  VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} __attribute__((packed)) EFI_MEMORY_DESCRIPTOR;

typedef struct {
	EFI_GUID VendorGuid;
	void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
	void *Reset;
	EFI_STATUS (*OutputString)(void *This, CHAR16 *String);
	void *TestString;
	void *QueryMode;
	void *SetMode;
	void *SetAttribute;
	void *ClearScreen;
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Field order matches EFI_BOOT_SERVICES; only the used entries have typed
 * signatures, the rest are void * but must still be present in order. */
typedef struct {
	EFI_TABLE_HEADER Hdr;
	void *RaiseTPL;
	void *RestoreTPL;
	EFI_STATUS (*AllocatePages)(UINT32 Type, UINT32 MemoryType,
				    UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
	void *FreePages;
	EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize,
				   EFI_MEMORY_DESCRIPTOR *MemoryMap,
				   UINTN *MapKey, UINTN *DescriptorSize,
				   UINT32 *DescriptorVersion);
	void *AllocatePool;
	void *FreePool;
	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;
	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	void *HandleProtocol;
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;
	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
	void *GetNextMonotonicCount;
	void *Stall;
	EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
				       UINTN DataSize, CHAR16 *WatchdogData);
	void *ConnectController;
	void *DisconnectController;
	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;
	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration,
				     void **Interface);
} EFI_BOOT_SERVICES;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	UINT32 Pad;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
	UINT32 RedMask;
	UINT32 GreenMask;
	UINT32 BlueMask;
	UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	UINT32 PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
	EFI_STATUS (*QueryMode)(void *This, UINT32 ModeNumber,
				UINTN *SizeOfInfo,
				EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
	EFI_STATUS (*SetMode)(void *This, UINT32 ModeNumber);
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#define EFI_PIXEL_RGBX     0
#define EFI_PIXEL_BGRX     1
#define EFI_PIXEL_BITMASK  2
#define EFI_PIXEL_BLTONLY  3

#define EFI_ALLOCATE_ANY_PAGES    0
#define EFI_ALLOCATE_MAX_ADDRESS  1
#define EFI_ALLOCATE_ADDRESS      2

#define EFI_LOADER_DATA            2
#define EFI_BOOT_SERVICES_CODE     3
#define EFI_BOOT_SERVICES_DATA     4
#define EFI_CONVENTIONAL_MEMORY    7

static const EFI_GUID efi_gop_guid = {
	0x9042A9DEu, 0x23DC, 0x4A38,
	{ 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A }
};
static const EFI_GUID efi_acpi20_guid = {
	0x8868E871u, 0xE4F1, 0x11D3,
	{ 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }
};
static const EFI_GUID efi_acpi10_guid = {
	0xEB9D2D30u, 0x2D88, 0x11D3,
	{ 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
};

#endif
