/* NESH BASIC: lexer, parser (AST) and tree-walking interpreter. */
#ifndef NESH_BASIC_H
#define NESH_BASIC_H

#include "../lib/rt.h"

/* ---- Values ---- */

typedef struct {
    int ref;
    size_t len;
    char s[];
} Str;

typedef enum { V_INT, V_STR } VType;

typedef struct {
    VType t;
    union {
        int64_t i;
        Str *s;
    };
} Value;

Str *str_new(const char *s, size_t n);
Str *str_from(const char *s);
Str *str_take_sb(Sbuf *b);
static inline Str *str_ref(Str *s) { s->ref++; return s; }
void str_unref(Str *s);
static inline Value v_int(int64_t i) { Value v; v.t = V_INT; v.i = i; return v; }
static inline Value v_str(Str *s) { Value v; v.t = V_STR; v.s = s; return v; }
Value v_cstr(const char *s);
void v_free(Value *v);
Value v_copy(const Value *v);

/* ---- Tokens ---- */

typedef enum {
    T_EOF, T_NEWLINE, T_NUM, T_STR, T_IDENT, T_ERROR,
    /* punctuation */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_BSLASH, T_CARET, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_LPAREN, T_RPAREN, T_COMMA, T_SEMI, T_COLON,
    /* keywords */
    K_FIRST,
    K_AND = K_FIRST, K_OR, K_XOR, K_NOT, K_MOD, K_SHL, K_SHR,
    K_IF, K_THEN, K_ELSE, K_ELSEIF, K_END, K_FOR, K_TO, K_STEP, K_NEXT, K_WHILE, K_WEND,
    K_DO, K_LOOP, K_UNTIL, K_SELECT, K_CASE, K_IS, K_EXIT, K_CONTINUE, K_GOTO, K_SUB,
    K_FUNCTION, K_CALL, K_RETURN, K_LOCAL, K_DIM, K_REDIM, K_PRINT, K_INPUT, K_LET, K_RUN,
    K_APPEND, K_CLS, K_COLOR, K_LOCATE, K_PAUSE, K_SLEEP,
    K_LAST
} TokType;

typedef struct {
    TokType t;
    int line;
    int64_t num;
    char *text; /* identifiers (as written) and strings; malloc'd */
    size_t len;
    size_t end; /* source offset just after the token (set by the parser) */
} Token;

typedef struct {
    const char *src;
    size_t pos, len;
    int line;
    Token tok; /* current token */
    char err[160];
} Lexer;

void lex_init(Lexer *lx, const char *src, size_t len);
void lex_next(Lexer *lx);
const char *tok_name(TokType t);
bool is_statement_keyword(TokType t);

/* ---- AST ---- */

typedef enum {
    /* expressions */
    E_NUM, E_STR, E_VAR, E_CALL, E_UNOP, E_BINOP,
    /* statements */
    S_ASSIGN, S_PRINT, S_INPUT, S_IF, S_FOR, S_WHILE, S_DO, S_SELECT, S_EXIT, S_CONTINUE,
    S_GOTO, S_CALL, S_RETURN, S_END, S_DIM, S_LOCAL, S_RUN, S_CLS, S_COLOR, S_LOCATE,
    S_PAUSE, S_SLEEP, S_NOP,
} NodeKind;

typedef struct Node Node;

typedef struct {
    char *name; /* lower case */
    int index;  /* statement index inside the block */
} Label;

typedef struct {
    Node **v;
    int n;
    Label *labels;
    int nlabels;
} Block;

typedef struct {
    Node **items; /* CASE values; E_BINOP with op T_TO for ranges, op=relop and a=NULL for IS */
    int nitems;
    bool is_else;
    Block body;
} CaseClause;

typedef struct {
    Node *cond; /* NULL for ELSE */
    Block body;
} IfClause;

struct Node {
    NodeKind kind;
    int line;
    int op;         /* operator token, EXIT/CONTINUE target, PRINT separators... */
    int64_t num;
    Str *str;       /* string literal */
    char *name;     /* variable/function name, lower case */
    Node *a, *b, *c, *d;
    Node **args;
    int nargs;
    char *seps;     /* PRINT: separator after each item (';' ',' or 0) */
    Block body;
    IfClause *ifs;  /* IF: clauses in order, last may be ELSE */
    int nifs;
    CaseClause *cases;
    int ncases;
    int flags;
};

/* DO loop flags */
#define DO_PRE_WHILE 1
#define DO_PRE_UNTIL 2
#define DO_POST_WHILE 4
#define DO_POST_UNTIL 8
/* RUN flags */
#define RUN_TO 1
#define RUN_APPEND 2

typedef struct Proc {
    char *name; /* lower case, with '$' for string functions */
    bool is_function;
    char **params;
    int nparams;
    Block body;
    int line;
    struct Proc *next;
} Proc;

typedef struct {
    Block main;
    Proc *procs; /* defined in this program */
    char err[200];
    int err_line;
    bool incomplete; /* parse ended inside an open block (interactive continuation) */
} Program;

bool parse_program(const char *src, size_t len, Program *prog);
void program_free(Program *prog);

/* ---- Interpreter ---- */

typedef struct Var Var;
typedef struct VarTable VarTable;

typedef struct Interp Interp;

Interp *interp_new(int argc, char **argv, bool interactive);
void interp_free(Interp *in);
/* Runs a whole program; returns the exit code (END n / runtime error -> 1). */
int interp_run(Interp *in, Program *prog, const char *name);
/* Interactive line (or block of lines). Returns the status of the last command. */
int interp_exec_interactive(Interp *in, const char *src, bool *incomplete);
/* Is the source line a BASIC statement (as opposed to a shell command)? */
bool interp_is_basic_line(Interp *in, const char *line);
int64_t interp_err(Interp *in);
void interp_set_err(Interp *in, int64_t code);
bool interp_ended(Interp *in, int *code); /* END/EXIT executed at top level */

/* Registers the portable built-in functions (call once at startup). */
void basic_core_funcs_init(void);

/* Runs a script file with arguments (argv[0] = path). Returns exit code. */
int basic_run_file(const char *path, int argc, char **argv);

#endif
