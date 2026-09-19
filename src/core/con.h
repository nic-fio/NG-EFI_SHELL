/* Console services used by the whole shell: output sinks (console, capture
 * buffer, file), colors, key input with type-ahead buffer, Ctrl-C handling. */
#ifndef NESH_CON_H
#define NESH_CON_H

#include "../pal/pal.h"

/* Output goes to the innermost sink: the console, a capture buffer (RUN$)
 * or a file (RUN ... TO). Error messages always go to the console. */
void out_write(const char *s, size_t n);
void out_puts(const char *s);
void out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void out_color(int fg, int bg); /* only effective on the console */
void out_reset_color(void);
bool out_is_console(void);

void err_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* "-data" output: records of "key=value" lines separated by an empty line. */
bool out_data_mode(void);
void out_set_data_mode(bool on); /* also restarts the record numbering */
void data_record(void);          /* starts a new record */
void data_field(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void data_key(const char *label, char *out, size_t n); /* "Release date" -> "release_date" */
/* "Label: value" (indented, label padded to width) or "label=value" in -data mode. */
void info_line(int indent, int width, const char *label, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

void out_push_capture(Sbuf *b);
int out_push_file(const char *path, bool append); /* returns PAL error */
int out_pop(void);                                /* returns PAL error of the popped file sink */

/* Keyboard. */
bool con_get_key(PalKey *k, int timeout_ms);
void con_poll(void);             /* read pending keys into the buffer, detect Ctrl-C */
bool con_break(void);            /* Ctrl-C pressed since the last con_clear_break() */
void con_clear_break(void);
bool con_pause(const char *msg); /* "press a key"; false if Ctrl-C/ESC */

/* Standard colors (EFI palette). */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LIGHTGRAY,
    C_DARKGRAY, C_LIGHTBLUE, C_LIGHTGREEN, C_LIGHTCYAN, C_LIGHTRED, C_LIGHTMAGENTA, C_YELLOW, C_WHITE
};

#endif
