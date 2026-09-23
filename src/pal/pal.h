/* Platform abstraction layer: everything the portable core needs from the
 * machine. Implemented by pal_efi.c (UEFI) and pal_host.c (Linux, tests).
 *
 * Paths given to the file functions are canonical absolute shell paths:
 * "fsN:\dir\file" (volume name, backslash separators, no "." or ".."). */
#ifndef NESH_PAL_H
#define NESH_PAL_H

#include "../lib/rt.h"

/* ---- Errors ---- */
enum {
    PAL_OK = 0,
    PAL_ENOENT = -1,
    PAL_EACCES = -2,
    PAL_EEXIST = -3,
    PAL_ENOTDIR = -4,
    PAL_EISDIR = -5,
    PAL_ENOSPC = -6,
    PAL_EIO = -7,
    PAL_EINVAL = -8,
    PAL_ENOTSUP = -9,
    PAL_ENOTEMPTY = -10,
    PAL_ENOMEM = -11,
    PAL_EROFS = -12,
    PAL_ENOMEDIA = -13,
    PAL_ESECURITY = -14,
    PAL_EABORT = -15,
};
const char *pal_strerror(int err);

/* ---- Memory ---- */
void *pal_alloc(size_t n);
void pal_free(void *p);

/* ---- Console ---- */
enum {
    KEY_NONE = 0,
    KEY_UP = 1,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_INSERT,
    KEY_DELETE,
    KEY_PGUP,
    KEY_PGDN,
    KEY_F1 = 0x0B, /* F1..F10 = 0x0B..0x14 */
    KEY_ESC = 0x17,
};
#define MOD_CTRL 1
#define MOD_ALT 2
#define MOD_SHIFT 4

typedef struct {
    uint32_t ch;   /* Unicode character, 0 if a special key */
    uint16_t scan; /* KEY_* for special keys */
    uint32_t mods;
} PalKey;

void pal_con_write(const char *utf8, size_t n);
/* timeout_ms < 0: wait forever. Returns false on timeout. */
bool pal_con_read_key(PalKey *k, int timeout_ms);
void pal_con_clear(void);
void pal_con_set_color(int fg, int bg); /* 0..15 / 0..7, EFI palette */
void pal_con_get_color(int *fg, int *bg);
void pal_con_reset_color(void);
void pal_con_size(int *cols, int *rows);
void pal_con_set_cursor(int col, int row);
void pal_con_get_cursor(int *col, int *row);
void pal_con_show_cursor(bool on);
bool pal_con_interactive(void);
void pal_con_raw(bool on); /* host: termios raw mode; no-op on EFI */
bool pal_con_ansi(void);   /* true: ANSI terminal (host), false: absolute cursor positioning (EFI) */

/* ---- Time ---- */
typedef struct {
    int year, month, day, hour, min, sec;
} PalTime;
uint64_t pal_ticks_ms(void);
void pal_sleep_ms(uint32_t ms);
void pal_sleep_us(uint64_t us);
bool pal_get_time(PalTime *t);
int pal_set_time(const PalTime *t);

/* ---- Files ---- */
#define PAL_O_READ 1
#define PAL_O_WRITE 2
#define PAL_O_CREATE 4
#define PAL_O_TRUNC 8
#define PAL_O_APPEND 16

#define PAL_ATTR_READONLY 0x01
#define PAL_ATTR_HIDDEN 0x02
#define PAL_ATTR_SYSTEM 0x04
#define PAL_ATTR_DIR 0x10
#define PAL_ATTR_ARCHIVE 0x20

typedef struct PalFile PalFile;
typedef struct PalDir PalDir;

typedef struct {
    char *name; /* malloc'd, only filled by pal_readdir */
    uint64_t size;
    uint64_t attr;
    PalTime mtime;
    bool is_dir;
} PalStat;

int pal_open(const char *path, int flags, PalFile **f);
int pal_read(PalFile *f, void *buf, size_t n, size_t *got);
int pal_write(PalFile *f, const void *buf, size_t n);
int pal_seek(PalFile *f, uint64_t pos);
int pal_close(PalFile *f);
int pal_stat(const char *path, PalStat *st);
int pal_opendir(const char *path, PalDir **d);
int pal_readdir(PalDir *d, PalStat *st); /* 1 entry, 0 end, <0 error; skips . and .. */
void pal_closedir(PalDir *d);
int pal_mkdir(const char *path);
int pal_remove(const char *path); /* file or empty directory */
int pal_rename(const char *from, const char *to); /* same volume */
int pal_set_attr(const char *path, uint64_t attr);
int pal_set_size(const char *path, uint64_t size);       /* truncate or extend with zeros */
int pal_set_mtime(const char *path, const PalTime *t);
int pal_volume_set_label(int idx, const char *label);

/* ---- Volumes ---- */
typedef struct {
    char name[16];    /* "fs0" */
    char *label;      /* may be empty */
    char *devpath;    /* textual device path, may be empty */
    uint64_t size, free;
    bool readonly;
    bool removable;
} PalVolume;

void pal_volumes_refresh(void);
int pal_volume_count(void);
const PalVolume *pal_volume(int idx);
int pal_boot_volume(void); /* index of the volume the shell was started from, -1 if unknown */

/* ---- System ---- */
enum { PAL_RESET_COLD, PAL_RESET_WARM, PAL_RESET_SHUTDOWN };
void pal_reset(int type);
void pal_exit(int code) __attribute__((noreturn));
const char *pal_platform_name(void);

/* Program arguments (UTF-8); set up by the platform entry point. */
extern int pal_argc;
extern char **pal_argv;

/* Portable entry point of the shell. */
int nesh_main(int argc, char **argv);

#endif
