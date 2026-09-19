/* Platform-specific commands for the Linux test build: firmware features
 * are not available, so only a minimal set is provided. */
#include "../core/shell.h"

int platform_run_image(const char *path, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    err_printf("%s: EFI applications cannot run in the host build\n", path);
    return RC_FAIL;
}

bool secure_boot_active(void)
{
    return getenv("NESH_SECUREBOOT") != NULL; /* lets tests exercise the read-only mode */
}

bool hw_write_allowed(const char *cmd)
{
    if (!secure_boot_active())
        return true;
    err_printf("%s: writing to hardware is disabled while Secure Boot is active\n", cmd);
    return false;
}

void platform_print_version(void)
{
}

void platform_env_load(void (*cb)(const char *name, const char *value))
{
    (void)cb;
}

bool platform_env_store(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return true; /* host build: "permanent" variables live only in memory */
}

void platform_alias_load(void (*cb)(const char *name, const char *value))
{
    (void)cb;
}

bool platform_alias_store(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return true;
}

void platform_map_blocks(bool verbose)
{
    (void)verbose;
}

bool platform_map_target(const char *target, char *volname, size_t n)
{
    (void)target;
    (void)volname;
    (void)n;
    return false;
}

int platform_fw_decompress(const uint8_t *in, size_t n, char **out, size_t *out_len)
{
    (void)in, (void)n, (void)out, (void)out_len;
    return -2;
}

int platform_blk_read(const char *dev, uint64_t lba, uint64_t count, uint8_t **buf, size_t *len, uint32_t *bsize)
{
    (void)dev, (void)lba, (void)count, (void)buf, (void)len, (void)bsize;
    return PAL_ENOTSUP;
}

int platform_blk_write(const char *dev, uint64_t lba, const uint8_t *buf, size_t len)
{
    (void)dev, (void)lba, (void)buf, (void)len;
    return PAL_ENOTSUP;
}

int platform_mem_read(uint64_t addr, size_t len, uint8_t **buf)
{
    (void)addr, (void)len, (void)buf;
    return PAL_ENOTSUP;
}

int platform_mem_write(uint64_t addr, const uint8_t *buf, size_t len)
{
    (void)addr, (void)buf, (void)len;
    return PAL_ENOTSUP;
}

bool platform_break_pending(void)
{
    return false;
}

const char *platform_uefi_version(void)
{
    return "none";
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pal_reset(PAL_RESET_COLD);
    return RC_OK;
}

static const Cmd host_cmds[] = {
    { "reset", cmd_reset, "reset", "Restart the machine (host build: exit)", NULL },
};

void platform_cmds_init(void)
{
    shell_register(host_cmds, ARRAY_SIZE(host_cmds));
}
