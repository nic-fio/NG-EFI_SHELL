/* Interpreter internals shared by interp.c and the built-in function modules. */
#ifndef NESH_INTERP_INT_H
#define NESH_INTERP_INT_H

#include "basic.h"

struct Var {
    char *name;
    bool is_array;
    Value v;         /* scalar */
    Value *arr;      /* array elements */
    int64_t count;   /* number of elements (UBOUND + 1) */
    struct Var *next;
};

struct VarTable {
    struct Var **b;
    int nb;
    int count;
};

typedef struct Frame {
    VarTable locals;
    struct Frame *up;
    Proc *proc;
    Value ret;
    bool has_ret;
} Frame;

struct Interp {
    VarTable globals;
    Frame *frame;
    Proc **procs;
    int nprocs;
    Program **progs; /* interactive programs kept alive (they may define procedures) */
    int nprogs;
    int64_t err;
    int argc;
    char **argv;
    bool interactive;
    int call_depth;
    const char *script_name;
    bool has_error;
    char errmsg[256];
    int errline;
    bool ended;
    int end_code;
    char *goto_label;
    char **dir_list;
    int dir_n, dir_i;
    uint64_t rnd;
};

/* Built-in functions. Argument type letters: 's' string, 'n' number,
 * '?' any, 'a' array (passed unevaluated, see interp_array_arg).
 * The last letter repeats for extra arguments. */
typedef bool (*BFn)(Interp *in, Node *call, Value *args, int nargs, Value *out);
typedef struct {
    const char *name; /* lower case, with '$' for string results */
    int minargs, maxargs;
    const char *types;
    BFn fn;
} BFunc;

void basic_register_funcs(const BFunc *table, int n);
const BFunc *basic_find_func(const char *name);
void basic_core_funcs_init(void);

/* Runtime error: records the message and returns false. */
bool rt_err(Interp *in, Node *at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Array passed to a built-in ('a' argument). Creates it if missing when
 * create is set (type from the name suffix). */
struct Var *interp_array_arg(Interp *in, Node *arg, bool create);
void var_array_resize(struct Var *v, int64_t count, bool keep);
bool name_is_str(const char *name);

/* RUN statement / RUN$ function (capture != NULL). Returns the exit code,
 * -1 on error or when the command was exit / exit /b (then in->ended is set). */
int basic_run_command(Interp *in, Node *n, Sbuf *capture);

/* Value -> newly allocated C string. */
char *v_to_cstr(const Value *v);

#endif
