/* Minimal UEFI definitions (UEFI Specification 2.10), written for NESH.
 * Only the types and protocols actually used by the shell are declared. */
#ifndef NESH_EFI_H
#define NESH_EFI_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint8_t BOOLEAN;
typedef int64_t INTN;
typedef uint64_t UINTN;
typedef int8_t INT8;
typedef uint8_t UINT8;
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef int32_t INT32;
typedef uint32_t UINT32;
typedef int64_t INT64;
typedef uint64_t UINT64;
typedef uint16_t CHAR16;
typedef char CHAR8;
typedef UINTN EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;
typedef UINTN EFI_TPL;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

#define EFI_ERROR_BIT (1ULL << 63)
#define EFIERR(n) (EFI_ERROR_BIT | (n))
#define EFI_ERROR(s) (((INTN)(s)) < 0)

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR EFIERR(1)
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_BAD_BUFFER_SIZE EFIERR(4)
#define EFI_BUFFER_TOO_SMALL EFIERR(5)
#define EFI_NOT_READY EFIERR(6)
#define EFI_DEVICE_ERROR EFIERR(7)
#define EFI_WRITE_PROTECTED EFIERR(8)
#define EFI_OUT_OF_RESOURCES EFIERR(9)
#define EFI_VOLUME_CORRUPTED EFIERR(10)
#define EFI_VOLUME_FULL EFIERR(11)
#define EFI_NO_MEDIA EFIERR(12)
#define EFI_MEDIA_CHANGED EFIERR(13)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ACCESS_DENIED EFIERR(15)
#define EFI_NO_RESPONSE EFIERR(16)
#define EFI_NO_MAPPING EFIERR(17)
#define EFI_TIMEOUT EFIERR(18)
#define EFI_NOT_STARTED EFIERR(19)
#define EFI_ALREADY_STARTED EFIERR(20)
#define EFI_ABORTED EFIERR(21)
#define EFI_SECURITY_VIOLATION EFIERR(26)
#define EFI_CRC_ERROR EFIERR(27)
#define EFI_END_OF_MEDIA EFIERR(28)
#define EFI_END_OF_FILE EFIERR(31)
#define EFI_WARN_DELETE_FAILURE 2

#define TRUE 1
#define FALSE 0

/* ---- Table header, time ---- */

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT16 Year;
    UINT8 Month;
    UINT8 Day;
    UINT8 Hour;
    UINT8 Minute;
    UINT8 Second;
    UINT8 Pad1;
    UINT32 Nanosecond;
    INT16 TimeZone;
    UINT8 Daylight;
    UINT8 Pad2;
} EFI_TIME;

typedef struct {
    UINT32 Resolution;
    UINT32 Accuracy;
    BOOLEAN SetsToZero;
} EFI_TIME_CAPABILITIES;

/* ---- Memory ---- */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- Device path ---- */

typedef struct {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define HARDWARE_DEVICE_PATH 0x01
#define ACPI_DEVICE_PATH 0x02
#define MESSAGING_DEVICE_PATH 0x03
#define MEDIA_DEVICE_PATH 0x04
#define BBS_DEVICE_PATH 0x05
#define END_DEVICE_PATH_TYPE 0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xff
#define END_INSTANCE_DEVICE_PATH_SUBTYPE 0x01

#define MEDIA_HARDDRIVE_DP 0x01
#define MEDIA_CDROM_DP 0x02
#define MEDIA_FILEPATH_DP 0x04
#define MEDIA_PIWG_FW_FILE_DP 0x06
#define MEDIA_PIWG_FW_VOL_DP 0x07

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    UINT32 PartitionNumber;
    UINT64 PartitionStart;
    UINT64 PartitionSize;
    UINT8 Signature[16];
    UINT8 MBRType;
    UINT8 SignatureType;
} __attribute__((packed)) HARDDRIVE_DEVICE_PATH;

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;
    CHAR16 PathName[];
} FILEPATH_DEVICE_PATH;

typedef struct {
    CHAR16 *(EFIAPI *ConvertDeviceNodeToText)(const EFI_DEVICE_PATH_PROTOCOL *Node,
                                              BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);
    CHAR16 *(EFIAPI *ConvertDevicePathToText)(const EFI_DEVICE_PATH_PROTOCOL *Path,
                                              BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL *(EFIAPI *ConvertTextToDeviceNode)(const CHAR16 *Text);
    EFI_DEVICE_PATH_PROTOCOL *(EFIAPI *ConvertTextToDevicePath)(const CHAR16 *Text);
} EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL;

/* ---- Console ---- */

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_NULL 0x00
#define SCAN_UP 0x01
#define SCAN_DOWN 0x02
#define SCAN_RIGHT 0x03
#define SCAN_LEFT 0x04
#define SCAN_HOME 0x05
#define SCAN_END 0x06
#define SCAN_INSERT 0x07
#define SCAN_DELETE 0x08
#define SCAN_PAGE_UP 0x09
#define SCAN_PAGE_DOWN 0x0A
#define SCAN_F1 0x0B
#define SCAN_F10 0x14
#define SCAN_ESC 0x17

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS(EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    UINT32 KeyShiftState;
    UINT8 KeyToggleState;
} EFI_KEY_STATE;

typedef struct {
    EFI_INPUT_KEY Key;
    EFI_KEY_STATE KeyState;
} EFI_KEY_DATA;

#define EFI_SHIFT_STATE_VALID 0x80000000
#define EFI_RIGHT_CONTROL_PRESSED 0x00000004
#define EFI_LEFT_CONTROL_PRESSED 0x00000008
#define EFI_RIGHT_ALT_PRESSED 0x00000010
#define EFI_LEFT_ALT_PRESSED 0x00000020

typedef struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS(EFIAPI *ReadKeyStrokeEx)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *Key);
    EFI_EVENT WaitForKeyEx;
    void *SetState;
    void *RegisterKeyNotify;
    void *UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

typedef struct {
    INT32 MaxMode;
    INT32 Mode;
    INT32 Attribute;
    INT32 CursorColumn;
    INT32 CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Extended);
    EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    EFI_STATUS(EFIAPI *TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    EFI_STATUS(EFIAPI *QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
                                  UINTN *Columns, UINTN *Rows);
    EFI_STATUS(EFIAPI *SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
    EFI_STATUS(EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS(EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS(EFIAPI *SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column,
                                          UINTN Row);
    EFI_STATUS(EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Services ---- */

typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific } EFI_RESET_TYPE;

typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;

#define EVT_TIMER 0x80000000
#define EVT_NOTIFY_WAIT 0x00000100
#define EVT_NOTIFY_SIGNAL 0x00000200
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;

#define TPL_APPLICATION 4
#define TPL_CALLBACK 8
#define TPL_NOTIFY 16

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x01
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x02
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL 0x04
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER 0x08
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x10
#define EFI_OPEN_PROTOCOL_EXCLUSIVE 0x20

typedef enum { EFI_NATIVE_INTERFACE } EFI_INTERFACE_TYPE;

typedef struct {
    EFI_HANDLE AgentHandle;
    EFI_HANDLE ControllerHandle;
    UINT32 Attributes;
    UINT32 OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL(EFIAPI *RaiseTPL)(EFI_TPL NewTpl);
    void(EFIAPI *RestoreTPL)(EFI_TPL OldTpl);
    EFI_STATUS(EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
                                      EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS(EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS(EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
                                     UINTN *DescriptorSize, UINT32 *DescriptorVersion);
    EFI_STATUS(EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS(EFIAPI *FreePool)(void *Buffer);
    EFI_STATUS(EFIAPI *CreateEvent)(UINT32 Type, EFI_TPL NotifyTpl, void *NotifyFunction, void *NotifyContext,
                                    EFI_EVENT *Event);
    EFI_STATUS(EFIAPI *SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime);
    EFI_STATUS(EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    EFI_STATUS(EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS(EFIAPI *CloseEvent)(EFI_EVENT Event);
    EFI_STATUS(EFIAPI *CheckEvent)(EFI_EVENT Event);
    EFI_STATUS(EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle, EFI_GUID *Protocol,
                                                 EFI_INTERFACE_TYPE InterfaceType, void *Interface);
    EFI_STATUS(EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol, void *OldInterface,
                                                   void *NewInterface);
    EFI_STATUS(EFIAPI *UninstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol, void *Interface);
    EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    EFI_STATUS(EFIAPI *RegisterProtocolNotify)(EFI_GUID *Protocol, EFI_EVENT Event, void **Registration);
    EFI_STATUS(EFIAPI *LocateHandle)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol, void *SearchKey,
                                     UINTN *BufferSize, EFI_HANDLE *Buffer);
    EFI_STATUS(EFIAPI *LocateDevicePath)(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath,
                                         EFI_HANDLE *Device);
    EFI_STATUS(EFIAPI *InstallConfigurationTable)(EFI_GUID *Guid, void *Table);
    EFI_STATUS(EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
                                  EFI_DEVICE_PATH_PROTOCOL *DevicePath, void *SourceBuffer, UINTN SourceSize,
                                  EFI_HANDLE *ImageHandle);
    EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
    EFI_STATUS(EFIAPI *Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize,
                             CHAR16 *ExitData);
    EFI_STATUS(EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    EFI_STATUS(EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    EFI_STATUS(EFIAPI *GetNextMonotonicCount)(UINT64 *Count);
    EFI_STATUS(EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS(EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize,
                                         CHAR16 *WatchdogData);
    EFI_STATUS(EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
                                          EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, BOOLEAN Recursive);
    EFI_STATUS(EFIAPI *DisconnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle,
                                             EFI_HANDLE ChildHandle);
    EFI_STATUS(EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface,
                                     EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
    EFI_STATUS(EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, EFI_HANDLE AgentHandle,
                                      EFI_HANDLE ControllerHandle);
    EFI_STATUS(EFIAPI *OpenProtocolInformation)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                                EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer,
                                                UINTN *EntryCount);
    EFI_STATUS(EFIAPI *ProtocolsPerHandle)(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer,
                                           UINTN *ProtocolBufferCount);
    EFI_STATUS(EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol,
                                           void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS(EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    EFI_STATUS(EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle, ...);
    EFI_STATUS(EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle, ...);
    EFI_STATUS(EFIAPI *CalculateCrc32)(void *Data, UINTN DataSize, UINT32 *Crc32);
    void(EFIAPI *CopyMem)(void *Destination, void *Source, UINTN Length);
    void(EFIAPI *SetMem)(void *Buffer, UINTN Size, UINT8 Value);
    EFI_STATUS(EFIAPI *CreateEventEx)(UINT32 Type, EFI_TPL NotifyTpl, void *NotifyFunction,
                                      const void *NotifyContext, const EFI_GUID *EventGroup, EFI_EVENT *Event);
} EFI_BOOT_SERVICES;

#define EFI_VARIABLE_NON_VOLATILE 0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS 0x00000004
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD 0x00000008
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS 0x00000010
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020
#define EFI_VARIABLE_APPEND_WRITE 0x00000040

#define EFI_OS_INDICATIONS_BOOT_TO_FW_UI 0x0000000000000001ULL

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS(EFIAPI *GetTime)(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Capabilities);
    EFI_STATUS(EFIAPI *SetTime)(EFI_TIME *Time);
    EFI_STATUS(EFIAPI *GetWakeupTime)(BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time);
    EFI_STATUS(EFIAPI *SetWakeupTime)(BOOLEAN Enable, EFI_TIME *Time);
    EFI_STATUS(EFIAPI *SetVirtualAddressMap)(UINTN MemoryMapSize, UINTN DescriptorSize,
                                             UINT32 DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap);
    EFI_STATUS(EFIAPI *ConvertPointer)(UINTN DebugDisposition, void **Address);
    EFI_STATUS(EFIAPI *GetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes,
                                    UINTN *DataSize, void *Data);
    EFI_STATUS(EFIAPI *GetNextVariableName)(UINTN *VariableNameSize, CHAR16 *VariableName,
                                            EFI_GUID *VendorGuid);
    EFI_STATUS(EFIAPI *SetVariable)(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 Attributes,
                                    UINTN DataSize, void *Data);
    EFI_STATUS(EFIAPI *GetNextHighMonotonicCount)(UINT32 *HighCount);
    void(EFIAPI *ResetSystem)(EFI_RESET_TYPE ResetType, EFI_STATUS ResetStatus, UINTN DataSize, void *ResetData);
    void *UpdateCapsule;
    void *QueryCapsuleCapabilities;
    EFI_STATUS(EFIAPI *QueryVariableInfo)(UINT32 Attributes, UINT64 *MaximumVariableStorageSize,
                                          UINT64 *RemainingVariableStorageSize, UINT64 *MaximumVariableSize);
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Loaded image ---- */

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS(EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- File system ---- */

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

#define EFI_FILE_READ_ONLY 0x01
#define EFI_FILE_HIDDEN 0x02
#define EFI_FILE_SYSTEM 0x04
#define EFI_FILE_RESERVED 0x08
#define EFI_FILE_DIRECTORY 0x10
#define EFI_FILE_ARCHIVE 0x20
#define EFI_FILE_VALID_ATTR 0x37

typedef struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS(EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **NewHandle,
                             CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS(EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    EFI_STATUS(EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *This);
    EFI_STATUS(EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS(EFIAPI *Write)(struct EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS(EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS(EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, UINT64 Position);
    EFI_STATUS(EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize,
                                void *Buffer);
    EFI_STATUS(EFIAPI *SetInfo)(struct EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize,
                                void *Buffer);
    EFI_STATUS(EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[];
} EFI_FILE_INFO;

typedef struct {
    UINT64 Size;
    BOOLEAN ReadOnly;
    UINT64 VolumeSize;
    UINT64 FreeSpace;
    UINT32 BlockSize;
    CHAR16 VolumeLabel[];
} EFI_FILE_SYSTEM_INFO;

/* ---- Block I/O ---- */

typedef struct {
    UINT32 MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    void *Reset;
    EFI_STATUS(EFIAPI *ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Lba,
                                   UINTN BufferSize, void *Buffer);
    void *WriteBlocks;
    void *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

/* ---- Shell parameters (for compatibility with UEFI Shell applications) ---- */

typedef void *SHELL_FILE_HANDLE;

typedef struct {
    CHAR16 **Argv;
    UINTN Argc;
    SHELL_FILE_HANDLE StdIn;
    SHELL_FILE_HANDLE StdOut;
    SHELL_FILE_HANDLE StdErr;
} EFI_SHELL_PARAMETERS_PROTOCOL;

/* ---- GUIDs ---- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID \
    { 0xbc62157e, 0x3e33, 0x4fec, { 0x99, 0x20, 0x2d, 0x3b, 0x36, 0xd7, 0x50, 0xdf } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_FILE_INFO_GUID \
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_FILE_SYSTEM_INFO_GUID \
    { 0x09576E93, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_FILE_SYSTEM_VOLUME_LABEL_GUID \
    { 0xDB47D7D3, 0xFE81, 0x11D3, { 0x9A, 0x35, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576E91, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID \
    { 0x8B843E20, 0x8132, 0x4852, { 0x90, 0xCC, 0x55, 0x1A, 0x4E, 0x4A, 0x7F, 0x1C } }
#define EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID \
    { 0x05C99A21, 0xC70F, 0x4AD2, { 0x8A, 0x5F, 0x35, 0xDF, 0x33, 0x43, 0xF5, 0x1E } }
#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID \
    { 0xDD9E7534, 0x7762, 0x4698, { 0x8C, 0x14, 0xF5, 0x85, 0x17, 0x3F, 0x5A, 0x7A } }
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964E5B21, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SHELL_PARAMETERS_PROTOCOL_GUID \
    { 0x752F3136, 0x4E16, 0x4FDC, { 0xA2, 0x2A, 0xE5, 0xF4, 0x68, 0x12, 0xF4, 0xCA } }
#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } }
#define EFI_IMAGE_SECURITY_DATABASE_GUID \
    { 0xD719B2CB, 0x3D3A, 0x4596, { 0xA3, 0xBC, 0xDA, 0xD0, 0x0E, 0x67, 0x65, 0x6F } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define EFI_SMBIOS_TABLE_GUID \
    { 0xEB9D2D31, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define EFI_SMBIOS3_TABLE_GUID \
    { 0xF2FD1544, 0x9794, 0x4A2C, { 0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94 } }

#endif
