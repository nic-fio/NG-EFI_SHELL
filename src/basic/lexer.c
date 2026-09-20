/* NESH BASIC lexer: turns source text into tokens (numbers, strings, names,
 * keywords, punctuation); skips comments and " _" line continuations. */
#include "basic.h"

static const struct {
    const char *name;
    TokType t;
} keywords[] = {
    { "and", K_AND }, { "or", K_OR }, { "xor", K_XOR }, { "not", K_NOT }, { "mod", K_MOD },
    { "shl", K_SHL }, { "shr", K_SHR }, { "if", K_IF }, { "then", K_THEN }, { "else", K_ELSE },
    { "elseif", K_ELSEIF }, { "end", K_END }, { "for", K_FOR }, { "to", K_TO }, { "step", K_STEP },
    { "next", K_NEXT }, { "while", K_WHILE }, { "wend", K_WEND }, { "do", K_DO }, { "loop", K_LOOP },
    { "until", K_UNTIL }, { "select", K_SELECT }, { "case", K_CASE }, { "is", K_IS },
    { "exit", K_EXIT }, { "continue", K_CONTINUE }, { "goto", K_GOTO }, { "sub", K_SUB },
    { "function", K_FUNCTION }, { "call", K_CALL }, { "return", K_RETURN }, { "local", K_LOCAL },
    { "dim", K_DIM }, { "redim", K_REDIM }, { "print", K_PRINT }, { "input", K_INPUT },
    { "let", K_LET }, { "run", K_RUN }, { "append", K_APPEND }, { "cls", K_CLS },
    { "color", K_COLOR }, { "locate", K_LOCATE }, { "pause", K_PAUSE }, { "sleep", K_SLEEP },
};

const char *tok_name(TokType t)
{
    static const char *punct[] = {
        "end of input", "end of line", "number", "string", "identifier", "invalid character",
        "'+'", "'-'", "'*'", "'/'", "'\\'", "'^'", "'='", "'<>'", "'<'", "'>'", "'<='", "'>='",
        "'('", "')'", "','", "';'", "':'",
    };
    if (t < K_FIRST)
        return punct[t];
    for (size_t i = 0; i < ARRAY_SIZE(keywords); i++)
        if (keywords[i].t == t)
            return keywords[i].name;
    return "?";
}

/* Is name (any case) a keyword of the language? */
bool basic_is_keyword(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(keywords); i++)
        if (!strcasecmp(keywords[i].name, name))
            return true;
    return false;
}

bool is_statement_keyword(TokType t)
{
    switch (t) {
    case K_IF: case K_FOR: case K_WHILE: case K_DO: case K_SELECT: case K_EXIT:
    case K_CONTINUE: case K_GOTO: case K_SUB: case K_FUNCTION: case K_CALL: case K_RETURN:
    case K_LOCAL: case K_DIM: case K_REDIM: case K_PRINT: case K_INPUT: case K_LET: case K_RUN:
    case K_CLS: case K_COLOR: case K_LOCATE: case K_PAUSE: case K_SLEEP: case K_END:
    case K_NEXT: case K_WEND: case K_LOOP: case K_ELSE: case K_ELSEIF: case K_CASE:
        return true;
    default:
        return false;
    }
}

void lex_init(Lexer *lx, const char *src, size_t len)
{
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = len;
    lx->line = 1;
    /* Skip a UTF-8 BOM and a "#!" first line. */
    if (len >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3))
        lx->pos = 3;
    if (lx->pos + 1 < len && src[lx->pos] == '#' && src[lx->pos + 1] == '!')
        while (lx->pos < len && src[lx->pos] != '\n')
            lx->pos++;
    lex_next(lx);
}

static int peekc(Lexer *lx, size_t off)
{
    return lx->pos + off < lx->len ? (uint8_t)lx->src[lx->pos + off] : -1;
}

static void set_tok(Lexer *lx, TokType t)
{
    lx->tok.t = t;
    lx->tok.line = lx->line;
}

static void lex_error(Lexer *lx, const char *msg)
{
    snprintf(lx->err, sizeof(lx->err), "%s", msg);
    set_tok(lx, T_ERROR);
}

static int digit_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 99;
}

static void lex_number(Lexer *lx, int base)
{
    uint64_t v = 0;
    int digits = 0;
    bool overflow = false;
    for (;;) {
        int c = peekc(lx, 0);
        if (c == '_' && digits) {
            lx->pos++;
            continue;
        }
        int d = c < 0 ? 99 : digit_val(c);
        if (d >= base)
            break;
        uint64_t nv = v * base + d;
        if (base == 10 && (v > UINT64_MAX / 10 || nv < v))
            overflow = true;
        v = nv;
        digits++;
        lx->pos++;
    }
    if (!digits) {
        lex_error(lx, "malformed number");
        return;
    }
    if (isalnum(peekc(lx, 0))) {
        lex_error(lx, "malformed number");
        return;
    }
    if (overflow || (base == 10 && v > (uint64_t)INT64_MAX + 1)) {
        lex_error(lx, "number too large");
        return;
    }
    set_tok(lx, T_NUM);
    lx->tok.num = (int64_t)v;
}

void lex_next(Lexer *lx)
{
    free(lx->tok.text);
    lx->tok.text = NULL;
    lx->tok.len = 0;
    if (lx->tok.t == T_ERROR && lx->err[0])
        return; /* sticky */
again:
    while (lx->pos < lx->len) {
        int c = peekc(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
        } else if (c == '_' && lx->pos > 0 && isspace((uint8_t)lx->src[lx->pos - 1])) {
            /* line continuation: " _" at the end of a line */
            size_t p = lx->pos + 1;
            while (p < lx->len && (lx->src[p] == ' ' || lx->src[p] == '\t' || lx->src[p] == '\r'))
                p++;
            if (p < lx->len && lx->src[p] == '\n') {
                lx->pos = p + 1;
                lx->line++;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (lx->pos >= lx->len) {
        set_tok(lx, T_EOF);
        return;
    }
    int c = peekc(lx, 0);
    if (c == '\'') {
        while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
            lx->pos++;
        goto again;
    }
    if (c == '\n') {
        set_tok(lx, T_NEWLINE);
        lx->pos++;
        lx->line++;
        return;
    }
    if (isdigit(c)) {
        if (c == '0' && (peekc(lx, 1) | 32) == 'x') {
            lx->pos += 2;
            lex_number(lx, 16);
        } else if (c == '0' && (peekc(lx, 1) | 32) == 'b' && (peekc(lx, 2) == '0' || peekc(lx, 2) == '1')) {
            lx->pos += 2;
            lex_number(lx, 2);
        } else {
            lex_number(lx, 10);
        }
        return;
    }
    if (c == '&') {
        int n = peekc(lx, 1) | 32;
        int base = n == 'h' ? 16 : n == 'b' ? 2 : n == 'o' ? 8 : 0;
        if (base) {
            lx->pos += 2;
            lex_number(lx, base);
            return;
        }
        lex_error(lx, "unexpected '&' (use + to join strings)");
        return;
    }
    if (c == '"') {
        Sbuf b;
        sb_init(&b);
        lx->pos++;
        for (;;) {
            int d = peekc(lx, 0);
            if (d < 0 || d == '\n') {
                sb_free(&b);
                lex_error(lx, "unterminated string");
                return;
            }
            lx->pos++;
            if (d == '"') {
                if (peekc(lx, 0) == '"') {
                    sb_putc(&b, '"');
                    lx->pos++;
                    continue;
                }
                break;
            }
            sb_putc(&b, (char)d);
        }
        set_tok(lx, T_STR);
        lx->tok.len = b.len;
        lx->tok.text = sb_steal(&b);
        return;
    }
    if (isalpha(c) || c == '_') {
        size_t start = lx->pos;
        while (lx->pos < lx->len && (isalnum((uint8_t)lx->src[lx->pos]) || lx->src[lx->pos] == '_'))
            lx->pos++;
        bool dollar = peekc(lx, 0) == '$';
        if (dollar)
            lx->pos++;
        size_t n = lx->pos - start;
        if (!dollar) {
            if (n == 3 && !strncasecmp(lx->src + start, "rem", 3) &&
                (lx->pos >= lx->len || isspace((uint8_t)lx->src[lx->pos]))) {
                while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
                    lx->pos++;
                goto again;
            }
            for (size_t i = 0; i < ARRAY_SIZE(keywords); i++) {
                if (strlen(keywords[i].name) == n && !strncasecmp(lx->src + start, keywords[i].name, n)) {
                    set_tok(lx, keywords[i].t);
                    return;
                }
            }
        }
        set_tok(lx, T_IDENT);
        lx->tok.text = xstrndup(lx->src + start, n);
        lx->tok.len = n;
        return;
    }
    lx->pos++;
    switch (c) {
    case '+': set_tok(lx, T_PLUS); return;
    case '-': set_tok(lx, T_MINUS); return;
    case '*': set_tok(lx, T_STAR); return;
    case '/': set_tok(lx, T_SLASH); return;
    case '\\': set_tok(lx, T_BSLASH); return;
    case '^': set_tok(lx, T_CARET); return;
    case '=': set_tok(lx, T_EQ); return;
    case '(': set_tok(lx, T_LPAREN); return;
    case ')': set_tok(lx, T_RPAREN); return;
    case ',': set_tok(lx, T_COMMA); return;
    case ';': set_tok(lx, T_SEMI); return;
    case ':': set_tok(lx, T_COLON); return;
    case '<':
        if (peekc(lx, 0) == '>') {
            lx->pos++;
            set_tok(lx, T_NE);
        } else if (peekc(lx, 0) == '=') {
            lx->pos++;
            set_tok(lx, T_LE);
        } else {
            set_tok(lx, T_LT);
        }
        return;
    case '>':
        if (peekc(lx, 0) == '=') {
            lx->pos++;
            set_tok(lx, T_GE);
        } else {
            set_tok(lx, T_GT);
        }
        return;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "unexpected character '%c'", c >= 0x20 && c < 0x7f ? c : '?');
    lex_error(lx, msg);
}
