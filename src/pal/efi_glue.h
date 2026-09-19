/* Access to UEFI services for EFI-only modules. */
#ifndef NESH_EFI_GLUE_H
#define NESH_EFI_GLUE_H

#include "../../include/efi.h"
#include "pal.h"

extern EFI_HANDLE gImage;
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_RUNTIME_SERVICES *gRT;
extern EFI_LOADED_IMAGE_PROTOCOL *gLoadedImage;

extern EFI_GUID gEfiLoadedImageGuid;
extern EFI_GUID gEfiSimpleFileSystemGuid;
extern EFI_GUID gEfiFileInfoGuid;
extern EFI_GUID gEfiFileSystemInfoGuid;
extern EFI_GUID gEfiDevicePathGuid;
extern EFI_GUID gEfiDevicePathToTextGuid;
extern EFI_GUID gEfiGlobalVariableGuid;

int efi_to_pal(EFI_STATUS st);
const char *efi_strerror(EFI_STATUS st);

/* Firmware handle of a shell volume (index from pal_volume). */
EFI_HANDLE efi_volume_handle(int idx);
/* Device path helpers (results are malloc'd). */
size_t efi_devpath_size(const EFI_DEVICE_PATH_PROTOCOL *dp); /* including end node */
char *efi_devpath_text(const EFI_DEVICE_PATH_PROTOCOL *dp);
/* Full device path for a canonical shell path "fsN:\dir\file". */
EFI_DEVICE_PATH_PROTOCOL *efi_file_devpath(const char *path);
/* Opens a canonical shell path with the file system protocol. */
EFI_STATUS efi_open_path(const char *path, UINT64 mode, UINT64 attr, EFI_FILE_PROTOCOL **out);

/* Called when the shell exits (uninstalls the protocols it provides). */
extern void (*efi_exit_hook)(void);

#endif
