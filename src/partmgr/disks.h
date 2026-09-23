/* partmgr: the disks of the machine, as the partition table code sees them.
 * disks_efi.c finds them through the firmware's block devices. */
#ifndef PARTMGR_DISKS_H
#define PARTMGR_DISKS_H

#include "ptable.h"

typedef struct {
    char name[16];     /* blkN, numbered as NESH's map numbers block devices */
    char kind[16];     /* NVMe, SATA, USB, SCSI, SD, disk... from the device path */
    char *devpath;     /* the device path as text */
    uint64_t size;     /* bytes */
    bool removable;
    bool readonly;     /* the medium is write-protected */
    bool boot;         /* partmgr was started from this disk: never written */
    PtDev dev;         /* reads and writes this disk; dev.write is NULL when read-only or boot */
} PmDisk;

/* Finds the whole disks with a medium (partitions and empty drives are left
 * out). Returns the count; the array stays valid until the next call. */
int pm_disks(PmDisk **out);

#endif
