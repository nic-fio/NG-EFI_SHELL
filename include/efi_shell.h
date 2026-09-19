/* UEFI Shell 2.2 protocols (from the UEFI Shell Specification), plus the
 * few extra UEFI protocols the shell implementation needs. */
#ifndef NESH_EFI_SHELL_H
#define NESH_EFI_SHELL_H

#include "efi.h"

typedef struct LIST_ENTRY {
    struct LIST_ENTRY *ForwardLink;
    struct LIST_ENTRY *BackLink;
} LIST_ENTRY;

typedef struct {
    LIST_ENTRY Link;
    EFI_STATUS Status;
    const CHAR16 *FullName;
    const CHAR16 *FileName;
    SHELL_FILE_HANDLE Handle;
    EFI_FILE_INFO *Info;
} EFI_SHELL_FILE_INFO;

#define EFI_DEVICE_NAME_USE_COMPONENT_NAME 0x00000001
#define EFI_DEVICE_NAME_USE_DEVICE_PATH 0x00000002

typedef struct EFI_SHELL_PROTOCOL {
    EFI_STATUS(EFIAPI *Execute)(EFI_HANDLE *ParentImageHandle, CHAR16 *CommandLine, CHAR16 **Environment,
                                EFI_STATUS *StatusCode);
    const CHAR16 *(EFIAPI *GetEnv)(const CHAR16 *Name);
    EFI_STATUS(EFIAPI *SetEnv)(const CHAR16 *Name, const CHAR16 *Value, BOOLEAN Volatile);
    const CHAR16 *(EFIAPI *GetAlias)(const CHAR16 *Alias, BOOLEAN *Volatile);
    EFI_STATUS(EFIAPI *SetAlias)(const CHAR16 *Command, const CHAR16 *Alias, BOOLEAN Replace, BOOLEAN Volatile);
    EFI_STATUS(EFIAPI *GetHelpText)(const CHAR16 *Command, const CHAR16 *Sections, CHAR16 **HelpText);
    const EFI_DEVICE_PATH_PROTOCOL *(EFIAPI *GetDevicePathFromMap)(const CHAR16 *Mapping);
    const CHAR16 *(EFIAPI *GetMapFromDevicePath)(EFI_DEVICE_PATH_PROTOCOL **DevicePath);
    EFI_DEVICE_PATH_PROTOCOL *(EFIAPI *GetDevicePathFromFilePath)(const CHAR16 *Path);
    CHAR16 *(EFIAPI *GetFilePathFromDevicePath)(const EFI_DEVICE_PATH_PROTOCOL *Path);
    EFI_STATUS(EFIAPI *SetMap)(const EFI_DEVICE_PATH_PROTOCOL *DevicePath, const CHAR16 *Mapping);
    const CHAR16 *(EFIAPI *GetCurDir)(const CHAR16 *FileSystemMapping);
    EFI_STATUS(EFIAPI *SetCurDir)(const CHAR16 *FileSystem, const CHAR16 *Dir);
    EFI_STATUS(EFIAPI *OpenFileList)(CHAR16 *Path, UINT64 OpenMode, EFI_SHELL_FILE_INFO **FileList);
    EFI_STATUS(EFIAPI *FreeFileList)(EFI_SHELL_FILE_INFO **FileList);
    EFI_STATUS(EFIAPI *RemoveDupInFileList)(EFI_SHELL_FILE_INFO **FileList);
    BOOLEAN(EFIAPI *BatchIsActive)(void);
    BOOLEAN(EFIAPI *IsRootShell)(void);
    void(EFIAPI *EnablePageBreak)(void);
    void(EFIAPI *DisablePageBreak)(void);
    BOOLEAN(EFIAPI *GetPageBreak)(void);
    EFI_STATUS(EFIAPI *GetDeviceName)(EFI_HANDLE DeviceHandle, UINT32 Flags, CHAR8 *Language, CHAR16 **BestDeviceName);
    EFI_FILE_INFO *(EFIAPI *GetFileInfo)(SHELL_FILE_HANDLE FileHandle);
    EFI_STATUS(EFIAPI *SetFileInfo)(SHELL_FILE_HANDLE FileHandle, const EFI_FILE_INFO *FileInfo);
    EFI_STATUS(EFIAPI *OpenFileByName)(const CHAR16 *FileName, SHELL_FILE_HANDLE *FileHandle, UINT64 OpenMode);
    EFI_STATUS(EFIAPI *CloseFile)(SHELL_FILE_HANDLE FileHandle);
    EFI_STATUS(EFIAPI *CreateFile)(const CHAR16 *FileName, UINT64 FileAttribs, SHELL_FILE_HANDLE *FileHandle);
    EFI_STATUS(EFIAPI *ReadFile)(SHELL_FILE_HANDLE FileHandle, UINTN *ReadSize, void *Buffer);
    EFI_STATUS(EFIAPI *WriteFile)(SHELL_FILE_HANDLE FileHandle, UINTN *BufferSize, void *Buffer);
    EFI_STATUS(EFIAPI *DeleteFile)(SHELL_FILE_HANDLE FileHandle);
    EFI_STATUS(EFIAPI *DeleteFileByName)(const CHAR16 *FileName);
    EFI_STATUS(EFIAPI *GetFilePosition)(SHELL_FILE_HANDLE FileHandle, UINT64 *Position);
    EFI_STATUS(EFIAPI *SetFilePosition)(SHELL_FILE_HANDLE FileHandle, UINT64 Position);
    EFI_STATUS(EFIAPI *FlushFile)(SHELL_FILE_HANDLE FileHandle);
    EFI_STATUS(EFIAPI *FindFiles)(const CHAR16 *FilePattern, EFI_SHELL_FILE_INFO **FileList);
    EFI_STATUS(EFIAPI *FindFilesInDir)(SHELL_FILE_HANDLE FileDirHandle, EFI_SHELL_FILE_INFO **FileList);
    EFI_STATUS(EFIAPI *GetFileSize)(SHELL_FILE_HANDLE FileHandle, UINT64 *Size);
    EFI_STATUS(EFIAPI *OpenRoot)(EFI_DEVICE_PATH_PROTOCOL *DevicePath, SHELL_FILE_HANDLE *FileHandle);
    EFI_STATUS(EFIAPI *OpenRootByHandle)(EFI_HANDLE DeviceHandle, SHELL_FILE_HANDLE *FileHandle);
    EFI_EVENT ExecutionBreak;
    UINT32 MajorVersion;
    UINT32 MinorVersion;
    /* UEFI Shell 2.1 */
    EFI_STATUS(EFIAPI *RegisterGuidName)(const EFI_GUID *Guid, const CHAR16 *GuidName);
    EFI_STATUS(EFIAPI *GetGuidName)(const EFI_GUID *Guid, const CHAR16 **GuidName);
    EFI_STATUS(EFIAPI *GetGuidFromName)(const CHAR16 *GuidName, EFI_GUID *Guid);
    const CHAR16 *(EFIAPI *GetEnvEx)(const CHAR16 *Name, UINT32 *Attributes);
} EFI_SHELL_PROTOCOL;

#define EFI_SHELL_PROTOCOL_GUID \
    { 0x6302d008, 0x7f9b, 0x4f30, { 0x87, 0xac, 0x60, 0xc9, 0xfe, 0xf5, 0xda, 0x4e } }

/* ---- Other protocols used by the shell ---- */


typedef struct EFI_COMPONENT_NAME2_PROTOCOL {
    EFI_STATUS(EFIAPI *GetDriverName)(struct EFI_COMPONENT_NAME2_PROTOCOL *This, CHAR8 *Language, CHAR16 **DriverName);
    EFI_STATUS(EFIAPI *GetControllerName)(struct EFI_COMPONENT_NAME2_PROTOCOL *This, EFI_HANDLE ControllerHandle,
                                          EFI_HANDLE ChildHandle, CHAR8 *Language, CHAR16 **ControllerName);
    CHAR8 *SupportedLanguages;
} EFI_COMPONENT_NAME2_PROTOCOL;

#define EFI_COMPONENT_NAME2_PROTOCOL_GUID \
    { 0x6a7a5cff, 0xe8d9, 0x4f70, { 0xba, 0xda, 0x75, 0xab, 0x30, 0x25, 0xce, 0x14 } }
#define EFI_COMPONENT_NAME_PROTOCOL_GUID \
    { 0x107a772c, 0xd5e1, 0x11d4, { 0x9a, 0x46, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

typedef struct {
    void *Supported;
    void *Start;
    void *Stop;
    UINT32 Version;
    EFI_HANDLE ImageHandle;
    EFI_HANDLE DriverBindingHandle;
} EFI_DRIVER_BINDING_PROTOCOL;

#define EFI_DRIVER_BINDING_PROTOCOL_GUID \
    { 0x18a031ab, 0xb443, 0x4d1a, { 0xa5, 0xc0, 0x0c, 0x09, 0x26, 0x1e, 0x9f, 0x71 } }

typedef enum {
    EfiDriverDiagnosticTypeStandard = 0,
    EfiDriverDiagnosticTypeExtended = 1,
    EfiDriverDiagnosticTypeManufacturing = 2,
    EfiDriverDiagnosticTypeCancel = 3,
} EFI_DRIVER_DIAGNOSTIC_TYPE;

typedef struct EFI_DRIVER_DIAGNOSTICS2_PROTOCOL {
    EFI_STATUS(EFIAPI *RunDiagnostics)(struct EFI_DRIVER_DIAGNOSTICS2_PROTOCOL *This, EFI_HANDLE ControllerHandle,
                                       EFI_HANDLE ChildHandle, EFI_DRIVER_DIAGNOSTIC_TYPE DiagnosticType,
                                       CHAR8 *Language, EFI_GUID **ErrorType, UINTN *BufferSize, CHAR16 **Buffer);
    CHAR8 *SupportedLanguages;
} EFI_DRIVER_DIAGNOSTICS2_PROTOCOL;

#define EFI_DRIVER_DIAGNOSTICS2_PROTOCOL_GUID \
    { 0x4d330321, 0x025f, 0x4aac, { 0x90, 0xd8, 0x5e, 0xd9, 0x00, 0x17, 0x3b, 0x63 } }

typedef enum {
    EfiDriverConfigurationActionNone = 0,
    EfiDriverConfigurationActionStopController = 1,
    EfiDriverConfigurationActionRestartController = 2,
    EfiDriverConfigurationActionRestartPlatform = 3,
} EFI_DRIVER_CONFIGURATION_ACTION_REQUIRED;

typedef struct EFI_DRIVER_CONFIGURATION2_PROTOCOL {
    EFI_STATUS(EFIAPI *SetOptions)(struct EFI_DRIVER_CONFIGURATION2_PROTOCOL *This, EFI_HANDLE ControllerHandle,
                                   EFI_HANDLE ChildHandle, CHAR8 *Language,
                                   EFI_DRIVER_CONFIGURATION_ACTION_REQUIRED *ActionRequired);
    EFI_STATUS(EFIAPI *OptionsValid)(struct EFI_DRIVER_CONFIGURATION2_PROTOCOL *This, EFI_HANDLE ControllerHandle,
                                     EFI_HANDLE ChildHandle);
    EFI_STATUS(EFIAPI *ForceDefaults)(struct EFI_DRIVER_CONFIGURATION2_PROTOCOL *This, EFI_HANDLE ControllerHandle,
                                      EFI_HANDLE ChildHandle, UINT32 DefaultType,
                                      EFI_DRIVER_CONFIGURATION_ACTION_REQUIRED *ActionRequired);
    CHAR8 *SupportedLanguages;
} EFI_DRIVER_CONFIGURATION2_PROTOCOL;

#define EFI_DRIVER_CONFIGURATION2_PROTOCOL_GUID \
    { 0xbfd7dc1d, 0x24f1, 0x40d9, { 0x82, 0xe7, 0x2e, 0x09, 0xbb, 0x6b, 0x4e, 0xbe } }
#define EFI_HII_CONFIG_ACCESS_PROTOCOL_GUID \
    { 0x330d4706, 0xf2a0, 0x4e4f, { 0xa3, 0x69, 0xb6, 0x6f, 0xa8, 0xd5, 0x43, 0x85 } }

typedef EFI_STATUS(EFIAPI *EFI_KEY_NOTIFY_FUNCTION)(EFI_KEY_DATA *KeyData);
typedef EFI_STATUS(EFIAPI *EFI_REGISTER_KEYSTROKE_NOTIFY)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                                         EFI_KEY_DATA *KeyData,
                                                         EFI_KEY_NOTIFY_FUNCTION KeyNotificationFunction,
                                                         void **NotifyHandle);
typedef EFI_STATUS(EFIAPI *EFI_UNREGISTER_KEYSTROKE_NOTIFY)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                                           void *NotificationHandle);

#endif
