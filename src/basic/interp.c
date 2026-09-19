/* NESH BASIC interpreter: values and variables, expression evaluation,
 * statement execution (tree walking), procedures, RUN/RUN$ and the public API. */
#include "interp_int.h"
#include "../core/shell.h"

#define MAX_CALL_DEPTH 200

typedef enum {
    X_OK, X_EXIT_FOR, X_EXIT_WHILE, X_EXIT_DO, X_CONT_FOR, X_CONT_WHILE, X_CONT_DO,
    X_RETURN, X_GOTO, X_END, X_ERROR
} Exec;

/* ---- Strings and values ---- */

Str *str_new(const char *s, size_t n)
{
    Str *r = xmalloc(sizeof(Str) + n + 1);
    r->ref = 1;
    r->len = n;
    if (n && s) /* s == NULL: the caller fills the buffer */
        memcpy(r->s, s, n);
    r->s[n] = 0;
    return r;
}

Str *str_from(const char *s)
{
    return str_new(s, strlen(s));
}

Str *str_take_sb(Sbuf *b)
{
    Str *r = str_new(b->s ? b->s : "", b->len);
    sb_free(b);
    return r;
}

void str_unref(Str *s)
{
    if (s && --s->ref == 0)
        free(s);
}

Value v_cstr(const char *s)
{
    return v_str(str_from(s));
}

void v_free(Value *v)
{
    if (v->t == V_STR)
        str_unref(v->s);
    v->t = V_INT;
    v->i = 0;
}

Value v_copy(const Value *v)
{
    if (v->t == V_STR)
        str_ref(v->s);
    return *v;
}

char *v_to_cstr(const Value *v)
{
    if (v->t == V_STR)
        return xstrndup(v->s->s, v->s->len);
    return xasprintf("%lld", (long long)v->i);
}

bool name_is_str(const char *name)
{
    size_t l = strlen(name);
    return l && name[l - 1] == '$';
}

static Value zero_for(const char *name)
{
    return name_is_str(name) ? v_cstr("") : v_int(0);
}

/* ---- Variable tables ---- */

static unsigned hash_name(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static Var *vt_find(VarTable *t, const char *name)
{
    if (!t->nb)
        return NULL;
    for (Var *v = t->b[hash_name(name) % t->nb]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return NULL;
}

static Var *vt_add(VarTable *t, const char *name)
{
    if (t->count >= t->nb * 2) {
        int nb = t->nb ? t->nb * 4 : 16;
        Var **b = xcalloc(nb, sizeof(Var *));
        for (int i = 0; i < t->nb; i++) {
            for (Var *v = t->b[i], *n; v; v = n) {
                n = v->next;
                unsigned h = hash_name(v->name) % nb;
                v->next = b[h];
                b[h] = v;
            }
        }
        free(t->b);
        t->b = b;
        t->nb = nb;
    }
    Var *v = xcalloc(1, sizeof(Var));
    v->name = xstrdup(name);
    v->v = zero_for(name);
    unsigned h = hash_name(name) % t->nb;
    v->next = t->b[h];
    t->b[h] = v;
    t->count++;
    return v;
}

static void var_clear(Var *v)
{
    v_free(&v->v);
    for (int64_t i = 0; i < v->count; i++)
        v_free(&v->arr[i]);
    free(v->arr);
    v->arr = NULL;
    v->count = 0;
}

static void vt_free(VarTable *t)
{
    for (int i = 0; i < t->nb; i++) {
        for (Var *v = t->b[i], *n; v; v = n) {
            n = v->next;
            var_clear(v);
            free(v->name);
            free(v);
        }
    }
    free(t->b);
    memset(t, 0, sizeof(*t));
}

void var_array_resize(Var *v, int64_t count, bool keep)
{
    if (!keep) {
        for (int64_t i = 0; i < v->count; i++)
            v_free(&v->arr[i]);
        v->count = 0;
    }
    for (int64_t i = count; i < v->count; i++)
        v_free(&v->arr[i]);
    v->arr = xrealloc(v->arr, sizeof(Value) * (size_t)(count ? count : 1));
    for (int64_t i = v->count; i < count; i++)
        v->arr[i] = zero_for(v->name);
    v->count = count;
    v->is_array = true;
}

/* Lookup order: locals of the current procedure, then globals. */
static Var *var_find(Interp *in, const char *name)
{
    if (in->frame) {
        Var *v = vt_find(&in->frame->locals, name);
        if (v)
            return v;
    }
    return vt_find(&in->globals, name);
}

static Var *var_get(Interp *in, const char *name)
{
    Var *v = var_find(in, name);
    return v ? v : vt_add(&in->globals, name);
}

/* ---- Errors ---- */

bool rt_err(Interp *in, Node *at, const char *fmt, ...)
{
    if (in->has_error)
        return false;
    in->has_error = true;
    in->errline = at ? at->line : 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in->errmsg, sizeof(in->errmsg), fmt, ap);
    va_end(ap);
    return false;
}

static void report_error(Interp *in)
{
    if (in->script_name && !in->interactive)
        err_printf("%s:%d: error: %s\n", in->script_name, in->errline, in->errmsg);
    else if (in->errline > 1)
        err_printf("line %d: error: %s\n", in->errline, in->errmsg);
    else
        err_printf("error: %s\n", in->errmsg);
}

/* ---- Procedures and built-ins ---- */

static const BFunc **bfuncs;
static int nbfuncs;

void basic_register_funcs(const BFunc *table, int n)
{
    bfuncs = xrealloc(bfuncs, sizeof(BFunc *) * (nbfuncs + n));
    for (int i = 0; i < n; i++)
        bfuncs[nbfuncs++] = &table[i];
}

const BFunc *basic_find_func(const char *name)
{
    for (int i = 0; i < nbfuncs; i++)
        if (!strcasecmp(bfuncs[i]->name, name))
            return bfuncs[i];
    return NULL;
}

static Proc *find_proc(Interp *in, const char *name)
{
    for (int i = 0; i < in->nprocs; i++)
        if (!strcmp(in->procs[i]->name, name))
            return in->procs[i];
    return NULL;
}

static bool register_procs(Interp *in, Program *prog)
{
    for (Proc *p = prog->procs; p; p = p->next) {
        if (basic_find_func(p->name)) {
            in->errline = p->line;
            snprintf(in->errmsg, sizeof(in->errmsg), "%s is a built-in function and cannot be redefined", p->name);
            in->has_error = true;
            return false;
        }
        for (int k = 0; k < p->nparams; k++) {
            if (basic_find_func(p->params[k])) {
                in->errline = p->line;
                snprintf(in->errmsg, sizeof(in->errmsg),
                         "parameter %s of %s has the name of a built-in function", p->params[k], p->name);
                in->has_error = true;
                return false;
            }
        }
        Proc *old = find_proc(in, p->name);
        if (old) {
            if (!in->interactive) {
                in->errline = p->line;
                snprintf(in->errmsg, sizeof(in->errmsg), "%s is defined twice", p->name);
                in->has_error = true;
                return false;
            }
            for (int i = 0; i < in->nprocs; i++) /* interactive redefinition */
                if (in->procs[i] == old)
                    in->procs[i] = p;
            continue;
        }
        in->procs = xrealloc(in->procs, sizeof(Proc *) * (in->nprocs + 1));
        in->procs[in->nprocs++] = p;
    }
    return true;
}

/* ---- Evaluation ---- */

static bool eval(Interp *in, Node *n, Value *out);
static Exec exec_block(Interp *in, Block *b);
static bool call_proc(Interp *in, Proc *pr, Node *call, Value *out);

static bool check_break(Interp *in, Node *at)
{
    if (con_break())
        return rt_err(in, at, "interrupted (Ctrl-C)");
    return true;
}

static bool eval_int(Interp *in, Node *n, int64_t *out)
{
    Value v;
    if (!eval(in, n, &v))
        return false;
    if (v.t != V_INT) {
        v_free(&v);
        return rt_err(in, n, "number expected, found a string");
    }
    *out = v.i;
    return true;
}

static bool interp_is_truthy(const Value *v)
{
    return v->t == V_INT ? v->i != 0 : v->s->len != 0;
}

static bool call_builtin(Interp *in, const BFunc *f, Node *call, Node **argn, int nargs, Value *out)
{
    if (nargs < f->minargs || nargs > f->maxargs) {
        if (f->minargs == f->maxargs)
            return rt_err(in, call, "%s expects %d argument%s", f->name, f->minargs, f->minargs == 1 ? "" : "s");
        return rt_err(in, call, "%s expects %d to %d arguments", f->name, f->minargs, f->maxargs);
    }
    Value args[16];
    if (nargs > 16)
        return rt_err(in, call, "too many arguments");
    size_t ntypes = strlen(f->types);
    int done = 0;
    bool ok = true;
    for (int i = 0; i < nargs; i++) {
        char t = ntypes ? f->types[(size_t)i < ntypes ? (size_t)i : ntypes - 1] : '?';
        if (t == 'a') {
            args[i] = v_int(0);
        } else if (!eval(in, argn[i], &args[i])) {
            ok = false;
            break;
        } else if (t == 's' && args[i].t != V_STR) {
            ok = rt_err(in, argn[i], "%s: argument %d must be a string", f->name, i + 1);
            done++;
            break;
        } else if (t == 'n' && args[i].t != V_INT) {
            ok = rt_err(in, argn[i], "%s: argument %d must be a number", f->name, i + 1);
            done++;
            break;
        }
        done++;
    }
    if (ok) {
        *out = v_int(0);
        ok = f->fn(in, call, args, nargs, out);
        if (ok && name_is_str(f->name) != (out->t == V_STR)) { /* internal consistency */
            v_free(out);
            *out = zero_for(f->name);
        }
    }
    for (int i = 0; i < done; i++)
        v_free(&args[i]);
    return ok;
}

Var *interp_array_arg(Interp *in, Node *arg, bool create)
{
    if (arg->kind != E_VAR && !(arg->kind == E_CALL && arg->nargs == 0)) {
        rt_err(in, arg, "array name expected");
        return NULL;
    }
    Var *v = var_find(in, arg->name);
    if (!v && create && basic_find_func(arg->name)) {
        rt_err(in, arg, "%s is the name of a built-in function", arg->name);
        return NULL;
    }
    if (!v && create) {
        v = vt_add(in->frame ? &in->frame->locals : &in->globals, arg->name);
        var_array_resize(v, 0, false);
    }
    if (!v || !v->is_array) {
        rt_err(in, arg, "%s is not an array (declare it with DIM)", arg->name);
        return NULL;
    }
    return v;
}

static bool eval_index(Interp *in, Var *v, Node *idx, Value **slot)
{
    int64_t i;
    if (!eval_int(in, idx, &i))
        return false;
    if (i < 0 || i >= v->count)
        return rt_err(in, idx, "index %lld out of range for %s (0 to %lld)", (long long)i, v->name,
                      (long long)(v->count - 1));
    *slot = &v->arr[i];
    return true;
}

static bool eval_call(Interp *in, Node *n, Value *out)
{
    Var *v = var_find(in, n->name);
    if (v && v->is_array) {
        if (n->nargs != 1)
            return rt_err(in, n, "%s is an array: use %s(index)", n->name, n->name);
        Value *slot;
        if (!eval_index(in, v, n->args[0], &slot))
            return false;
        *out = v_copy(slot);
        return true;
    }
    Proc *pr = find_proc(in, n->name);
    if (pr) {
        if (!pr->is_function)
            return rt_err(in, n, "%s is a SUB and has no value (use it as a statement)", n->name);
        return call_proc(in, pr, n, out);
    }
    const BFunc *f = basic_find_func(n->name);
    if (f)
        return call_builtin(in, f, n, n->args, n->nargs, out);
    return rt_err(in, n, "unknown function or array: %s", n->name);
}

static bool eval_binop(Interp *in, Node *n, Value *out)
{
    Value a, b;
    if (!eval(in, n->a, &a))
        return false;
    /* no short-circuit: AND/OR are bitwise, as in classic BASIC */
    if (!eval(in, n->b, &b)) {
        v_free(&a);
        return false;
    }
    bool ok = true;
    if (a.t == V_STR || b.t == V_STR) {
        if (a.t != b.t) {
            ok = rt_err(in, n, "type mismatch: cannot combine a string and a number with %s (use STR$ or VAL)",
                        tok_name(n->op));
        } else if (n->op == T_PLUS) {
            Str *r = xmalloc(sizeof(Str) + a.s->len + b.s->len + 1);
            r->ref = 1;
            r->len = a.s->len + b.s->len;
            memcpy(r->s, a.s->s, a.s->len);
            memcpy(r->s + a.s->len, b.s->s, b.s->len);
            r->s[r->len] = 0;
            *out = v_str(r);
        } else {
            size_t ml = MIN(a.s->len, b.s->len);
            int c = memcmp(a.s->s, b.s->s, ml);
            if (!c)
                c = a.s->len < b.s->len ? -1 : a.s->len > b.s->len ? 1 : 0;
            bool r;
            switch (n->op) {
            case T_EQ: r = c == 0; break;
            case T_NE: r = c != 0; break;
            case T_LT: r = c < 0; break;
            case T_GT: r = c > 0; break;
            case T_LE: r = c <= 0; break;
            case T_GE: r = c >= 0; break;
            default:
                ok = rt_err(in, n, "operator %s cannot be used with strings", tok_name(n->op));
                r = false;
            }
            if (ok)
                *out = v_int(r ? -1 : 0);
        }
        v_free(&a);
        v_free(&b);
        return ok;
    }
    int64_t x = a.i, y = b.i, r = 0;
    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
    switch (n->op) {
    case T_PLUS: r = (int64_t)(ux + uy); break;
    case T_MINUS: r = (int64_t)(ux - uy); break;
    case T_STAR: r = (int64_t)(ux * uy); break;
    case T_SLASH:
    case T_BSLASH:
    case K_MOD:
        if (y == 0)
            return rt_err(in, n, "division by zero");
        if (x == INT64_MIN && y == -1)
            r = n->op == K_MOD ? 0 : INT64_MIN;
        else
            r = n->op == K_MOD ? x % y : x / y;
        break;
    case T_CARET: {
        if (y < 0)
            return rt_err(in, n, "negative exponent");
        uint64_t base = ux, acc = 1;
        for (uint64_t e = (uint64_t)y; e; e >>= 1) {
            if (e & 1)
                acc *= base;
            base *= base;
        }
        r = (int64_t)acc;
        break;
    }
    case T_EQ: r = x == y ? -1 : 0; break;
    case T_NE: r = x != y ? -1 : 0; break;
    case T_LT: r = x < y ? -1 : 0; break;
    case T_GT: r = x > y ? -1 : 0; break;
    case T_LE: r = x <= y ? -1 : 0; break;
    case T_GE: r = x >= y ? -1 : 0; break;
    case K_AND: r = x & y; break;
    case K_OR: r = x | y; break;
    case K_XOR: r = x ^ y; break;
    case K_SHL: r = y >= 64 || y < 0 ? 0 : (int64_t)(ux << y); break;
    case K_SHR: r = y >= 64 || y < 0 ? 0 : (int64_t)(ux >> y); break; /* logical shift */
    default: return rt_err(in, n, "internal error: bad operator");
    }
    *out = v_int(r);
    return true;
}

static bool eval(Interp *in, Node *n, Value *out)
{
    switch (n->kind) {
    case E_NUM:
        *out = v_int(n->num);
        return true;
    case E_STR:
        *out = v_str(str_ref(n->str));
        return true;
    case E_VAR: {
        Var *v = var_find(in, n->name);
        if (v) {
            if (v->is_array)
                return rt_err(in, n, "%s is an array: use %s(index)", n->name, n->name);
            *out = v_copy(&v->v);
            return true;
        }
        const BFunc *f = basic_find_func(n->name);
        if (f)
            return call_builtin(in, f, n, NULL, 0, out);
        Proc *pr = find_proc(in, n->name);
        if (pr && pr->is_function)
            return call_proc(in, pr, n, out);
        *out = zero_for(n->name);
        return true;
    }
    case E_CALL:
        return eval_call(in, n, out);
    case E_UNOP: {
        Value a;
        if (!eval(in, n->a, &a))
            return false;
        if (a.t != V_INT) {
            v_free(&a);
            return rt_err(in, n, "%s needs a number", n->op == K_NOT ? "NOT" : "unary minus");
        }
        *out = v_int(n->op == K_NOT ? ~a.i : (int64_t)(0 - (uint64_t)a.i));
        return true;
    }
    case E_BINOP:
        return eval_binop(in, n, out);
    default:
        return rt_err(in, n, "internal error: not an expression");
    }
}

/* ---- Procedures ---- */

static bool call_proc(Interp *in, Proc *pr, Node *call, Value *out)
{
    if (call->nargs != pr->nparams)
        return rt_err(in, call, "%s expects %d argument%s, %d given", pr->name, pr->nparams,
                      pr->nparams == 1 ? "" : "s", call->nargs);
    if (in->call_depth >= MAX_CALL_DEPTH)
        return rt_err(in, call, "too many nested calls (recursion limit %d)", MAX_CALL_DEPTH);
    Frame *f = xcalloc(1, sizeof(Frame));
    for (int i = 0; i < pr->nparams; i++) {
        Value v;
        if (!eval(in, call->args[i], &v)) {
            vt_free(&f->locals);
            free(f);
            return false;
        }
        if ((v.t == V_STR) != name_is_str(pr->params[i])) {
            v_free(&v);
            vt_free(&f->locals);
            free(f);
            return rt_err(in, call->args[i], "%s: parameter %s expects a %s", pr->name, pr->params[i],
                          name_is_str(pr->params[i]) ? "string" : "number");
        }
        Var *pv = vt_add(&f->locals, pr->params[i]);
        v_free(&pv->v);
        pv->v = v;
    }
    f->proc = pr;
    f->up = in->frame;
    in->frame = f;
    in->call_depth++;
    Exec x = exec_block(in, &pr->body);
    in->call_depth--;
    in->frame = f->up;
    bool ok = true;
    if (x == X_GOTO) {
        ok = rt_err(in, call, "label %s not found in %s", in->goto_label, pr->name);
    } else if (x == X_ERROR) {
        ok = false;
    } else if (x == X_EXIT_FOR || x == X_EXIT_WHILE || x == X_EXIT_DO || x == X_CONT_FOR ||
               x == X_CONT_WHILE || x == X_CONT_DO) {
        ok = rt_err(in, call, "EXIT/CONTINUE outside of a loop in %s", pr->name);
    }
    if (ok && x == X_END) {
        /* END inside a procedure ends the program: propagate as an error-free stop */
        ok = false;
    }
    if (ok) {
        if (f->has_ret) {
            *out = f->ret;
            f->has_ret = false;
        } else {
            *out = zero_for(pr->name);
        }
        if (pr->is_function && (out->t == V_STR) != name_is_str(pr->name)) {
            v_free(out);
            ok = rt_err(in, call, "FUNCTION %s must return a %s", pr->name, name_is_str(pr->name) ? "string" : "number");
        }
    }
    if (f->has_ret)
        v_free(&f->ret);
    vt_free(&f->locals);
    free(f);
    return ok;
}

/* ---- Statements ---- */

static bool assign(Interp *in, Node *target, Value *v)
{
    if ((v->t == V_STR) != name_is_str(target->name)) {
        v_free(v);
        return rt_err(in, target, "type mismatch: %s is a %s variable", target->name,
                      name_is_str(target->name) ? "string" : "numeric");
    }
    if (target->kind == E_CALL) {
        Var *var = var_find(in, target->name);
        if (!var || !var->is_array) {
            v_free(v);
            return rt_err(in, target, "%s is not an array (declare it with DIM)", target->name);
        }
        Value *slot;
        if (!eval_index(in, var, target->args[0], &slot)) {
            v_free(v);
            return false;
        }
        v_free(slot);
        *slot = *v;
        return true;
    }
    if (basic_find_func(target->name) || find_proc(in, target->name)) {
        v_free(v);
        return rt_err(in, target, "%s is a function name and cannot be assigned", target->name);
    }
    Var *var = var_get(in, target->name);
    if (var->is_array) {
        v_free(v);
        return rt_err(in, target, "%s is an array: use %s(index) = value", target->name, target->name);
    }
    v_free(&var->v);
    var->v = *v;
    return true;
}

static void print_value(const Value *v)
{
    if (v->t == V_STR)
        out_write(v->s->s, v->s->len);
    else
        out_printf("%lld", (long long)v->i);
}

static int print_col; /* approximate output column for ',' tabulation */

static void print_track(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            print_col = 0;
        else if (((uint8_t)s[i] & 0xC0) != 0x80)
            print_col++;
    }
}

static Exec exec_print(Interp *in, Node *n)
{
    for (int i = 0; i < n->nargs; i++) {
        Value v;
        if (!eval(in, n->args[i], &v))
            return X_ERROR;
        char *s = v_to_cstr(&v);
        print_value(&v);
        print_track(s, strlen(s));
        free(s);
        v_free(&v);
        if (n->seps[i] == ',') {
            int pad = 14 - print_col % 14;
            for (int k = 0; k < pad; k++)
                out_write(" ", 1);
            print_col += pad;
        }
    }
    if (!n->nargs || !n->seps[n->nargs - 1]) {
        out_write("\n", 1);
        print_col = 0;
    }
    return X_OK;
}

static Exec exec_input(Interp *in, Node *n)
{
    char *prompt = NULL;
    if (n->a) {
        Value v;
        if (!eval(in, n->a, &v))
            return X_ERROR;
        prompt = v_to_cstr(&v);
        v_free(&v);
    }
    const char *name = n->b->name;
    for (;;) {
        char *line = lineedit_read(prompt ? prompt : "? ", false);
        if (!line) {
            free(prompt);
            rt_err(in, n, "input cancelled");
            return X_ERROR;
        }
        Value v;
        if (name_is_str(name)) {
            v = v_cstr(line);
        } else {
            int64_t x;
            if (!parse_int(line, &x)) {
                free(line);
                err_printf("please enter a number\n");
                continue;
            }
            v = v_int(x);
        }
        free(line);
        free(prompt);
        return assign(in, n->b, &v) ? X_OK : X_ERROR;
    }
}

static Exec exec_for(Interp *in, Node *n)
{
    int64_t start, end, step = 1;
    if (!eval_int(in, n->a, &start) || !eval_int(in, n->b, &end) || (n->c && !eval_int(in, n->c, &step)))
        return X_ERROR;
    if (step == 0) {
        rt_err(in, n, "FOR with STEP 0");
        return X_ERROR;
    }
    if (basic_find_func(n->name)) {
        rt_err(in, n, "%s is the name of a built-in function", n->name);
        return X_ERROR;
    }
    Var *v = var_get(in, n->name);
    if (v->is_array) {
        rt_err(in, n, "%s is an array", n->name);
        return X_ERROR;
    }
    v_free(&v->v);
    v->v = v_int(start);
    for (;;) {
        /* re-read the variable: the body may modify it */
        v = var_get(in, n->name);
        int64_t i = v->v.t == V_INT ? v->v.i : 0;
        if (step > 0 ? i > end : i < end)
            break;
        Exec x = exec_block(in, &n->body);
        if (x == X_EXIT_FOR)
            break;
        if (x != X_OK && x != X_CONT_FOR)
            return x;
        v = var_get(in, n->name);
        i = v->v.t == V_INT ? v->v.i : 0;
        int64_t next = (int64_t)((uint64_t)i + (uint64_t)step);
        if ((step > 0 && next < i) || (step < 0 && next > i))
            break; /* overflow: the loop would never terminate */
        v->v = v_int(next);
    }
    return X_OK;
}

static bool cond_true(Interp *in, Node *c, bool *r)
{
    Value v;
    if (!eval(in, c, &v))
        return false;
    *r = interp_is_truthy(&v);
    v_free(&v);
    return true;
}

static Exec exec_while(Interp *in, Node *n)
{
    for (;;) {
        bool c;
        if (!cond_true(in, n->a, &c))
            return X_ERROR;
        if (!c)
            return X_OK;
        Exec x = exec_block(in, &n->body);
        if (x == X_EXIT_WHILE)
            return X_OK;
        if (x != X_OK && x != X_CONT_WHILE)
            return x;
        if (!check_break(in, n))
            return X_ERROR;
    }
}

static Exec exec_do(Interp *in, Node *n)
{
    for (;;) {
        bool c;
        if (n->flags & (DO_PRE_WHILE | DO_PRE_UNTIL)) {
            if (!cond_true(in, n->a, &c))
                return X_ERROR;
            if ((n->flags & DO_PRE_WHILE) ? !c : c)
                return X_OK;
        }
        Exec x = exec_block(in, &n->body);
        if (x == X_EXIT_DO)
            return X_OK;
        if (x != X_OK && x != X_CONT_DO)
            return x;
        if (n->flags & (DO_POST_WHILE | DO_POST_UNTIL)) {
            if (!cond_true(in, n->b, &c))
                return X_ERROR;
            if ((n->flags & DO_POST_WHILE) ? !c : c)
                return X_OK;
        }
        if (!check_break(in, n))
            return X_ERROR;
    }
}

static int value_cmp(const Value *a, const Value *b)
{
    if (a->t == V_INT)
        return a->i < b->i ? -1 : a->i > b->i;
    size_t ml = MIN(a->s->len, b->s->len);
    int c = memcmp(a->s->s, b->s->s, ml);
    return c ? c : a->s->len < b->s->len ? -1 : a->s->len > b->s->len;
}

static Exec exec_select(Interp *in, Node *n)
{
    Value sel;
    if (!eval(in, n->a, &sel))
        return X_ERROR;
    for (int i = 0; i < n->ncases; i++) {
        CaseClause *cc = &n->cases[i];
        bool match = cc->is_else;
        for (int j = 0; j < cc->nitems && !match; j++) {
            Node *it = cc->items[j];
            if (it->kind == E_BINOP && (it->op == K_TO || !it->a)) {
                Value lo, hi;
                if (it->op == K_TO) {
                    if (!eval(in, it->a, &lo)) {
                        v_free(&sel);
                        return X_ERROR;
                    }
                    if (!eval(in, it->b, &hi)) {
                        v_free(&lo);
                        v_free(&sel);
                        return X_ERROR;
                    }
                    if (lo.t != sel.t || hi.t != sel.t) {
                        v_free(&lo);
                        v_free(&hi);
                        v_free(&sel);
                        rt_err(in, it, "CASE value type does not match SELECT");
                        return X_ERROR;
                    }
                    match = value_cmp(&sel, &lo) >= 0 && value_cmp(&sel, &hi) <= 0;
                    v_free(&lo);
                    v_free(&hi);
                } else {
                    if (!eval(in, it->b, &hi)) {
                        v_free(&sel);
                        return X_ERROR;
                    }
                    if (hi.t != sel.t) {
                        v_free(&hi);
                        v_free(&sel);
                        rt_err(in, it, "CASE value type does not match SELECT");
                        return X_ERROR;
                    }
                    int c = value_cmp(&sel, &hi);
                    v_free(&hi);
                    switch (it->op) {
                    case T_EQ: match = c == 0; break;
                    case T_NE: match = c != 0; break;
                    case T_LT: match = c < 0; break;
                    case T_GT: match = c > 0; break;
                    case T_LE: match = c <= 0; break;
                    default: match = c >= 0; break;
                    }
                }
            } else {
                Value v;
                if (!eval(in, it, &v)) {
                    v_free(&sel);
                    return X_ERROR;
                }
                if (v.t != sel.t) {
                    v_free(&v);
                    v_free(&sel);
                    rt_err(in, it, "CASE value type does not match SELECT");
                    return X_ERROR;
                }
                match = value_cmp(&sel, &v) == 0;
                v_free(&v);
            }
        }
        if (match) {
            v_free(&sel);
            return exec_block(in, &cc->body);
        }
    }
    v_free(&sel);
    return X_OK;
}

static Exec exec_dim(Interp *in, Node *n, bool local)
{
    for (int i = 0; i < n->nargs; i++) {
        Node *d = n->args[i];
        if (basic_find_func(d->name)) {
            rt_err(in, d, "%s is the name of a built-in function", d->name);
            return X_ERROR;
        }
        VarTable *t = local ? &in->frame->locals : (in->frame && vt_find(&in->frame->locals, d->name))
                                                   ? &in->frame->locals : &in->globals;
        Var *v = vt_find(t, d->name);
        if (local && v) {
            rt_err(in, d, "%s is already a local variable", d->name);
            return X_ERROR;
        }
        if (!v)
            v = vt_add(t, d->name);
        if (d->kind == E_CALL) {
            int64_t ub;
            if (!eval_int(in, d->args[0], &ub))
                return X_ERROR;
            if (ub < -1 || ub > 10000000) {
                rt_err(in, d, "invalid array size %lld", (long long)ub);
                return X_ERROR;
            }
            if (v->is_array && !n->flags && !local) {
                rt_err(in, d, "%s is already dimensioned (use REDIM to resize)", d->name);
                return X_ERROR;
            }
            v_free(&v->v);
            var_array_resize(v, ub + 1, n->flags != 0);
        } else if (v->is_array) {
            rt_err(in, d, "%s is an array", d->name);
            return X_ERROR;
        }
    }
    return X_OK;
}

static char *value_arg_string(const Value *v)
{
    return v_to_cstr(v);
}

/* RUN "cmd args" / RUN "cmd", arg1, arg2 [TO file | APPEND file]
 * A single string is parsed like a command line (quotes, "> file");
 * several arguments are passed as they are. */
int basic_run_command(Interp *in, Node *n, Sbuf *capture)
{
    int argc = 0;
    char **argv = NULL;
    char *line = NULL;
    if (n->nargs == 1) {
        Value v;
        if (!eval(in, n->args[0], &v))
            return -1;
        line = value_arg_string(&v);
        v_free(&v);
    } else {
        argv = xcalloc(n->nargs + 1, sizeof(char *));
        for (int i = 0; i < n->nargs; i++) {
            Value v;
            if (!eval(in, n->args[i], &v)) {
                argv_free(argv);
                return -1;
            }
            argv[argc++] = value_arg_string(&v);
            v_free(&v);
        }
    }
    char *redirect = NULL;
    if (n->a) {
        Value v;
        if (!eval(in, n->a, &v)) {
            argv_free(argv);
            free(line);
            return -1;
        }
        char *raw = value_arg_string(&v);
        v_free(&v);
        redirect = path_resolve(raw);
        if (!redirect) {
            rt_err(in, n, "RUN: invalid output file %s", raw);
            free(raw);
            argv_free(argv);
            free(line);
            return -1;
        }
        free(raw);
    }
    int rc;
    if (capture)
        out_push_capture(capture);
    if (redirect) {
        int e = out_push_file(redirect, n->flags == RUN_APPEND);
        if (e) {
            if (capture)
                out_pop();
            err_printf("%s: %s\n", redirect, pal_strerror(e));
            free(redirect);
            argv_free(argv);
            free(line);
            in->err = RC_FAIL;
            return RC_FAIL;
        }
    }
    rc = line ? shell_exec_line(line) : shell_exec_argv(argc, argv);
    if (redirect) {
        int e = out_pop();
        if (e && !rc) {
            err_printf("%s: %s\n", redirect, pal_strerror(e));
            rc = RC_FAIL;
        }
    }
    if (capture)
        out_pop();
    free(redirect);
    argv_free(argv);
    free(line);
    in->err = rc;
    if (shell_exit_requested || shell_script_exit_requested) {
        /* exit (every script) or exit /b (this script): stop like END */
        shell_script_exit_requested = false;
        in->ended = true;
        in->end_code = shell_exit_code;
        return -1;
    }
    return rc;
}

static Exec exec_stmt(Interp *in, Node *n)
{
    switch (n->kind) {
    case S_ASSIGN: {
        Value v;
        if (!eval(in, n->b, &v))
            return X_ERROR;
        return assign(in, n->a, &v) ? X_OK : X_ERROR;
    }
    case S_PRINT:
        return exec_print(in, n);
    case S_INPUT:
        return exec_input(in, n);
    case S_IF:
        for (int i = 0; i < n->nifs; i++) {
            bool c = true;
            if (n->ifs[i].cond && !cond_true(in, n->ifs[i].cond, &c))
                return X_ERROR;
            if (c)
                return exec_block(in, &n->ifs[i].body);
        }
        return X_OK;
    case S_FOR:
        return exec_for(in, n);
    case S_WHILE:
        return exec_while(in, n);
    case S_DO:
        return exec_do(in, n);
    case S_SELECT:
        return exec_select(in, n);
    case S_EXIT:
        return n->op == K_FOR ? X_EXIT_FOR : n->op == K_WHILE ? X_EXIT_WHILE : X_EXIT_DO;
    case S_CONTINUE:
        return n->op == K_FOR ? X_CONT_FOR : n->op == K_WHILE ? X_CONT_WHILE : X_CONT_DO;
    case S_GOTO:
        free(in->goto_label);
        in->goto_label = xstrdup(n->name);
        return X_GOTO;
    case S_CALL: {
        Proc *pr = find_proc(in, n->name);
        Value v;
        if (pr) {
            if (!call_proc(in, pr, n, &v))
                return in->ended ? X_END : X_ERROR;
            v_free(&v);
            return X_OK;
        }
        const BFunc *f = basic_find_func(n->name);
        if (f) {
            if (!call_builtin(in, f, n, n->args, n->nargs, &v))
                return X_ERROR;
            v_free(&v);
            return X_OK;
        }
        Var *var = var_find(in, n->name);
        if (var && !n->nargs) {
            rt_err(in, n, "%s is a variable: did you mean to assign it (%s = ...)?", n->name, n->name);
            return X_ERROR;
        }
        rt_err(in, n, "unknown statement or SUB: %s (to run a shell command use RUN \"%s ...\")", n->name, n->name);
        return X_ERROR;
    }
    case S_RETURN:
        if (!in->frame) {
            rt_err(in, n, "RETURN outside of SUB or FUNCTION");
            return X_ERROR;
        }
        if (n->a) {
            if (!in->frame->proc->is_function) {
                rt_err(in, n, "a SUB cannot return a value");
                return X_ERROR;
            }
            Value v;
            if (!eval(in, n->a, &v))
                return X_ERROR;
            if (in->frame->has_ret)
                v_free(&in->frame->ret);
            in->frame->ret = v;
            in->frame->has_ret = true;
        }
        return X_RETURN;
    case S_END: {
        int64_t code = 0;
        if (n->a && !eval_int(in, n->a, &code))
            return X_ERROR;
        in->ended = true;
        in->end_code = (int)code;
        return X_END;
    }
    case S_DIM:
        return exec_dim(in, n, false);
    case S_LOCAL:
        return exec_dim(in, n, true);
    case S_RUN:
        if (basic_run_command(in, n, NULL) < 0)
            return in->ended ? X_END : X_ERROR;
        return X_OK;
    case S_CLS:
        if (out_is_console())
            pal_con_clear();
        return X_OK;
    case S_COLOR: {
        int64_t fg = 7, bg = 0;
        int cfg, cbg;
        pal_con_get_color(&cfg, &cbg);
        if (n->nargs == 0) {
            out_reset_color();
            return X_OK;
        }
        if (!eval_int(in, n->args[0], &fg))
            return X_ERROR;
        bg = cbg;
        if (n->nargs > 1 && !eval_int(in, n->args[1], &bg))
            return X_ERROR;
        if (fg < 0 || fg > 15 || bg < 0 || bg > 7) {
            rt_err(in, n, "COLOR: foreground 0-15, background 0-7");
            return X_ERROR;
        }
        out_color((int)fg, (int)bg);
        return X_OK;
    }
    case S_LOCATE: {
        int64_t row, col;
        if (!eval_int(in, n->args[0], &row) || !eval_int(in, n->args[1], &col))
            return X_ERROR;
        if (out_is_console() && row >= 1 && col >= 1)
            pal_con_set_cursor((int)col - 1, (int)row - 1);
        return X_OK;
    }
    case S_SLEEP: {
        int64_t ms;
        if (!eval_int(in, n->args[0], &ms))
            return X_ERROR;
        uint64_t end = pal_ticks_ms() + (ms > 0 ? (uint64_t)ms : 0);
        while (pal_ticks_ms() < end) {
            uint64_t left = end - pal_ticks_ms();
            pal_sleep_ms(left > 20 ? 20 : (uint32_t)left);
            if (!check_break(in, n))
                return X_ERROR;
        }
        return X_OK;
    }
    case S_PAUSE: {
        char *msg = NULL;
        if (n->nargs) {
            Value v;
            if (!eval(in, n->args[0], &v))
                return X_ERROR;
            msg = v_to_cstr(&v);
            v_free(&v);
        }
        bool ok = con_pause(msg);
        free(msg);
        if (!ok && con_break()) {
            rt_err(in, n, "interrupted (Ctrl-C)");
            return X_ERROR;
        }
        return X_OK;
    }
    case S_NOP:
        return X_OK;
    default:
        rt_err(in, n, "internal error: not a statement");
        return X_ERROR;
    }
}

static Exec exec_block(Interp *in, Block *b)
{
    for (int i = 0; i < b->n; i++) {
        if (!check_break(in, b->v[i]))
            return X_ERROR;
        Exec x = exec_stmt(in, b->v[i]);
        if (x == X_ERROR && in->ended && !in->has_error)
            x = X_END; /* END executed inside a FUNCTION called from an expression */
        if (x == X_OK)
            continue;
        if (x == X_GOTO) {
            int target = -1;
            for (int k = 0; k < b->nlabels; k++)
                if (!strcmp(b->labels[k].name, in->goto_label))
                    target = b->labels[k].index;
            if (target >= 0) {
                i = target - 1;
                continue;
            }
        }
        return x;
    }
    return X_OK;
}

/* ---- Public API ---- */

Interp *interp_new(int argc, char **argv, bool interactive)
{
    Interp *in = xcalloc(1, sizeof(Interp));
    in->argc = argc;
    in->argv = argv;
    in->interactive = interactive;
    in->rnd = pal_ticks_ms() * 6364136223846793005ULL + 1442695040888963407ULL;
    if (!in->rnd)
        in->rnd = 88172645463325252ULL;
    return in;
}

void interp_free(Interp *in)
{
    vt_free(&in->globals);
    for (int i = 0; i < in->nprogs; i++) {
        program_free(in->progs[i]);
        free(in->progs[i]);
    }
    free(in->progs);
    free(in->procs);
    free(in->goto_label);
    for (int i = 0; i < in->dir_n; i++)
        free(in->dir_list[i]);
    free(in->dir_list);
    free(in);
}

int64_t interp_err(Interp *in)
{
    return in->err;
}

void interp_set_err(Interp *in, int64_t code)
{
    in->err = code;
}

bool interp_ended(Interp *in, int *code)
{
    if (in->ended && code)
        *code = in->end_code;
    return in->ended;
}

static int finish_exec(Interp *in, Exec x)
{
    if (x == X_GOTO) {
        rt_err(in, NULL, "label %s not found", in->goto_label);
        x = X_ERROR;
    } else if (x == X_EXIT_FOR || x == X_EXIT_WHILE || x == X_EXIT_DO || x == X_CONT_FOR ||
               x == X_CONT_WHILE || x == X_CONT_DO) {
        rt_err(in, NULL, "EXIT/CONTINUE outside of a loop");
        x = X_ERROR;
    } else if (x == X_RETURN) {
        rt_err(in, NULL, "RETURN outside of SUB or FUNCTION");
        x = X_ERROR;
    }
    if (x == X_ERROR || (in->has_error && !in->ended)) {
        bool brk = con_break();
        report_error(in);
        in->has_error = false;
        con_clear_break();
        in->frame = NULL;
        in->call_depth = 0;
        return brk ? RC_BREAK : RC_FAIL;
    }
    if (in->ended)
        return in->end_code;
    return RC_OK;
}

int interp_run(Interp *in, Program *prog, const char *name)
{
    in->script_name = name;
    if (!register_procs(in, prog)) {
        report_error(in);
        in->has_error = false;
        return RC_FAIL;
    }
    Exec x = exec_block(in, &prog->main);
    return finish_exec(in, x);
}

int interp_exec_interactive(Interp *in, const char *src, bool *incomplete)
{
    Program *prog = xcalloc(1, sizeof(Program));
    *incomplete = false;
    if (!parse_program(src, strlen(src), prog)) {
        if (prog->incomplete) {
            *incomplete = true;
            free(prog);
            return RC_OK;
        }
        if (prog->err_line > 1)
            err_printf("line %d: syntax error: %s\n", prog->err_line, prog->err);
        else
            err_printf("syntax error: %s\n", prog->err);
        free(prog);
        return RC_FAIL;
    }
    in->ended = false;
    int rc = interp_run(in, prog, NULL);
    if (prog->procs) {
        in->progs = xrealloc(in->progs, sizeof(Program *) * (in->nprogs + 1));
        in->progs[in->nprogs++] = prog;
    } else {
        program_free(prog);
        free(prog);
    }
    return rc;
}

/* Decides if an interactive line is BASIC: it starts with a statement
 * keyword, is an assignment "name =" / "name(...) =", or calls a SUB. */
bool interp_is_basic_line(Interp *in, const char *line)
{
    Lexer lx;
    lex_init(&lx, line, strlen(line));
    bool basic = false;
    if (lx.tok.t >= K_FIRST && is_statement_keyword(lx.tok.t)) {
        basic = true;
        if (lx.tok.t == K_EXIT) { /* "exit" alone or with a number is the shell command */
            lex_next(&lx);
            basic = lx.tok.t == K_FOR || lx.tok.t == K_WHILE || lx.tok.t == K_DO ||
                    lx.tok.t == K_SUB || lx.tok.t == K_FUNCTION;
        }
    } else if (lx.tok.t == T_IDENT) {
        char *name = xstrdup(lx.tok.text);
        for (char *q = name; *q; q++)
            *q = (char)tolower((uint8_t)*q);
        size_t after_ident = lx.pos;
        lex_next(&lx);
        if (lx.tok.t == T_EQ) {
            basic = true;
        } else if (lx.tok.t == T_LPAREN) {
            Var *v = var_find(in, name);
            if ((v && v->is_array) || find_proc(in, name)) {
                basic = true;
            } else {
                /* name(...) = value : scan to the matching parenthesis */
                int depth = 0;
                size_t i = after_ident;
                for (; i < strlen(line); i++) {
                    if (line[i] == '"') {
                        while (++i < strlen(line) && line[i] != '"')
                            ;
                    } else if (line[i] == '(') {
                        depth++;
                    } else if (line[i] == ')' && --depth == 0) {
                        break;
                    }
                }
                size_t j = i + 1;
                while (j < strlen(line) && (line[j] == ' ' || line[j] == '\t'))
                    j++;
                basic = depth == 0 && j < strlen(line) && line[j] == '=' && line[j + 1] != '=';
            }
        } else if (find_proc(in, name)) {
            basic = true;
        }
        free(name);
    } else if (lx.tok.t == T_NUM || lx.tok.t == T_STR) {
        basic = false;
    }
    free(lx.tok.text);
    return basic;
}

int basic_run_file(const char *path, int argc, char **argv)
{
    char *src;
    size_t len;
    int e = file_read_text(path, &src, &len);
    if (e) {
        err_printf("%s: %s\n", path, pal_strerror(e));
        return RC_FAIL;
    }
    Program prog;
    if (!parse_program(src, len, &prog)) {
        err_printf("%s:%d: syntax error: %s\n", path, prog.err_line, prog.err);
        free(src);
        return RC_FAIL;
    }
    free(src);
    Interp *in = interp_new(argc, argv, false);
    shell_script_enter();
    int rc = interp_run(in, &prog, path);
    shell_script_leave();
    interp_free(in);
    program_free(&prog);
    return rc;
}
