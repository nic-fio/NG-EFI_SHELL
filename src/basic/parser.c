/* NESH BASIC parser: recursive descent from tokens to the AST (Program:
 * main block + procedures); detects incomplete input for the interactive prompt. */
#include "basic.h"

#define MAX_EXPR_DEPTH 64
#define MAX_BLOCK_DEPTH 64

typedef enum {
    TERM_EOF, TERM_NEXT, TERM_WEND, TERM_LOOP, TERM_ELSE, TERM_ELSEIF, TERM_CASE,
    TERM_END_IF, TERM_END_SUB, TERM_END_FUNCTION, TERM_END_SELECT, TERM_END_WHILE,
    TERM_ERROR
} Term;

typedef struct {
    Lexer lx;
    Token tok;      /* current */
    Token pk;       /* lookahead */
    bool has_pk;
    bool line_start;
    Program *prog;
    int depth;
    int block_depth;
    bool failed;
    bool in_proc;
} Parser;

/* ---- Token stream with one token of lookahead ---- */

static Token take_lex(Parser *p)
{
    Token t = p->lx.tok;
    t.end = p->lx.pos;     /* the lexer stops right after the current token */
    p->lx.tok.text = NULL; /* ownership moves to t */
    lex_next(&p->lx);
    return t;
}

static void adv(Parser *p)
{
    p->line_start = p->tok.t == T_NEWLINE;
    free(p->tok.text);
    if (p->has_pk) {
        p->tok = p->pk;
        p->has_pk = false;
    } else {
        p->tok = take_lex(p);
    }
}

static Token *peek(Parser *p)
{
    if (!p->has_pk) {
        p->pk = take_lex(p);
        p->has_pk = true;
    }
    return &p->pk;
}

static void perr(Parser *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void perr(Parser *p, const char *fmt, ...)
{
    if (p->failed)
        return;
    p->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->prog->err, sizeof(p->prog->err), fmt, ap);
    va_end(ap);
    p->prog->err_line = p->tok.line;
}

static void err_unexpected(Parser *p, const char *expected)
{
    if (p->tok.t == T_ERROR)
        perr(p, "%s", p->lx.err);
    else if (expected)
        perr(p, "expected %s, found %s", expected, tok_name(p->tok.t));
    else
        perr(p, "unexpected %s", tok_name(p->tok.t));
    if (p->tok.t == T_EOF)
        p->prog->incomplete = true;
}

static bool accept(Parser *p, TokType t)
{
    if (p->tok.t == t) {
        adv(p);
        return true;
    }
    return false;
}

static bool expect(Parser *p, TokType t)
{
    if (accept(p, t))
        return true;
    err_unexpected(p, tok_name(t));
    return false;
}

static bool end_of_stmt(Parser *p)
{
    return p->tok.t == T_NEWLINE || p->tok.t == T_EOF || p->tok.t == T_COLON || p->tok.t == K_ELSE;
}

static char *lower_dup(const char *s)
{
    char *r = xstrdup(s);
    for (char *q = r; *q; q++)
        *q = (char)tolower((uint8_t)*q);
    return r;
}

static Node *new_node(Parser *p, NodeKind k)
{
    Node *n = xcalloc(1, sizeof(Node));
    n->kind = k;
    n->line = p->tok.line;
    return n;
}

static void block_add(Block *b, Node *n)
{
    b->v = xrealloc(b->v, sizeof(Node *) * (b->n + 1));
    b->v[b->n++] = n;
}

static void node_add_arg(Node *n, Node *a)
{
    n->args = xrealloc(n->args, sizeof(Node *) * (n->nargs + 1));
    n->args[n->nargs++] = a;
}

/* ---- Freeing ---- */

static void node_free(Node *n);

static void block_free(Block *b)
{
    for (int i = 0; i < b->n; i++)
        node_free(b->v[i]);
    free(b->v);
    for (int i = 0; i < b->nlabels; i++)
        free(b->labels[i].name);
    free(b->labels);
    memset(b, 0, sizeof(*b));
}

static void node_free(Node *n)
{
    if (!n)
        return;
    if (n->str)
        str_unref(n->str);
    free(n->name);
    node_free(n->a);
    node_free(n->b);
    node_free(n->c);
    node_free(n->d);
    for (int i = 0; i < n->nargs; i++)
        node_free(n->args[i]);
    free(n->args);
    free(n->seps);
    block_free(&n->body);
    for (int i = 0; i < n->nifs; i++) {
        node_free(n->ifs[i].cond);
        block_free(&n->ifs[i].body);
    }
    free(n->ifs);
    for (int i = 0; i < n->ncases; i++) {
        for (int j = 0; j < n->cases[i].nitems; j++)
            node_free(n->cases[i].items[j]);
        free(n->cases[i].items);
        block_free(&n->cases[i].body);
    }
    free(n->cases);
    free(n);
}

static void proc_free(Proc *pr)
{
    free(pr->name);
    for (int i = 0; i < pr->nparams; i++)
        free(pr->params[i]);
    free(pr->params);
    block_free(&pr->body);
    free(pr);
}

void program_free(Program *prog)
{
    block_free(&prog->main);
    while (prog->procs) {
        Proc *n = prog->procs->next;
        proc_free(prog->procs);
        prog->procs = n;
    }
}

/* ---- Expressions ---- */

static Node *parse_expr(Parser *p);

static Node *binop(Parser *p, int op, Node *a, Node *b, int line)
{
    Node *n = new_node(p, E_BINOP);
    n->op = op;
    n->a = a;
    n->b = b;
    n->line = line;
    return n;
}

/* Arguments inside parentheses; the opening '(' is current. A bare "name()"
 * inside an argument list denotes a whole array (for SPLIT, UBOUND...). */
static bool parse_paren_args(Parser *p, Node *call)
{
    if (!expect(p, T_LPAREN))
        return false;
    if (accept(p, T_RPAREN))
        return true;
    for (;;) {
        Node *a = parse_expr(p);
        if (!a)
            return false;
        node_add_arg(call, a);
        if (accept(p, T_COMMA))
            continue;
        return expect(p, T_RPAREN);
    }
}

static Node *parse_primary(Parser *p)
{
    Node *n;
    switch (p->tok.t) {
    case T_NUM:
        n = new_node(p, E_NUM);
        n->num = p->tok.num;
        adv(p);
        return n;
    case T_STR:
        n = new_node(p, E_STR);
        n->str = str_new(p->tok.text, p->tok.len);
        adv(p);
        return n;
    case T_IDENT:
        n = new_node(p, E_VAR);
        n->name = lower_dup(p->tok.text);
        adv(p);
        if (p->tok.t == T_LPAREN) {
            n->kind = E_CALL;
            if (!parse_paren_args(p, n)) {
                node_free(n);
                return NULL;
            }
        }
        return n;
    case T_LPAREN: {
        adv(p);
        n = parse_expr(p);
        if (!n)
            return NULL;
        if (!expect(p, T_RPAREN)) {
            node_free(n);
            return NULL;
        }
        return n;
    }
    case K_RUN: {
        /* RUN$ is lexed as identifier; plain RUN inside an expression is an error. */
        perr(p, "RUN is a statement: use RUN$(...) to capture the output of a command");
        return NULL;
    }
    default:
        err_unexpected(p, "an expression");
        return NULL;
    }
}

static Node *parse_power(Parser *p)
{
    Node *a = parse_primary(p);
    if (a && p->tok.t == T_CARET) {
        int line = p->tok.line;
        adv(p);
        Node *b = parse_power(p); /* right associative */
        if (!b) {
            node_free(a);
            return NULL;
        }
        return binop(p, T_CARET, a, b, line);
    }
    return a;
}

static Node *parse_unary(Parser *p)
{
    if (p->tok.t == T_MINUS || p->tok.t == T_PLUS) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        if (++p->depth > MAX_EXPR_DEPTH) {
            perr(p, "expression too complex");
            return NULL;
        }
        Node *a = parse_unary(p);
        p->depth--;
        if (!a)
            return NULL;
        if (op == T_PLUS)
            return a;
        Node *n = new_node(p, E_UNOP);
        n->op = T_MINUS;
        n->a = a;
        n->line = line;
        return n;
    }
    return parse_power(p);
}

static Node *parse_mul(Parser *p)
{
    Node *a = parse_unary(p);
    while (a && (p->tok.t == T_STAR || p->tok.t == T_SLASH || p->tok.t == T_BSLASH || p->tok.t == K_MOD)) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        Node *b = parse_unary(p);
        if (!b) {
            node_free(a);
            return NULL;
        }
        a = binop(p, op, a, b, line);
    }
    return a;
}

static Node *parse_add(Parser *p)
{
    Node *a = parse_mul(p);
    while (a && (p->tok.t == T_PLUS || p->tok.t == T_MINUS)) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        Node *b = parse_mul(p);
        if (!b) {
            node_free(a);
            return NULL;
        }
        a = binop(p, op, a, b, line);
    }
    return a;
}

static Node *parse_shift(Parser *p)
{
    Node *a = parse_add(p);
    while (a && (p->tok.t == K_SHL || p->tok.t == K_SHR)) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        Node *b = parse_add(p);
        if (!b) {
            node_free(a);
            return NULL;
        }
        a = binop(p, op, a, b, line);
    }
    return a;
}

static bool is_relop(TokType t)
{
    return t == T_EQ || t == T_NE || t == T_LT || t == T_GT || t == T_LE || t == T_GE;
}

static Node *parse_cmp(Parser *p)
{
    Node *a = parse_shift(p);
    while (a && is_relop(p->tok.t)) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        Node *b = parse_shift(p);
        if (!b) {
            node_free(a);
            return NULL;
        }
        a = binop(p, op, a, b, line);
    }
    return a;
}

static Node *parse_not(Parser *p)
{
    if (p->tok.t == K_NOT) {
        int line = p->tok.line;
        adv(p);
        if (++p->depth > MAX_EXPR_DEPTH) {
            perr(p, "expression too complex");
            return NULL;
        }
        Node *a = parse_not(p);
        p->depth--;
        if (!a)
            return NULL;
        Node *n = new_node(p, E_UNOP);
        n->op = K_NOT;
        n->a = a;
        n->line = line;
        return n;
    }
    return parse_cmp(p);
}

static Node *parse_and(Parser *p)
{
    Node *a = parse_not(p);
    while (a && p->tok.t == K_AND) {
        int line = p->tok.line;
        adv(p);
        Node *b = parse_not(p);
        if (!b) {
            node_free(a);
            return NULL;
        }
        a = binop(p, K_AND, a, b, line);
    }
    return a;
}

static Node *parse_expr(Parser *p)
{
    if (++p->depth > MAX_EXPR_DEPTH) {
        perr(p, "expression too complex");
        return NULL;
    }
    Node *a = parse_and(p);
    while (a && (p->tok.t == K_OR || p->tok.t == K_XOR)) {
        int op = p->tok.t, line = p->tok.line;
        adv(p);
        Node *b = parse_and(p);
        if (!b) {
            node_free(a);
            a = NULL;
            break;
        }
        a = binop(p, op, a, b, line);
    }
    p->depth--;
    return a;
}

/* ---- Statements ---- */

static Term parse_block(Parser *p, Block *b);
static Node *parse_statement(Parser *p);

static const char *term_name(Term t)
{
    switch (t) {
    case TERM_NEXT: return "NEXT";
    case TERM_WEND: return "WEND";
    case TERM_LOOP: return "LOOP";
    case TERM_ELSE: return "ELSE";
    case TERM_ELSEIF: return "ELSEIF";
    case TERM_CASE: return "CASE";
    case TERM_END_IF: return "END IF";
    case TERM_END_SUB: return "END SUB";
    case TERM_END_FUNCTION: return "END FUNCTION";
    case TERM_END_SELECT: return "END SELECT";
    case TERM_END_WHILE: return "END WHILE";
    default: return "end of input";
    }
}

/* Reports a block that ended with the wrong terminator. */
static void bad_term(Parser *p, Term got, const char *opener, const char *closer, int open_line)
{
    if (got == TERM_ERROR)
        return;
    if (got == TERM_EOF) {
        perr(p, "%s without %s (opened at line %d)", opener, closer, open_line);
        p->prog->incomplete = true;
    } else {
        perr(p, "%s where %s was expected (%s opened at line %d)", term_name(got), closer, opener, open_line);
    }
}

static Node *parse_if(Parser *p)
{
    Node *n = new_node(p, S_IF);
    int open_line = p->tok.line;
    adv(p); /* IF */
    Node *cond = parse_expr(p);
    if (!cond || !expect(p, K_THEN)) {
        node_free(cond);
        node_free(n);
        return NULL;
    }
    n->ifs = xcalloc(1, sizeof(IfClause));
    n->nifs = 1;
    n->ifs[0].cond = cond;

    if (p->tok.t != T_NEWLINE && p->tok.t != T_EOF) {
        /* single-line IF: statements up to the end of the line, optional ELSE */
        for (;;) {
            Node *s = parse_statement(p);
            if (p->failed) {
                node_free(s);
                node_free(n);
                return NULL;
            }
            if (s)
                block_add(&n->ifs[0].body, s);
            if (accept(p, T_COLON))
                continue;
            break;
        }
        if (accept(p, K_ELSE)) {
            n->ifs = xrealloc(n->ifs, sizeof(IfClause) * 2);
            memset(&n->ifs[1], 0, sizeof(IfClause));
            n->nifs = 2;
            for (;;) {
                Node *s = parse_statement(p);
                if (p->failed) {
                    node_free(s);
                    node_free(n);
                    return NULL;
                }
                if (s)
                    block_add(&n->ifs[1].body, s);
                if (accept(p, T_COLON))
                    continue;
                break;
            }
        }
        return n;
    }

    /* block IF */
    for (;;) {
        IfClause *cl = &n->ifs[n->nifs - 1];
        Term t = parse_block(p, &cl->body);
        if (t == TERM_END_IF)
            return n;
        if (t == TERM_ELSEIF || t == TERM_ELSE) {
            if (!cl->cond) {
                perr(p, "%s after ELSE", term_name(t));
                break;
            }
            adv(p);
            Node *c = NULL;
            if (t == TERM_ELSEIF) {
                c = parse_expr(p);
                if (!c || !expect(p, K_THEN)) {
                    node_free(c);
                    break;
                }
            }
            n->ifs = xrealloc(n->ifs, sizeof(IfClause) * (n->nifs + 1));
            memset(&n->ifs[n->nifs], 0, sizeof(IfClause));
            n->ifs[n->nifs++].cond = c;
            continue;
        }
        bad_term(p, t, "IF", "END IF", open_line);
        break;
    }
    node_free(n);
    return NULL;
}

static Node *parse_for(Parser *p)
{
    Node *n = new_node(p, S_FOR);
    int open_line = p->tok.line;
    adv(p);
    if (p->tok.t != T_IDENT) {
        err_unexpected(p, "loop variable");
        goto fail;
    }
    n->name = lower_dup(p->tok.text);
    if (strchr(n->name, '$')) {
        perr(p, "FOR variable must be numeric");
        goto fail;
    }
    adv(p);
    if (!expect(p, T_EQ) || !(n->a = parse_expr(p)) || !expect(p, K_TO) || !(n->b = parse_expr(p)))
        goto fail;
    if (accept(p, K_STEP) && !(n->c = parse_expr(p)))
        goto fail;
    Term t = parse_block(p, &n->body);
    if (t != TERM_NEXT) {
        bad_term(p, t, "FOR", "NEXT", open_line);
        goto fail;
    }
    adv(p); /* NEXT */
    if (p->tok.t == T_IDENT) {
        char *v = lower_dup(p->tok.text);
        bool same = !strcmp(v, n->name);
        free(v);
        if (!same) {
            perr(p, "NEXT %s does not match FOR %s", p->tok.text, n->name);
            goto fail;
        }
        adv(p);
    }
    return n;
fail:
    node_free(n);
    return NULL;
}

static Node *parse_while(Parser *p)
{
    Node *n = new_node(p, S_WHILE);
    int open_line = p->tok.line;
    adv(p);
    if (!(n->a = parse_expr(p)))
        goto fail;
    Term t = parse_block(p, &n->body);
    if (t == TERM_WEND) {
        adv(p);
        return n;
    }
    if (t == TERM_END_WHILE)
        return n;
    bad_term(p, t, "WHILE", "WEND", open_line);
fail:
    node_free(n);
    return NULL;
}

static Node *parse_do(Parser *p)
{
    Node *n = new_node(p, S_DO);
    int open_line = p->tok.line;
    adv(p);
    if (p->tok.t == K_WHILE || p->tok.t == K_UNTIL) {
        n->flags |= p->tok.t == K_WHILE ? DO_PRE_WHILE : DO_PRE_UNTIL;
        adv(p);
        if (!(n->a = parse_expr(p)))
            goto fail;
    }
    Term t = parse_block(p, &n->body);
    if (t != TERM_LOOP) {
        bad_term(p, t, "DO", "LOOP", open_line);
        goto fail;
    }
    adv(p);
    if (p->tok.t == K_WHILE || p->tok.t == K_UNTIL) {
        if (n->a) {
            perr(p, "DO loop has a condition both at the start and at the end");
            goto fail;
        }
        n->flags |= p->tok.t == K_WHILE ? DO_POST_WHILE : DO_POST_UNTIL;
        adv(p);
        if (!(n->b = parse_expr(p)))
            goto fail;
    }
    return n;
fail:
    node_free(n);
    return NULL;
}

static Node *parse_select(Parser *p)
{
    Node *n = new_node(p, S_SELECT);
    int open_line = p->tok.line;
    adv(p);
    if (!expect(p, K_CASE) || !(n->a = parse_expr(p)))
        goto fail;
    /* Only blank lines/comments may appear before the first CASE. */
    Block pre = { 0 };
    Term t = parse_block(p, &pre);
    bool nonempty = pre.n > 0;
    block_free(&pre);
    if (nonempty) {
        perr(p, "statements between SELECT CASE and the first CASE");
        goto fail;
    }
    while (t == TERM_CASE) {
        adv(p); /* CASE */
        n->cases = xrealloc(n->cases, sizeof(CaseClause) * (n->ncases + 1));
        CaseClause *cc = &n->cases[n->ncases++];
        memset(cc, 0, sizeof(*cc));
        if (accept(p, K_ELSE)) {
            cc->is_else = true;
        } else {
            for (;;) {
                Node *item;
                if (accept(p, K_IS)) {
                    if (!is_relop(p->tok.t)) {
                        err_unexpected(p, "a comparison operator");
                        goto fail;
                    }
                    int op = p->tok.t;
                    adv(p);
                    Node *v = parse_expr(p);
                    if (!v)
                        goto fail;
                    item = binop(p, op, NULL, v, v->line);
                } else {
                    Node *v = parse_expr(p);
                    if (!v)
                        goto fail;
                    if (accept(p, K_TO)) {
                        Node *hi = parse_expr(p);
                        if (!hi) {
                            node_free(v);
                            goto fail;
                        }
                        item = binop(p, K_TO, v, hi, v->line);
                    } else {
                        item = v;
                    }
                }
                cc->items = xrealloc(cc->items, sizeof(Node *) * (cc->nitems + 1));
                cc->items[cc->nitems++] = item;
                if (!accept(p, T_COMMA))
                    break;
            }
        }
        t = parse_block(p, &cc->body);
        if (cc->is_else && t == TERM_CASE) {
            perr(p, "CASE after CASE ELSE");
            goto fail;
        }
    }
    if (t == TERM_END_SELECT)
        return n;
    bad_term(p, t, "SELECT CASE", "END SELECT", open_line);
fail:
    node_free(n);
    return NULL;
}

static Node *parse_proc(Parser *p, bool is_function)
{
    int open_line = p->tok.line;
    if (p->in_proc || p->block_depth > 1) {
        perr(p, "%s definitions are only allowed at the top level", is_function ? "FUNCTION" : "SUB");
        return NULL;
    }
    adv(p);
    if (p->tok.t != T_IDENT) {
        err_unexpected(p, "a name");
        return NULL;
    }
    Proc *pr = xcalloc(1, sizeof(Proc));
    pr->name = lower_dup(p->tok.text);
    pr->is_function = is_function;
    pr->line = open_line;
    if (!is_function && strchr(pr->name, '$')) {
        perr(p, "SUB names cannot end with $");
        proc_free(pr);
        return NULL;
    }
    adv(p);
    if (accept(p, T_LPAREN) && !accept(p, T_RPAREN)) {
        for (;;) {
            if (p->tok.t != T_IDENT) {
                err_unexpected(p, "a parameter name");
                proc_free(pr);
                return NULL;
            }
            pr->params = xrealloc(pr->params, sizeof(char *) * (pr->nparams + 1));
            pr->params[pr->nparams++] = lower_dup(p->tok.text);
            adv(p);
            if (accept(p, T_COMMA))
                continue;
            if (!expect(p, T_RPAREN)) {
                proc_free(pr);
                return NULL;
            }
            break;
        }
    }
    p->in_proc = true;
    Term t = parse_block(p, &pr->body);
    p->in_proc = false;
    Term want = is_function ? TERM_END_FUNCTION : TERM_END_SUB;
    if (t != want) {
        bad_term(p, t, is_function ? "FUNCTION" : "SUB", term_name(want), open_line);
        proc_free(pr);
        return NULL;
    }
    for (Proc *q = p->prog->procs; q; q = q->next) {
        if (!strcmp(q->name, pr->name)) {
            perr(p, "%s is defined twice", pr->name);
            proc_free(pr);
            return NULL;
        }
    }
    pr->next = p->prog->procs;
    p->prog->procs = pr;
    return new_node(p, S_NOP);
}

/* Name list for DIM/REDIM/LOCAL: name[(size)] {, name[(size)]} */
static Node *parse_decl(Parser *p, NodeKind kind, bool redim)
{
    Node *n = new_node(p, kind);
    n->flags = redim;
    adv(p);
    for (;;) {
        if (p->tok.t != T_IDENT) {
            err_unexpected(p, "a variable name");
            node_free(n);
            return NULL;
        }
        Node *v = new_node(p, E_VAR);
        v->name = lower_dup(p->tok.text);
        adv(p);
        if (p->tok.t == T_LPAREN) {
            v->kind = E_CALL;
            if (!parse_paren_args(p, v) || v->nargs != 1) {
                if (!p->failed)
                    perr(p, "arrays have exactly one dimension: %s(size)", v->name);
                node_free(v);
                node_free(n);
                return NULL;
            }
        }
        node_add_arg(n, v);
        if (!accept(p, T_COMMA))
            break;
    }
    return n;
}

/* Comma separated expressions until the end of the statement. */
static bool parse_expr_list(Parser *p, Node *n, bool allow_empty)
{
    if (end_of_stmt(p))
        return allow_empty ? true : (err_unexpected(p, "an expression"), false);
    for (;;) {
        Node *a = parse_expr(p);
        if (!a)
            return false;
        node_add_arg(n, a);
        if (!accept(p, T_COMMA))
            return true;
    }
}

static Node *parse_statement(Parser *p)
{
    Node *n;
    switch (p->tok.t) {
    case K_IF:
        return parse_if(p);
    case K_FOR:
        return parse_for(p);
    case K_WHILE:
        return parse_while(p);
    case K_DO:
        return parse_do(p);
    case K_SELECT:
        return parse_select(p);
    case K_SUB:
        return parse_proc(p, false);
    case K_FUNCTION:
        return parse_proc(p, true);
    case K_DIM:
        return parse_decl(p, S_DIM, false);
    case K_REDIM:
        return parse_decl(p, S_DIM, true);
    case K_LOCAL:
        if (!p->in_proc) {
            perr(p, "LOCAL can only be used inside SUB or FUNCTION");
            return NULL;
        }
        return parse_decl(p, S_LOCAL, false);
    case K_PRINT: {
        n = new_node(p, S_PRINT);
        adv(p);
        while (!end_of_stmt(p)) {
            Node *a = parse_expr(p);
            if (!a) {
                node_free(n);
                return NULL;
            }
            node_add_arg(n, a);
            n->seps = xrealloc(n->seps, n->nargs);
            n->seps[n->nargs - 1] = 0;
            if (p->tok.t == T_SEMI || p->tok.t == T_COMMA) {
                n->seps[n->nargs - 1] = p->tok.t == T_SEMI ? ';' : ',';
                adv(p);
                continue;
            }
            break;
        }
        return n;
    }
    case K_INPUT: {
        n = new_node(p, S_INPUT);
        adv(p);
        if (p->tok.t != T_IDENT) {
            if (!(n->a = parse_expr(p)))
                goto fail;
            if (!accept(p, T_SEMI) && !accept(p, T_COMMA)) {
                err_unexpected(p, "';' after the INPUT prompt");
                goto fail;
            }
        }
        if (p->tok.t != T_IDENT) {
            err_unexpected(p, "a variable");
            goto fail;
        }
        n->b = parse_primary(p);
        if (!n->b)
            goto fail;
        return n;
    }
    case K_LET:
        adv(p);
        if (p->tok.t != T_IDENT) {
            err_unexpected(p, "a variable");
            return NULL;
        }
        /* fall through */
    case T_IDENT: {
        int line = p->tok.line;
        char *name = lower_dup(p->tok.text);
        adv(p);
        Node *target = new_node(p, E_VAR);
        target->name = name;
        target->line = line;
        if (p->tok.t == T_LPAREN) {
            target->kind = E_CALL;
            if (!parse_paren_args(p, target)) {
                node_free(target);
                return NULL;
            }
        }
        if (accept(p, T_EQ)) {
            if (target->kind == E_CALL && target->nargs != 1) {
                perr(p, "array index expected: %s(index) = value", name);
                node_free(target);
                return NULL;
            }
            n = new_node(p, S_ASSIGN);
            n->line = line;
            n->a = target;
            if (!(n->b = parse_expr(p)))
                goto fail;
            return n;
        }
        /* procedure / builtin call used as a statement */
        n = new_node(p, S_CALL);
        n->line = line;
        n->name = xstrdup(name);
        if (target->kind == E_CALL) {
            n->args = target->args;
            n->nargs = target->nargs;
            target->args = NULL;
            target->nargs = 0;
            if (!end_of_stmt(p)) {
                node_free(target);
                err_unexpected(p, NULL);
                goto fail;
            }
        } else if (!parse_expr_list(p, n, true)) {
            node_free(target);
            goto fail;
        }
        node_free(target);
        return n;
    }
    case K_CALL: {
        adv(p);
        if (p->tok.t != T_IDENT) {
            err_unexpected(p, "a SUB name");
            return NULL;
        }
        n = new_node(p, S_CALL);
        n->name = lower_dup(p->tok.text);
        adv(p);
        if (p->tok.t == T_LPAREN) {
            if (!parse_paren_args(p, n))
                goto fail;
        } else if (!parse_expr_list(p, n, true)) {
            goto fail;
        }
        return n;
    }
    case K_EXIT:
    case K_CONTINUE: {
        bool is_exit = p->tok.t == K_EXIT;
        n = new_node(p, is_exit ? S_EXIT : S_CONTINUE);
        adv(p);
        switch (p->tok.t) {
        case K_FOR: case K_WHILE: case K_DO:
            n->op = p->tok.t;
            adv(p);
            return n;
        case K_SUB: case K_FUNCTION:
            if (!is_exit)
                break;
            n->kind = S_RETURN;
            adv(p);
            return n;
        default:
            if (is_exit) { /* EXIT [code]: leave the script (or the shell at the prompt) */
                n->kind = S_END;
                n->flags = 1;
                if (!end_of_stmt(p) && !(n->a = parse_expr(p)))
                    goto fail;
                return n;
            }
        }
        err_unexpected(p, "FOR, WHILE or DO");
        goto fail;
    }
    case K_GOTO:
        n = new_node(p, S_GOTO);
        adv(p);
        if (p->tok.t != T_IDENT) {
            err_unexpected(p, "a label");
            goto fail;
        }
        n->name = lower_dup(p->tok.text);
        adv(p);
        return n;
    case K_RETURN:
        n = new_node(p, S_RETURN);
        adv(p);
        if (!end_of_stmt(p) && !(n->a = parse_expr(p)))
            goto fail;
        return n;
    case K_END:
        /* END IF / END SUB... are consumed by parse_block */
        n = new_node(p, S_END);
        adv(p);
        if (!end_of_stmt(p) && !(n->a = parse_expr(p)))
            goto fail;
        return n;
    case K_RUN:
        n = new_node(p, S_RUN);
        adv(p);
        if (!parse_expr_list(p, n, false))
            goto fail;
        if (p->tok.t == K_TO || p->tok.t == K_APPEND) {
            n->flags = p->tok.t == K_TO ? RUN_TO : RUN_APPEND;
            adv(p);
            if (!(n->a = parse_expr(p)))
                goto fail;
        }
        return n;
    case K_CLS:
        adv(p);
        return new_node(p, S_CLS);
    case K_COLOR:
    case K_LOCATE:
    case K_SLEEP:
    case K_PAUSE: {
        TokType kw = p->tok.t;
        n = new_node(p, kw == K_COLOR ? S_COLOR : kw == K_LOCATE ? S_LOCATE : kw == K_SLEEP ? S_SLEEP : S_PAUSE);
        adv(p);
        if (!parse_expr_list(p, n, kw == K_PAUSE || kw == K_COLOR))
            goto fail;
        int maxargs = kw == K_COLOR || kw == K_LOCATE ? 2 : 1;
        if (n->nargs > maxargs || (kw == K_LOCATE && n->nargs != 2)) {
            perr(p, "wrong number of arguments for %s", tok_name(kw));
            goto fail;
        }
        return n;
    }
    case T_NEWLINE:
    case T_COLON:
    case T_EOF:
        return NULL; /* empty statement */
    default:
        err_unexpected(p, "a statement");
        return NULL;
    }
fail:
    node_free(n);
    return NULL;
}

static void add_label(Parser *p, Block *b, const char *name)
{
    char *l = lower_dup(name);
    for (int i = 0; i < b->nlabels; i++) {
        if (!strcmp(b->labels[i].name, l)) {
            perr(p, "label %s defined twice", name);
            free(l);
            return;
        }
    }
    b->labels = xrealloc(b->labels, sizeof(Label) * (b->nlabels + 1));
    b->labels[b->nlabels].name = l;
    b->labels[b->nlabels].index = b->n;
    b->nlabels++;
}

/* After "name" and a peeked ':', is the rest of the line empty or a comment? */
static bool label_alone(Parser *p)
{
    const char *s = p->lx.src;
    size_t i = peek(p)->end; /* just after the ':' */
    while (i < p->lx.len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
        i++;
    if (i >= p->lx.len || s[i] == '\n' || s[i] == '\'')
        return true;
    return i + 3 <= p->lx.len && !strncasecmp(s + i, "rem", 3) && (i + 3 == p->lx.len || isspace((uint8_t)s[i + 3]));
}

/* Parses statements until a block terminator (returned) or end of input. */
static Term parse_block(Parser *p, Block *b)
{
    if (++p->block_depth > MAX_BLOCK_DEPTH) {
        perr(p, "blocks nested too deeply");
        p->block_depth--;
        return TERM_ERROR;
    }
    Term result = TERM_ERROR;
    for (;;) {
        if (p->failed)
            break;
        if (p->tok.t == T_NEWLINE || p->tok.t == T_COLON) {
            adv(p);
            continue;
        }
        switch (p->tok.t) {
        case T_EOF: result = TERM_EOF; goto done;
        case K_NEXT: result = TERM_NEXT; goto done;
        case K_WEND: result = TERM_WEND; goto done;
        case K_LOOP: result = TERM_LOOP; goto done;
        case K_ELSE: result = TERM_ELSE; goto done;
        case K_ELSEIF: result = TERM_ELSEIF; goto done;
        case K_CASE: result = TERM_CASE; goto done;
        case K_END: {
            Token *nx = peek(p);
            Term t = nx->t == K_IF ? TERM_END_IF : nx->t == K_SUB ? TERM_END_SUB
                   : nx->t == K_FUNCTION ? TERM_END_FUNCTION : nx->t == K_SELECT ? TERM_END_SELECT
                   : nx->t == K_WHILE ? TERM_END_WHILE : TERM_ERROR;
            if (t != TERM_ERROR) {
                adv(p);
                adv(p);
                result = t;
                goto done;
            }
            break;
        }
        case T_ERROR:
            err_unexpected(p, NULL);
            goto done;
        default:
            break;
        }
        /* label: "name:" alone on its line (so that "mysub: mysub" stays two calls) */
        if (p->tok.t == T_IDENT && p->line_start && peek(p)->t == T_COLON && label_alone(p)) {
            add_label(p, b, p->tok.text);
            adv(p);
            adv(p);
            continue;
        }
        Node *s = parse_statement(p);
        if (p->failed) {
            node_free(s);
            break;
        }
        if (s) {
            if (s->kind == S_NOP)
                node_free(s);
            else
                block_add(b, s);
        }
        if (p->tok.t != T_NEWLINE && p->tok.t != T_COLON && p->tok.t != T_EOF) {
            if (p->tok.t == K_ELSE) /* ELSE on the same line in a block IF */
                continue;
            err_unexpected(p, "end of statement");
            break;
        }
    }
done:
    p->block_depth--;
    return p->failed ? TERM_ERROR : result;
}

bool parse_program(const char *src, size_t len, Program *prog)
{
    memset(prog, 0, sizeof(*prog));
    Parser p;
    memset(&p, 0, sizeof(p));
    p.prog = prog;
    lex_init(&p.lx, src, len);
    p.tok = take_lex(&p);
    p.line_start = true;
    Term t = parse_block(&p, &prog->main);
    if (!p.failed && t != TERM_EOF) {
        const char *what = term_name(t);
        perr(&p, "%s without a matching opening statement", what);
    }
    free(p.tok.text);
    if (p.has_pk)
        free(p.pk.text);
    free(p.lx.tok.text);
    if (p.failed) {
        program_free(prog);
        return false;
    }
    return true;
}
