/* Partition tables of partmgr: reading GPT and MBR (extended and logical
 * partitions included) into one description of the disk. Portable: the disk
 * is reached through a read function, so the same code works on a UEFI block
 * device and, in the tests, on a disk image file. */
#ifndef PARTMGR_PTABLE_H
#define PARTMGR_PTABLE_H

#include "../lib/rt.h"

/* Reads COUNT blocks from LBA into BUF; 0 on success, a PAL_E* code otherwise. */
typedef int (*PtReadFn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

typedef struct {
    PtReadFn read;
    void *ctx;
    uint32_t bsize;   /* bytes per block: 512 or 4096 */
    uint64_t nblocks; /* size of the disk in blocks */
} PtDev;

enum { PT_NONE, PT_MBR, PT_GPT };             /* kind of table */
enum { PT_PRIMARY, PT_EXTENDED, PT_LOGICAL }; /* role of an MBR partition; GPT: primary */

typedef struct {
    int num;              /* GPT: entry index + 1; MBR: 1-4 primary/extended, 5... logical */
    int role;
    uint64_t start, size; /* in blocks */
    /* MBR */
    uint8_t mbr_type;
    bool active;
    uint64_t ebr_lba;     /* logical: the block of the table that describes it */
    /* GPT */
    uint8_t type_guid[16];
    uint8_t guid[16];
    uint64_t attrs;
    char name[112];       /* UTF-8, from 36 UTF-16 units */
} PtPart;

#define PT_MAX_NOTES 8

typedef struct {
    int kind;
    uint32_t bsize;
    uint64_t nblocks;
    /* MBR: disk signature; GPT: disk GUID, usable area and entry array */
    uint32_t mbr_sig;
    uint8_t disk_guid[16];
    uint64_t first_usable, last_usable;
    uint32_t max_entries, entry_size;
    uint64_t primary_entries_lba, backup_lba;
    bool primary_ok, backup_ok; /* GPT: the two copies passed every check */
    bool hybrid;                /* GPT with an MBR that is not only protective */
    int nparts;
    PtPart *parts;              /* in table order */
    int nnotes;
    char notes[PT_MAX_NOTES][100]; /* problems found while reading, for the user */
} PtTable;

/* Reads the table of the disk. Returns 0 even when the disk has no table
 * (kind PT_NONE) or when problems were noted; an error only if the disk
 * cannot be read at all. */
int pt_read(const PtDev *dev, PtTable *t);
void pt_free(PtTable *t);

/* Human-readable name of the partition's type ("EFI system", "Linux"...);
 * NULL when the type is not known. */
const char *pt_type_name(const PtTable *t, const PtPart *p);

/* GUID as text, upper case: "C12A7328-F81F-11D2-BA4B-00A0C93EC93B". */
void pt_guid_str(const uint8_t g[16], char out[37]);

#endif
