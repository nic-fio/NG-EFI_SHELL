/* Partition tables of partmgr: GPT and MBR (extended and logical partitions
 * included) read into one description of the disk, changed in memory, and
 * written back. Portable: the disk is reached through read and write
 * functions, so the same code works on a UEFI block device and, in the
 * tests, on a disk image file.
 *
 * ptable.c reads, ptedit.c changes the description (nothing touches the disk
 * until pt_write), ptwrite.c writes tables, backups and restores. */
#ifndef PARTMGR_PTABLE_H
#define PARTMGR_PTABLE_H

#include "../lib/rt.h"

/* Read or write COUNT blocks at LBA; 0 on success, a PAL_E* code otherwise. */
typedef int (*PtReadFn)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef int (*PtWriteFn)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
/* Fills BUF with N random bytes (disk and partition identifiers). */
typedef void (*PtRandomFn)(void *ctx, void *buf, size_t n);

typedef struct {
    PtReadFn read;
    PtWriteFn write;   /* NULL: the disk is read-only */
    PtRandomFn random;
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
    bool changed;         /* added or changed since the table was read */
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
    uint64_t primary_entries_lba, backup_lba, backup_entries_lba;
    bool primary_ok, backup_ok; /* GPT: the two copies passed every check */
    bool hybrid;                /* GPT with an MBR that is not only protective */
    uint8_t lba0[512];          /* the MBR sector as read: boot code, signature */
    bool changed;               /* anything changed since the table was read */
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

/* The types to choose from for a table of KIND, common ones first: the name
 * and the MBR type byte or the GPT type GUID of entry I; false past the end. */
bool pt_type_at(int kind, int i, const char **name, uint8_t *mbr_type, uint8_t guid[16]);

/* GUID as text, upper case: "C12A7328-F81F-11D2-BA4B-00A0C93EC93B". */
void pt_guid_str(const uint8_t g[16], char out[37]);
bool pt_guid_parse(const char *s, uint8_t g[16]);

/* ---- changing the table in memory (ptedit.c) ----
 * The functions that can refuse return NULL on success or a message for the
 * user. Nothing is written to the disk until pt_write. */

/* Blocks per MiB: partitions start on 1 MiB boundaries. */
uint64_t pt_align(const PtTable *t);

/* Replaces the table with an empty one of KIND (PT_GPT, PT_MBR or PT_NONE to
 * delete it), with new identifiers and no boot code: a table made from zero. */
void pt_new(PtTable *t, const PtDev *dev, int kind);

/* The free areas where a partition can be added, in disk order, each at
 * least 1 MiB and starting on a 1 MiB boundary. logical: inside the extended
 * partition of an MBR disk, so only a logical partition fits there. */
typedef struct {
    uint64_t start, size;
    bool logical;
} PtFree;
int pt_free_space(const PtTable *t, PtFree **out); /* returns the count; free(*out) */

/* Adds a partition. The caller fills start, size, the type (mbr_type or
 * type_guid) and, for GPT, name; on MBR disks role is PT_PRIMARY or
 * PT_LOGICAL (a logical partition outside an extended one creates the
 * extended partition over the whole free area). num and guid are assigned. */
const char *pt_add(PtTable *t, const PtDev *dev, const PtPart *p);
/* Deletes partition NUM; deleting the extended one deletes its logicals. */
const char *pt_delete(PtTable *t, int num);
const char *pt_set_type(PtTable *t, int num, uint8_t mbr_type, const uint8_t type_guid[16]);
const char *pt_set_name(PtTable *t, int num, const char *name); /* GPT */
const char *pt_set_active(PtTable *t, int num, bool on);      /* MBR: at most one */
PtPart *pt_find(PtTable *t, int num);
/* Whether partition NUM may be wiped now: not the extended one (its logical
 * partitions and their records are inside), and only while the table on the
 * screen is the one on the disk. NULL or the reason. */
const char *pt_can_wipe(PtTable *t, int num);

/* ---- writing (ptwrite.c) ---- */

/* Writes the table: GPT (protective MBR, both copies), MBR (with the chain
 * of logical partitions) or, for PT_NONE, destroys the old table. */
int pt_write(const PtDev *dev, const PtTable *t);

/* A backup holds the blocks of the table as read (MBR and extended boot
 * records; or protective MBR and both GPT copies), with the disk's size and a
 * checksum. pt_backup builds it in memory; pt_restore checks it against the
 * disk and writes it back. */
int pt_backup(const PtDev *dev, const PtTable *t, uint8_t **data, size_t *len);
const char *pt_restore(const PtDev *dev, const uint8_t *data, size_t len);

/* Wipe: overwrites COUNT blocks from START twice, with random data and then
 * with zeros. PROGRESS is called before the first chunk of each pass and
 * after every chunk, with the pass (1 or 2) and the blocks done in it; when
 * it returns false the wipe stops. Returns 0, PAL_EABORT when stopped, or
 * the error of the write that failed. */
typedef bool (*PtProgressFn)(void *ctx, int pass, uint64_t done, uint64_t total);
int pt_wipe(const PtDev *dev, uint64_t start, uint64_t count, PtProgressFn progress, void *ctx);

#endif
