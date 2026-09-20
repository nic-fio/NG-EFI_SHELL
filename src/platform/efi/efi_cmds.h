/* Shared declarations of the EFI-only command modules. */
#ifndef NESH_EFI_CMDS_H
#define NESH_EFI_CMDS_H

#include "../../pal/efi_glue.h"
#include "../../core/shell.h"

void platform_print_version(void);
void efi_print_gop_summary(void);
EFI_MEMORY_DESCRIPTOR *efi_memory_map(UINTN *count, UINTN *desc_size);
int efi_start_image(const char *path, int argc, char **argv, bool driver_ok);
bool efi_blocked_by_secure_boot(EFI_STATUS st); /* image refused because it is not signed */

/* UEFI variables (efi_var.c) */
bool guid_parse(const char *s, EFI_GUID *g);   /* text GUID or a known name ("global", "db"...) */
void guid_format(const EFI_GUID *g, char out[37]);
const char *guid_name(const EFI_GUID *g);       /* friendly name or NULL */
/* Reads a variable; returns malloc'd data or NULL (status in *st). */
void *efi_var_read(const char *name, const EFI_GUID *g, size_t *size, UINT32 *attr, EFI_STATUS *st);
EFI_STATUS efi_var_write(const char *name, const EFI_GUID *g, UINT32 attr, const void *data, size_t size);
void efi_var_init(void);

/* GUID names (efi_guids.c) */
const char *guid_db_name(const EFI_GUID *g);
bool guid_db_find(const char *name, EFI_GUID *g);
bool guid_db_register(const EFI_GUID *g, const char *name);
const char *guid_str(const EFI_GUID *g, char buf[37]);

/* Shell protocol (efi_shellproto.c) */
void efi_shell_protocol_install(void);
void efi_shell_protocol_uninstall(void);
SHELL_FILE_HANDLE efi_console_file(int which); /* 0 stdin, 1 stdout, 2 stderr */
bool efi_break_pending(void);                  /* Ctrl-C seen by the key notification */
void efi_break_clear(void);
char *efi_device_name(EFI_HANDLE h, bool component_name, bool device_path); /* malloc'd or NULL */

/* Boot manager (efi_boot.c) */
void efi_boot_init(void);

/* Drivers and devices (efi_drivers.c) */
void efi_drivers_init(void);
void efi_disk_init(void);
void efi_hw_init(void);
void efi_net_init(void);
int efi_handle_index(EFI_HANDLE h);
bool efi_parse_handle(const char *s, EFI_HANDLE *h);

#endif
