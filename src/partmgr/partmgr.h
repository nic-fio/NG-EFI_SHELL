/* partmgr.efi: what the screens share. */
#ifndef PARTMGR_H
#define PARTMGR_H

#include "disks.h"
#include "ui.h"
#include "units.h"

#define PARTMGR_VERSION "0.1"

/* "GPT", "MBR" or "no table" */
const char *pm_table_name(int kind);

/* The screen of one disk (diskview.c). */
void pm_disk_screen(PmDisk *d);

#endif
