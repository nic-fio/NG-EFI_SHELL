/* Parts of the platform layer shared by the UEFI and Linux builds: error names. */
#include "pal.h"

const char *pal_strerror(int err)
{
    switch (err) {
    case PAL_OK: return "success";
    case PAL_ENOENT: return "not found";
    case PAL_EACCES: return "access denied";
    case PAL_EEXIST: return "already exists";
    case PAL_ENOTDIR: return "not a directory";
    case PAL_EISDIR: return "is a directory";
    case PAL_ENOSPC: return "volume full";
    case PAL_EIO: return "I/O error";
    case PAL_EINVAL: return "invalid parameter";
    case PAL_ENOTSUP: return "not supported";
    case PAL_ENOTEMPTY: return "directory not empty";
    case PAL_ENOMEM: return "out of memory";
    case PAL_EROFS: return "write protected";
    case PAL_ENOMEDIA: return "no media";
    case PAL_ESECURITY: return "security violation";
    case PAL_EABORT: return "aborted";
    default: return "unknown error";
    }
}
