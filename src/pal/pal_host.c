/* Platform layer for Linux: used for fast development and automated tests.
 * Volumes are directories listed in NESH_FS (separated by ';'), default ".". */
#define _GNU_SOURCE
#include "pal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

int pal_argc;
char **pal_argv;

static int map_errno(int e)
{
    switch (e) {
    case 0: return PAL_OK;
    case ENOENT: return PAL_ENOENT;
    case EACCES: case EPERM: return PAL_EACCES;
    case EEXIST: return PAL_EEXIST;
    case ENOTDIR: return PAL_ENOTDIR;
    case EISDIR: return PAL_EISDIR;
    case ENOSPC: return PAL_ENOSPC;
    case EINVAL: return PAL_EINVAL;
    case ENOTEMPTY: return PAL_ENOTEMPTY;
    case ENOMEM: return PAL_ENOMEM;
    case EROFS: return PAL_EROFS;
    case EXDEV: return PAL_ENOTSUP;
    default: return PAL_EIO;
    }
}

void *pal_alloc(size_t n) { return malloc(n); }
void pal_free(void *p) { free(p); }

void rt_fatal(const char *msg)
{
    fprintf(stderr, "\nnesh: fatal: %s\n", msg);
    exit(1);
}

/* ---- Console ---- */

static struct termios saved_tio;
static bool raw_on;
static int cur_fg = 7, cur_bg = 0;

void pal_con_write(const char *s, size_t n)
{
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

void pal_con_raw(bool on)
{
    if (!isatty(0) || on == raw_on)
        return;
    if (on) {
        tcgetattr(0, &saved_tio);
        struct termios t = saved_tio;
        t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    } else {
        tcsetattr(0, TCSANOW, &saved_tio);
    }
    raw_on = on;
}

static int read_byte(int timeout_ms)
{
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    unsigned char c;
    if (read(0, &c, 1) != 1)
        return -2;
    return c;
}

bool pal_con_read_key(PalKey *k, int timeout_ms)
{
    memset(k, 0, sizeof(*k));
    int c = read_byte(timeout_ms);
    if (c == -2) { /* EOF on stdin: behave like Ctrl-D */
        k->ch = 4;
        return true;
    }
    if (c < 0)
        return false;
    if (c == 27) {
        int c2 = read_byte(30);
        if (c2 < 0) {
            k->scan = KEY_ESC;
            return true;
        }
        if (c2 == '[' || c2 == 'O') {
            int num = 0, c3;
            while ((c3 = read_byte(30)) >= '0' && c3 <= '9')
                num = num * 10 + (c3 - '0');
            if (c3 == ';') { /* modifiers, e.g. ESC[1;5C */
                int m = 0;
                while ((c3 = read_byte(30)) >= '0' && c3 <= '9')
                    m = m * 10 + (c3 - '0');
                if (m == 5)
                    k->mods |= MOD_CTRL;
            }
            switch (c3) {
            case 'A': k->scan = KEY_UP; break;
            case 'B': k->scan = KEY_DOWN; break;
            case 'C': k->scan = KEY_RIGHT; break;
            case 'D': k->scan = KEY_LEFT; break;
            case 'H': k->scan = KEY_HOME; break;
            case 'F': k->scan = KEY_END; break;
            case 'P': k->scan = KEY_F1; break;
            case '~':
                switch (num) {
                case 1: case 7: k->scan = KEY_HOME; break;
                case 2: k->scan = KEY_INSERT; break;
                case 3: k->scan = KEY_DELETE; break;
                case 4: case 8: k->scan = KEY_END; break;
                case 5: k->scan = KEY_PGUP; break;
                case 6: k->scan = KEY_PGDN; break;
                default: return false;
                }
                break;
            default:
                return false;
            }
            return true;
        }
        k->ch = c2;
        k->mods = MOD_ALT;
        return true;
    }
    if (c == 127) {
        k->ch = 8;
        return true;
    }
    if (c == '\r') {
        k->ch = '\r';
        return true;
    }
    if (c >= 0x80) { /* UTF-8 sequence */
        char buf[4] = { (char)c };
        int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        for (int i = 1; i < n; i++) {
            int x = read_byte(30);
            if (x < 0)
                break;
            buf[i] = (char)x;
        }
        uint32_t cp;
        utf8_decode(buf, n, &cp);
        k->ch = cp;
        return true;
    }
    k->ch = c;
    return true;
}

void pal_con_clear(void)
{
    pal_con_write("\x1b[2J\x1b[H", 7);
}

static const int ansi_map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 }; /* EFI -> ANSI order */

void pal_con_set_color(int fg, int bg)
{
    cur_fg = fg & 15;
    cur_bg = bg & 7;
    if (!isatty(1))
        return;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[0;%s%d;%dm", cur_fg >= 8 ? "1;" : "",
                     30 + ansi_map[cur_fg & 7], 40 + ansi_map[cur_bg]);
    pal_con_write(buf, n);
}

void pal_con_get_color(int *fg, int *bg)
{
    *fg = cur_fg;
    *bg = cur_bg;
}

void pal_con_reset_color(void)
{
    cur_fg = 7;
    cur_bg = 0;
    if (isatty(1))
        pal_con_write("\x1b[0m", 4);
}

void pal_con_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 25;
    }
}

void pal_con_set_cursor(int col, int row)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
    pal_con_write(buf, n);
}

void pal_con_get_cursor(int *col, int *row)
{
    *col = 0;
    *row = 0;
}

void pal_con_show_cursor(bool on)
{
    pal_con_write(on ? "\x1b[?25h" : "\x1b[?25l", 6);
}

bool pal_con_ansi(void)
{
    return true;
}

bool pal_con_interactive(void)
{
    return isatty(0) && isatty(1);
}

/* ---- Time ---- */

uint64_t pal_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pal_sleep_us(uint64_t us)
{
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

void pal_sleep_ms(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void tm_to_pal(const struct tm *tm, PalTime *t)
{
    t->year = tm->tm_year + 1900;
    t->month = tm->tm_mon + 1;
    t->day = tm->tm_mday;
    t->hour = tm->tm_hour;
    t->min = tm->tm_min;
    t->sec = tm->tm_sec;
}

bool pal_get_time(PalTime *t)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm_to_pal(&tm, t);
    return true;
}

int pal_set_time(const PalTime *t)
{
    (void)t;
    return PAL_EACCES;
}

/* ---- Volumes ---- */

typedef struct {
    PalVolume v;
    char *root;
} HostVolume;

static HostVolume *volumes;
static int nvolumes;

void pal_volumes_refresh(void)
{
    if (volumes)
        return; /* static on the host */
    const char *env = getenv("NESH_FS");
    char *list = xstrdup(env && *env ? env : ".");
    char *save = NULL;
    for (char *tok = strtok_r(list, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        volumes = xrealloc(volumes, sizeof(HostVolume) * (nvolumes + 1));
        HostVolume *hv = &volumes[nvolumes];
        memset(hv, 0, sizeof(*hv));
        snprintf(hv->v.name, sizeof(hv->v.name), "fs%d", nvolumes);
        char *abs = realpath(tok, NULL);
        hv->root = abs ? abs : xstrdup(tok);
        const char *base = strrchr(hv->root, '/');
        hv->v.label = xstrdup(base && base[1] ? base + 1 : "HOST");
        hv->v.devpath = xasprintf("Host(%s)", hv->root);
        nvolumes++;
    }
    free(list);
}

int pal_volume_count(void) { return nvolumes; }
const PalVolume *pal_volume(int idx) { return idx >= 0 && idx < nvolumes ? &volumes[idx].v : NULL; }
int pal_boot_volume(void) { return nvolumes ? 0 : -1; }

/* "fsN:\a\b" -> "/root/a/b" (malloc'd), NULL if the volume is unknown. */
static char *host_path(const char *path)
{
    const char *c = strchr(path, ':');
    if (!c)
        return NULL;
    for (int i = 0; i < nvolumes; i++) {
        size_t l = strlen(volumes[i].v.name);
        if ((size_t)(c - path) == l && !strncasecmp(path, volumes[i].v.name, l)) {
            Sbuf b;
            sb_init(&b);
            sb_adds(&b, volumes[i].root);
            for (const char *p = c + 1; *p; p++)
                sb_putc(&b, *p == '\\' ? '/' : *p);
            return sb_steal(&b);
        }
    }
    return NULL;
}

/* ---- Files ---- */

struct PalFile {
    int fd;
};

struct PalDir {
    DIR *d;
    char *path;
};

int pal_open(const char *path, int flags, PalFile **f)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    int of = (flags & (PAL_O_WRITE | PAL_O_APPEND)) ? O_RDWR : O_RDONLY;
    if (flags & PAL_O_CREATE)
        of |= O_CREAT;
    if (flags & PAL_O_TRUNC)
        of |= O_TRUNC;
    if (flags & PAL_O_APPEND)
        of |= O_APPEND;
    struct stat st;
    if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode) && (flags & (PAL_O_WRITE | PAL_O_APPEND))) {
        free(hp);
        return PAL_EISDIR;
    }
    int fd = open(hp, of, 0644);
    free(hp);
    if (fd < 0)
        return map_errno(errno);
    *f = xmalloc(sizeof(PalFile));
    (*f)->fd = fd;
    return PAL_OK;
}

int pal_read(PalFile *f, void *buf, size_t n, size_t *got)
{
    ssize_t r = read(f->fd, buf, n);
    if (r < 0) {
        *got = 0;
        return errno == EISDIR ? PAL_EISDIR : map_errno(errno);
    }
    *got = (size_t)r;
    return PAL_OK;
}

int pal_write(PalFile *f, const void *buf, size_t n)
{
    while (n) {
        ssize_t w = write(f->fd, buf, n);
        if (w < 0)
            return map_errno(errno);
        buf = (const char *)buf + w;
        n -= (size_t)w;
    }
    return PAL_OK;
}

int pal_seek(PalFile *f, uint64_t pos)
{
    return lseek(f->fd, (off_t)pos, SEEK_SET) < 0 ? map_errno(errno) : PAL_OK;
}

int pal_close(PalFile *f)
{
    int r = close(f->fd);
    free(f);
    return r ? map_errno(errno) : PAL_OK;
}

static void fill_stat(PalStat *ps, const struct stat *st)
{
    ps->size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
    ps->is_dir = S_ISDIR(st->st_mode);
    ps->attr = ps->is_dir ? PAL_ATTR_DIR : PAL_ATTR_ARCHIVE;
    if (!(st->st_mode & S_IWUSR))
        ps->attr |= PAL_ATTR_READONLY;
    struct tm tm;
    localtime_r(&st->st_mtime, &tm);
    tm_to_pal(&tm, &ps->mtime);
}

int pal_stat(const char *path, PalStat *ps)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    struct stat st;
    int r = stat(hp, &st);
    free(hp);
    if (r)
        return map_errno(errno);
    ps->name = NULL;
    fill_stat(ps, &st);
    return PAL_OK;
}

int pal_opendir(const char *path, PalDir **d)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    DIR *dir = opendir(hp);
    if (!dir) {
        int e = map_errno(errno);
        free(hp);
        return e;
    }
    *d = xmalloc(sizeof(PalDir));
    (*d)->d = dir;
    (*d)->path = hp;
    return PAL_OK;
}

int pal_readdir(PalDir *d, PalStat *ps)
{
    struct dirent *e;
    while ((e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *full = xasprintf("%s/%s", d->path, e->d_name);
        struct stat st;
        int r = stat(full, &st);
        free(full);
        if (r)
            continue;
        fill_stat(ps, &st);
        ps->name = xstrdup(e->d_name);
        return 1;
    }
    return 0;
}

void pal_closedir(PalDir *d)
{
    closedir(d->d);
    free(d->path);
    free(d);
}

int pal_mkdir(const char *path)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    int r = mkdir(hp, 0755);
    free(hp);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_remove(const char *path)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    int r = remove(hp);
    free(hp);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_rename(const char *from, const char *to)
{
    char *a = host_path(from), *b = host_path(to);
    int r = (a && b) ? rename(a, b) : (errno = ENOENT, -1);
    free(a);
    free(b);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_set_attr(const char *path, uint64_t attr)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    struct stat st;
    int r = stat(hp, &st);
    if (!r)
        r = chmod(hp, (attr & PAL_ATTR_READONLY) ? (st.st_mode & ~0222) : (st.st_mode | S_IWUSR));
    free(hp);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_set_size(const char *path, uint64_t size)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    int r = truncate(hp, (off_t)size);
    free(hp);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_set_mtime(const char *path, const PalTime *t)
{
    char *hp = host_path(path);
    if (!hp)
        return PAL_ENOENT;
    struct tm tm = { 0 };
    tm.tm_year = t->year - 1900;
    tm.tm_mon = t->month - 1;
    tm.tm_mday = t->day;
    tm.tm_hour = t->hour;
    tm.tm_min = t->min;
    tm.tm_sec = t->sec;
    tm.tm_isdst = -1;
    struct timeval tv[2] = { { mktime(&tm), 0 }, { mktime(&tm), 0 } };
    int r = utimes(hp, tv);
    free(hp);
    return r ? map_errno(errno) : PAL_OK;
}

int pal_volume_set_label(int idx, const char *label)
{
    (void)idx;
    (void)label;
    return PAL_ENOTSUP;
}

/* ---- System ---- */

void pal_reset(int type)
{
    (void)type;
    pal_con_raw(false);
    fprintf(stderr, "nesh: reset requested (host build: exiting)\n");
    exit(0);
}

void pal_exit(int code)
{
    pal_con_raw(false);
    exit(code);
}

const char *pal_platform_name(void)
{
    return "Linux host (test build)";
}

int main(int argc, char **argv)
{
    pal_argc = argc;
    pal_argv = argv;
    pal_volumes_refresh();
    int rc = nesh_main(argc, argv);
    pal_con_raw(false);
    return rc;
}
