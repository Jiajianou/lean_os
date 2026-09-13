#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "paths.h"

extern char **environ;

#define SH_LINE_MAX 4096

#define SH_MAX_DEPTH 64

static int last_status;
static int interactive;
static int exec_keep_redirs;

static int opt_errexit;
static int opt_nounset;
static int opt_noglob;
static int opt_xtrace;
static int errexit_suspend;
static int shell_pid;

enum { FLOW_NONE = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN, FLOW_EXIT };
static int flow;
static int flow_levels;
static int exit_code;

typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
    char *mem;
} Chunk;

typedef struct {
    Chunk *head;
    int retained;
} Arena;

static Arena *cur_arena;

static void die_oom(void) {
    write(2, "sh: out of memory\n", 18);
    _exit(1);
}

static void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 8192 ? n : 8192;
        Chunk *c = (Chunk *)malloc(sizeof(Chunk));
        if (!c) {
            die_oom();
        }
        c->mem = (char *)malloc(cap);
        if (!c->mem) {
            die_oom();
        }
        c->used = 0;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->mem + a->head->used;
    a->head->used += n;
    return p;
}

static void arena_free(Arena *a) {
    if (a->retained) {
        return;
    }
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c->mem);
        free(c);
        c = next;
    }
    a->head = 0;
}

static char *astrndup(const char *s, size_t n) {
    char *p = (char *)arena_alloc(cur_arena, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *astrdup(const char *s) {
    return astrndup(s, strlen(s));
}

typedef struct {
    char *p;
    size_t len, cap;
} Sbuf;

static void sb_init(Sbuf *b) {
    b->p = 0;
    b->len = b->cap = 0;
}

static void sb_putn(Sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = (char *)realloc(b->p, cap);
        if (!p) {
            die_oom();
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_putc(Sbuf *b, char c) {
    sb_putn(b, &c, 1);
}

static void sb_puts(Sbuf *b, const char *s) {
    sb_putn(b, s, strlen(s));
}

static void sb_free(Sbuf *b) {
    free(b->p);
    sb_init(b);
}

typedef struct {
    char **v;
    int n, cap;
} Vec;

static void vec_init(Vec *w) {
    w->v = 0;
    w->n = w->cap = 0;
}

static void vec_push(Vec *w, char *s) {
    if (w->n + 1 >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        char **v = (char **)realloc(w->v, sizeof(char *) * (size_t)w->cap);
        if (!v) {
            die_oom();
        }
        w->v = v;
    }
    w->v[w->n++] = s;
    w->v[w->n] = 0;
}

static void vec_free(Vec *w) {
    for (int i = 0; i < w->n; i++) {
        free(w->v[i]);
    }
    free(w->v);
    vec_init(w);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p) {
        die_oom();
    }
    memcpy(p, s, n);
    return p;
}

static void out_fd_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

static void errmsg(const char *a, const char *b, const char *c) {
    out_fd_str(2, "sh: ");
    out_fd_str(2, a);
    if (b) {
        out_fd_str(2, b);
    }
    if (c) {
        out_fd_str(2, c);
    }
    out_fd_str(2, "\n");
}

static char *num_to_str(long v) {
    static char b[24];
    int i = (int)sizeof(b) - 1;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = '\0';
    if (u == 0) {
        b[--i] = '0';
    }
    while (u) {
        b[--i] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (neg) {
        b[--i] = '-';
    }
    return b + i;
}

typedef struct Var {
    struct Var *next;
    char *name;
    char *value;
    int exported;
    int readonly;
} Var;

static Var *vars;

static Var *var_find(const char *name) {
    for (Var *v = vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            return v;
        }
    }
    return 0;
}

static const char *var_get(const char *name) {
    Var *v = var_find(name);
    if (v) {
        return v->value ? v->value : "";
    }
    const char *e = getenv(name);
    return e ? e : "";
}

static int var_is_set(const char *name) {
    Var *v = var_find(name);
    if (v) {
        return v->value != 0;
    }
    return getenv(name) != 0;
}

static void var_set(const char *name, const char *value) {
    Var *v = var_find(name);
    if (!v) {
        v = (Var *)malloc(sizeof(Var));
        if (!v) {
            die_oom();
        }
        v->name = xstrdup(name);
        v->value = 0;
        v->exported = getenv(name) != 0;
        v->readonly = 0;
        v->next = vars;
        vars = v;
    }
    if (v->readonly) {
        errmsg(name, ": is read only", 0);
        return;
    }
    free(v->value);
    v->value = xstrdup(value);
    if (v->exported) {
        setenv(name, value, 1);
    }
}

static void var_export(const char *name) {
    Var *v = var_find(name);
    if (!v) {
        var_set(name, var_get(name));
        v = var_find(name);
    }
    v->exported = 1;
    setenv(name, v->value ? v->value : "", 1);
}

static void var_unset(const char *name) {
    Var **pp = &vars;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            Var *dead = *pp;
            *pp = dead->next;
            free(dead->name);
            free(dead->value);
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    unsetenv(name);
}

static char **pos_params;
static int pos_count;
static char *script_name = (char *)"sh";

static void set_positional(char **argv, int n) {
    for (int i = 0; i < pos_count; i++) {
        free(pos_params[i]);
    }
    free(pos_params);
    pos_params = 0;
    pos_count = 0;
    if (n > 0) {
        pos_params = (char **)malloc(sizeof(char *) * (size_t)n);
        if (!pos_params) {
            die_oom();
        }
        for (int i = 0; i < n; i++) {
            pos_params[i] = xstrdup(argv[i]);
        }
        pos_count = n;
    }
}

static const char *bracket_end(const char *pat) {
    const char *p = pat + 1;
    if (*p == '!' || *p == '^') {
        p++;
    }
    if (*p == ']') {
        p++;
    }
    while (*p && *p != ']') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        p++;
    }
    return *p == ']' ? p + 1 : 0;
}

static int bracket_has(const char *pat, char c) {
    const char *p = pat + 1;
    int negate = 0;
    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    int found = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        char lo = *p;
        if (lo == '\\' && p[1]) {
            p++;
            lo = *p;
        }
        p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            char hi = *p;
            if (hi == '\\' && p[1]) {
                p++;
                hi = *p;
            }
            p++;
            if ((unsigned char)c >= (unsigned char)lo &&
                (unsigned char)c <= (unsigned char)hi) {
                found = 1;
            }
        } else if (c == lo) {
            found = 1;
        }
    }
    return negate ? !found : found;
}

static int glob_match(const char *pat, const char *name) {
    while (*pat && *name) {
        if (*pat == '*') {
            pat++;
            if (!*pat) {
                return 1;
            }
            for (const char *n = name; *n; n++) {
                if (glob_match(pat, n)) {
                    return 1;
                }
            }
            return 0;
        }
        if (*pat == '[') {
            const char *end = bracket_end(pat);
            if (end) {
                if (!bracket_has(pat, *name)) {
                    return 0;
                }
                pat = end;
                name++;
                continue;
            }
        }
        if (*pat == '\\' && pat[1]) {
            pat++;
            if (*pat != *name) {
                return 0;
            }
            pat++;
            name++;
            continue;
        }
        if (*pat != '?' && *pat != *name) {
            return 0;
        }
        pat++;
        name++;
    }
    while (*pat == '*') {
        pat++;
    }
    return !*pat && !*name;
}

static int has_glob(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '*' || *p == '?') {
            return 1;
        }
        if (*p == '[' && bracket_end(p)) {
            return 1;
        }
    }
    return 0;
}

static int str_cmp_qsort(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void glob_expand(const char *pat, Vec *out) {
    char *slash = strrchr(pat, '/');
    char dirbuf[PATH_MAX_LEN];
    const char *dir;
    const char *base;

    if (slash) {
        size_t n = (size_t)(slash - pat);
        if (n == 0) {
            dirbuf[0] = '/';
            dirbuf[1] = '\0';
        } else {
            if (n >= sizeof(dirbuf)) {
                n = sizeof(dirbuf) - 1;
            }
            memcpy(dirbuf, pat, n);
            dirbuf[n] = '\0';
        }
        dir = dirbuf;
        base = slash + 1;
    } else {
        dir = ".";
        base = pat;
    }

    if (has_glob(dir)) {
        vec_push(out, xstrdup(pat));
        return;
    }

    DIR *d = opendir(dir);
    if (!d) {
        vec_push(out, xstrdup(pat));
        return;
    }

    Vec hits;
    vec_init(&hits);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (e->d_name[0] == '.' && base[0] != '.') {
            continue;
        }
        if (!glob_match(base, e->d_name)) {
            continue;
        }
        Sbuf b;
        sb_init(&b);
        if (slash) {
            sb_putn(&b, pat, (size_t)(slash - pat) + 1);
        }
        sb_puts(&b, e->d_name);
        vec_push(&hits, b.p);
    }
    closedir(d);

    if (hits.n == 0) {
        vec_free(&hits);
        vec_push(out, xstrdup(pat));
        return;
    }
    qsort(hits.v, (size_t)hits.n, sizeof(char *), str_cmp_qsort);
    for (int i = 0; i < hits.n; i++) {
        vec_push(out, hits.v[i]);
    }
    free(hits.v);
}

enum { T_EOF, T_WORD, T_IONUM, T_OP };

typedef struct {
    int type;
    char *text;
    int quoted;
} Tok;

typedef struct Redir {
    struct Redir *next;
    struct Redir *here_next;
    int fd;
    int kind;
    char *word;
    int expand;
    int strip;
} Redir;

enum { RD_IN, RD_OUT, RD_APPEND, RD_DUP_OUT, RD_DUP_IN, RD_HEREDOC };

typedef struct {
    const char *src;
    size_t i;
    int incomplete;
    Redir *pending_here;
} Lexer;

static Lexer lx;

static int is_op_char(char c) {
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '\n';
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void gather_heredocs(void) {
    Redir *queue = lx.pending_here;
    lx.pending_here = 0;
    while (queue) {
        {
            Redir *h = queue;
            queue = h->here_next;
            h->here_next = 0;
            const char *delim = h->word;
            int strip_tabs = h->strip;

            Sbuf body;
            sb_init(&body);
            for (;;) {
                if (!lx.src[lx.i]) {
                    lx.incomplete = 1;
                    break;
                }
                size_t start = lx.i;
                while (lx.src[lx.i] && lx.src[lx.i] != '\n') {
                    lx.i++;
                }
                size_t len = lx.i - start;
                const char *line = lx.src + start;
                if (strip_tabs) {
                    while (len > 0 && *line == '\t') {
                        line++;
                        len--;
                    }
                }
                if (lx.src[lx.i] == '\n') {
                    lx.i++;
                }
                if (strlen(delim) == len && memcmp(line, delim, len) == 0) {
                    break;
                }
                sb_putn(&body, line, len);
                sb_putc(&body, '\n');
            }
            h->word = astrdup(body.p ? body.p : "");
            sb_free(&body);
        }
    }
}

static Tok lex_next(void) {
    Tok t;
    t.type = T_EOF;
    t.text = 0;
    t.quoted = 0;

    for (;;) {
        while (is_blank(lx.src[lx.i])) {
            lx.i++;
        }
        if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1] == '\n') {
            lx.i += 2;
            continue;
        }
        if (lx.src[lx.i] == '#') {
            while (lx.src[lx.i] && lx.src[lx.i] != '\n') {
                lx.i++;
            }
            continue;
        }
        break;
    }

    char c = lx.src[lx.i];
    if (!c) {
        return t;
    }

    if (c == '\n') {
        lx.i++;
        gather_heredocs();
        t.type = T_OP;
        t.text = astrdup("\n");
        return t;
    }

    if (is_op_char(c)) {
        const char *two[] = { "&&", "||", ";;", "<<", ">>", ">&", "<&", ">|", 0 };
        for (int i = 0; two[i]; i++) {
            if (c == two[i][0] && lx.src[lx.i + 1] == two[i][1]) {
                lx.i += 2;
                if (strcmp(two[i], "<<") == 0 && lx.src[lx.i] == '-') {
                    lx.i++;
                    t.type = T_OP;
                    t.text = astrdup("<<-");
                    return t;
                }
                t.type = T_OP;
                t.text = astrdup(two[i]);
                return t;
            }
        }
        lx.i++;
        char one[2] = { c, '\0' };
        t.type = T_OP;
        t.text = astrdup(one);
        return t;
    }

    Sbuf w;
    sb_init(&w);
    int quoted = 0;
    while ((c = lx.src[lx.i]) != '\0') {
        if (is_blank(c) || is_op_char(c)) {
            break;
        }
        if (c == '\\') {
            if (lx.src[lx.i + 1] == '\n') {
                lx.i += 2;
                continue;
            }
            if (!lx.src[lx.i + 1]) {
                lx.incomplete = 1;
                lx.i++;
                break;
            }
            sb_putc(&w, '\\');
            sb_putc(&w, lx.src[lx.i + 1]);
            lx.i += 2;
            quoted = 1;
            continue;
        }
        if (c == '\'') {
            quoted = 1;
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '\'') {
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '\'');
            lx.i++;
            continue;
        }
        if (c == '"') {
            quoted = 1;
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '"') {
                if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1]) {
                    sb_putc(&w, lx.src[lx.i]);
                    sb_putc(&w, lx.src[lx.i + 1]);
                    lx.i += 2;
                    continue;
                }
                if (lx.src[lx.i] == '`') {
                    sb_putc(&w, lx.src[lx.i++]);
                    while (lx.src[lx.i] && lx.src[lx.i] != '`') {
                        if (lx.src[lx.i] == '\\' && lx.src[lx.i + 1]) {
                            sb_putc(&w, lx.src[lx.i]);
                            lx.i++;
                        }
                        sb_putc(&w, lx.src[lx.i++]);
                    }
                    if (lx.src[lx.i]) {
                        sb_putc(&w, lx.src[lx.i++]);
                    }
                    continue;
                }
                if (lx.src[lx.i] == '$' && lx.src[lx.i + 1] == '(') {
                    int depth = 0;
                    sb_putc(&w, lx.src[lx.i++]);
                    for (;;) {
                        char d = lx.src[lx.i];
                        if (!d) {
                            break;
                        }
                        if (d == '(') {
                            depth++;
                        } else if (d == ')') {
                            depth--;
                        }
                        sb_putc(&w, d);
                        lx.i++;
                        if (depth == 0) {
                            break;
                        }
                    }
                    continue;
                }
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '"');
            lx.i++;
            continue;
        }
        if (c == '`') {
            sb_putc(&w, c);
            lx.i++;
            while (lx.src[lx.i] && lx.src[lx.i] != '`') {
                sb_putc(&w, lx.src[lx.i++]);
            }
            if (!lx.src[lx.i]) {
                lx.incomplete = 1;
                break;
            }
            sb_putc(&w, '`');
            lx.i++;
            continue;
        }
        if (c == '$' && (lx.src[lx.i + 1] == '(' || lx.src[lx.i + 1] == '{')) {
            char open = lx.src[lx.i + 1];
            char close = open == '(' ? ')' : '}';
            int depth = 0;
            sb_putc(&w, '$');
            lx.i++;
            for (;;) {
                char d = lx.src[lx.i];
                if (!d) {
                    lx.incomplete = 1;
                    break;
                }
                if (d == open) {
                    depth++;
                } else if (d == close) {
                    depth--;
                }
                sb_putc(&w, d);
                lx.i++;
                if (depth == 0) {
                    break;
                }
            }
            continue;
        }
        sb_putc(&w, c);
        lx.i++;
    }

    t.type = T_WORD;
    t.text = astrdup(w.p ? w.p : "");
    t.quoted = quoted;
    sb_free(&w);

    if (!quoted && t.text[0] >= '0' && t.text[0] <= '9') {
        int all_digits = 1;
        for (const char *p = t.text; *p; p++) {
            if (*p < '0' || *p > '9') {
                all_digits = 0;
                break;
            }
        }
        if (all_digits && (lx.src[lx.i] == '<' || lx.src[lx.i] == '>')) {
            t.type = T_IONUM;
        }
    }
    return t;
}

enum {
    N_SIMPLE, N_PIPE, N_AND, N_OR, N_SEQ, N_NOT, N_BG,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_SUBSHELL, N_GROUP, N_FUNC
};

typedef struct CaseItem {
    struct CaseItem *next;
    char **pats;
    int npats;
    struct Node *body;
} CaseItem;

typedef struct Node {
    int kind;
    struct Node *left, *right;
    struct Node *third;
    Redir *redirs;
    char **words;
    int nwords;
    char **assigns;
    int nassigns;
    char *name;
    CaseItem *cases;
} Node;

static Node *node_new(int kind) {
    Node *n = (Node *)arena_alloc(cur_arena, sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

static Tok tok;
static int parse_error;
static int parse_depth;

static void advance(void) {
    tok = lex_next();
}

static int tok_is_op(const char *s) {
    return tok.type == T_OP && strcmp(tok.text, s) == 0;
}

static int tok_is_word(const char *s) {
    return tok.type == T_WORD && !tok.quoted && strcmp(tok.text, s) == 0;
}

static void syntax(const char *what) {
    if (parse_error) {
        return;
    }
    parse_error = 1;
    if (tok.type == T_EOF) {
        lx.incomplete = 1;
        if (interactive) {
            return;
        }
    }
    errmsg("syntax error near ", what, 0);
}

static void skip_newlines(void) {
    while (tok_is_op("\n")) {
        advance();
    }
}

static Node *parse_list(const char *const *terminators);
static Node *parse_and_or(void);
static Node *parse_command(void);

static int is_terminator(const char *const *terms) {
    if (!terms) {
        return 0;
    }
    for (int i = 0; terms[i]; i++) {
        if (tok.type == T_WORD && !tok.quoted && strcmp(tok.text, terms[i]) == 0) {
            return 1;
        }
        if (tok.type == T_OP && strcmp(tok.text, terms[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static char *unquote(const char *raw) {
    Sbuf b;
    sb_init(&b);
    for (const char *p = raw; *p; p++) {
        if (*p == '\'' || *p == '"') {
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
        }
        sb_putc(&b, *p);
    }
    char *out = astrdup(b.p ? b.p : "");
    sb_free(&b);
    return out;
}

static Redir *parse_redirect(void) {
    int fd = -1;
    if (tok.type == T_IONUM) {
        fd = atoi(tok.text);
        advance();
    }
    if (tok.type != T_OP) {
        return 0;
    }
    int kind;
    if (strcmp(tok.text, "<") == 0) {
        kind = RD_IN;
    } else if (strcmp(tok.text, ">") == 0 || strcmp(tok.text, ">|") == 0) {
        kind = RD_OUT;
    } else if (strcmp(tok.text, ">>") == 0) {
        kind = RD_APPEND;
    } else if (strcmp(tok.text, ">&") == 0) {
        kind = RD_DUP_OUT;
    } else if (strcmp(tok.text, "<&") == 0) {
        kind = RD_DUP_IN;
    } else if (strcmp(tok.text, "<<") == 0 || strcmp(tok.text, "<<-") == 0) {
        kind = RD_HEREDOC;
    } else {
        return 0;
    }
    int strip = tok.type == T_OP && strcmp(tok.text, "<<-") == 0;
    advance();
    if (tok.type != T_WORD) {
        syntax("a redirect with no target");
        return 0;
    }

    Redir *r = (Redir *)arena_alloc(cur_arena, sizeof(Redir));
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->word = tok.text;
    r->strip = strip;
    r->fd = fd >= 0 ? fd : (kind == RD_IN || kind == RD_HEREDOC || kind == RD_DUP_IN ? 0 : 1);
    if (kind == RD_HEREDOC) {
        r->expand = !tok.quoted;
        r->word = unquote(r->word);
        r->here_next = 0;
        Redir **tail = &lx.pending_here;
        while (*tail) {
            tail = &(*tail)->here_next;
        }
        *tail = r;
    }
    advance();
    return r;
}

static void append_redir(Node *n, Redir *r) {
    Redir **pp = &n->redirs;
    while (*pp) {
        pp = &(*pp)->next;
    }
    *pp = r;
}

static char **words_push(char **arr, int *n, char *w) {
    char **v = (char **)arena_alloc(cur_arena, sizeof(char *) * (size_t)(*n + 2));
    for (int i = 0; i < *n; i++) {
        v[i] = arr[i];
    }
    v[*n] = w;
    v[*n + 1] = 0;
    (*n)++;
    return v;
}

static int is_assignment(const char *s) {
    if (!s[0] || (s[0] >= '0' && s[0] <= '9')) {
        return 0;
    }
    for (const char *p = s; *p; p++) {
        if (*p == '=') {
            return p != s;
        }
        if (!is_name_char(*p)) {
            return 0;
        }
    }
    return 0;
}

static Node *parse_simple(void) {
    Node *n = node_new(N_SIMPLE);
    int leading = 1;
    for (;;) {
        if (tok.type == T_IONUM ||
            (tok.type == T_OP && (strcmp(tok.text, "<") == 0 || strcmp(tok.text, ">") == 0 ||
                                  strcmp(tok.text, ">>") == 0 || strcmp(tok.text, "<<") == 0 ||
                                  strcmp(tok.text, "<<-") == 0 || strcmp(tok.text, ">&") == 0 ||
                                  strcmp(tok.text, "<&") == 0 || strcmp(tok.text, ">|") == 0))) {
            Redir *r = parse_redirect();
            if (!r) {
                return n;
            }
            append_redir(n, r);
            continue;
        }
        if (tok.type != T_WORD) {
            break;
        }
        if (leading && is_assignment(tok.text)) {
            n->assigns = words_push(n->assigns, &n->nassigns, tok.text);
            advance();
            continue;
        }
        leading = 0;
        n->words = words_push(n->words, &n->nwords, tok.text);
        advance();
    }
    if (n->nwords == 0 && n->nassigns == 0 && !n->redirs) {
        return 0;
    }
    return n;
}

static Node *parse_if(void) {
    advance();
    static const char *const then_term[] = { "then", 0 };
    Node *n = node_new(N_IF);
    n->left = parse_list(then_term);
    if (!tok_is_word("then")) {
        syntax("`if` with no `then`");
        return n;
    }
    advance();
    static const char *const body_term[] = { "elif", "else", "fi", 0 };
    n->right = parse_list(body_term);
    if (tok_is_word("elif")) {
        n->third = parse_if();
        return n;
    }
    if (tok_is_word("else")) {
        advance();
        static const char *const else_term[] = { "fi", 0 };
        n->third = parse_list(else_term);
    }
    if (!tok_is_word("fi")) {
        syntax("`if` with no `fi`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_while(int until) {
    advance();
    Node *n = node_new(until ? N_UNTIL : N_WHILE);
    static const char *const do_term[] = { "do", 0 };
    n->left = parse_list(do_term);
    if (!tok_is_word("do")) {
        syntax("a loop with no `do`");
        return n;
    }
    advance();
    static const char *const done_term[] = { "done", 0 };
    n->right = parse_list(done_term);
    if (!tok_is_word("done")) {
        syntax("a loop with no `done`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_for(void) {
    advance();
    if (tok.type != T_WORD) {
        syntax("`for` with no variable");
        return 0;
    }
    Node *n = node_new(N_FOR);
    n->name = tok.text;
    advance();
    skip_newlines();
    if (tok_is_word("in")) {
        advance();
        while (tok.type == T_WORD) {
            n->words = words_push(n->words, &n->nwords, tok.text);
            advance();
        }
    } else {
        n->words = words_push(n->words, &n->nwords, astrdup("\"$@\""));
    }
    while (tok_is_op(";") || tok_is_op("\n")) {
        advance();
    }
    if (!tok_is_word("do")) {
        syntax("`for` with no `do`");
        return n;
    }
    advance();
    static const char *const done_term[] = { "done", 0 };
    n->right = parse_list(done_term);
    if (!tok_is_word("done")) {
        syntax("`for` with no `done`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_case(void) {
    advance();
    if (tok.type != T_WORD) {
        syntax("`case` with nothing to match");
        return 0;
    }
    Node *n = node_new(N_CASE);
    n->name = tok.text;
    advance();
    skip_newlines();
    if (!tok_is_word("in")) {
        syntax("`case` with no `in`");
        return n;
    }
    advance();
    skip_newlines();

    CaseItem **tail = &n->cases;
    while (!tok_is_word("esac") && tok.type != T_EOF && !parse_error) {
        CaseItem *it = (CaseItem *)arena_alloc(cur_arena, sizeof(CaseItem));
        memset(it, 0, sizeof(*it));
        if (tok_is_op("(")) {
            advance();
        }
        for (;;) {
            if (tok.type != T_WORD) {
                syntax("a `case` branch with no pattern");
                return n;
            }
            it->pats = words_push(it->pats, &it->npats, tok.text);
            advance();
            if (tok_is_op("|")) {
                advance();
                continue;
            }
            break;
        }
        if (!tok_is_op(")")) {
            syntax("a `case` pattern with no `)`");
            return n;
        }
        advance();
        static const char *const item_term[] = { ";;", "esac", 0 };
        it->body = parse_list(item_term);
        if (tok_is_op(";;")) {
            advance();
        }
        skip_newlines();
        *tail = it;
        tail = &it->next;
    }
    if (!tok_is_word("esac")) {
        syntax("`case` with no `esac`");
        return n;
    }
    advance();
    return n;
}

static Node *parse_command(void) {
    if (++parse_depth > SH_MAX_DEPTH) {
        syntax("something nested too deeply");
        parse_depth--;
        return 0;
    }
    Node *n = 0;

    if (tok_is_word("if")) {
        n = parse_if();
    } else if (tok_is_word("while")) {
        n = parse_while(0);
    } else if (tok_is_word("until")) {
        n = parse_while(1);
    } else if (tok_is_word("for")) {
        n = parse_for();
    } else if (tok_is_word("case")) {
        n = parse_case();
    } else if (tok_is_op("(")) {
        advance();
        static const char *const close[] = { ")", 0 };
        n = node_new(N_SUBSHELL);
        n->left = parse_list(close);
        if (!tok_is_op(")")) {
            syntax("a subshell with no `)`");
        } else {
            advance();
        }
    } else if (tok_is_word("{")) {
        advance();
        static const char *const close[] = { "}", 0 };
        n = node_new(N_GROUP);
        n->left = parse_list(close);
        if (!tok_is_word("}")) {
            syntax("a group with no `}`");
        } else {
            advance();
        }
    } else if (tok.type == T_WORD) {
        char *maybe_name = tok.text;
        int was_quoted = tok.quoted;
        size_t save_i = lx.i;
        Tok save_tok = tok;
        size_t peek = lx.i;
        while (is_blank(lx.src[peek])) {
            peek++;
        }
        int looks_like_func = !was_quoted && lx.src[peek] == '(';
        if (looks_like_func) {
            advance();
        }
        if (looks_like_func && tok_is_op("(")) {
            advance();
            if (!tok_is_op(")")) {
                syntax("a function definition with no `)`");
                parse_depth--;
                return 0;
            }
            advance();
            skip_newlines();
            n = node_new(N_FUNC);
            n->name = maybe_name;
            n->left = parse_command();
            cur_arena->retained = 1;
            parse_depth--;
            return n;
        }
        lx.i = save_i;
        tok = save_tok;
        n = parse_simple();
    } else {
        n = parse_simple();
    }

    if (n && n->kind != N_SIMPLE) {
        for (;;) {
            if (tok.type == T_IONUM ||
                (tok.type == T_OP && (strcmp(tok.text, "<") == 0 || strcmp(tok.text, ">") == 0 ||
                                      strcmp(tok.text, ">>") == 0 || strcmp(tok.text, "<<") == 0 ||
                                      strcmp(tok.text, "<<-") == 0 || strcmp(tok.text, ">&") == 0 ||
                                      strcmp(tok.text, "<&") == 0))) {
                Redir *r = parse_redirect();
                if (!r) {
                    break;
                }
                append_redir(n, r);
                continue;
            }
            break;
        }
    }
    parse_depth--;
    return n;
}

static Node *parse_pipeline(void) {
    int negate = 0;
    while (tok_is_word("!")) {
        negate = !negate;
        advance();
    }
    Node *n = parse_command();
    while (tok_is_op("|")) {
        advance();
        skip_newlines();
        Node *p = node_new(N_PIPE);
        p->left = n;
        p->right = parse_command();
        if (!p->right && !parse_error) {
            syntax("a pipe with nothing after it");
        }
        n = p;
    }
    if (negate) {
        Node *not_node = node_new(N_NOT);
        not_node->left = n;
        n = not_node;
    }
    return n;
}

static Node *parse_and_or(void) {
    Node *n = parse_pipeline();
    for (;;) {
        int kind;
        if (tok_is_op("&&")) {
            kind = N_AND;
        } else if (tok_is_op("||")) {
            kind = N_OR;
        } else {
            return n;
        }
        advance();
        skip_newlines();
        Node *p = node_new(kind);
        p->left = n;
        p->right = parse_pipeline();
        if (!p->right && !parse_error) {
            syntax("`&&` or `||` with nothing after it");
        }
        n = p;
    }
}

static Node *parse_list(const char *const *terminators) {
    Node *head = 0;
    for (;;) {
        skip_newlines();
        if (tok.type == T_EOF || is_terminator(terminators) || parse_error) {
            return head;
        }
        Node *n = parse_and_or();
        if (!n) {
            if (!parse_error && tok.type != T_EOF && !is_terminator(terminators)) {
                syntax(tok.text ? tok.text : "end of input");
            }
            return head;
        }
        if (tok_is_op("&")) {
            Node *bg = node_new(N_BG);
            bg->left = n;
            n = bg;
            advance();
        } else if (tok_is_op(";")) {
            advance();
        }
        if (!head) {
            head = n;
        } else {
            Node *seq = node_new(N_SEQ);
            seq->left = head;
            seq->right = n;
            head = seq;
        }
    }
}

static int exec_node(Node *n);
static int exec_text(const char *text);

typedef struct {
    Sbuf cur;
    int started;
    int quoted_here;
    Vec *out;
    int split;
    int glob;
    int at_killed;
} Ex;

static void ex_flush(Ex *e) {
    if (e->at_killed && !e->cur.p) {
        e->started = 0;
        e->quoted_here = 0;
        e->at_killed = 0;
        return;
    }
    e->at_killed = 0;
    if (!e->started) {
        return;
    }
    char *field = e->cur.p ? e->cur.p : xstrdup("");
    if (!e->cur.p) {
        vec_push(e->out, field);
    } else if (e->glob && !opt_noglob && !e->quoted_here && has_glob(field)) {
        glob_expand(field, e->out);
        free(field);
    } else {
        vec_push(e->out, field);
    }
    sb_init(&e->cur);
    e->started = 0;
    e->quoted_here = 0;
}

static void ex_add(Ex *e, const char *s, size_t n) {
    if (n) {
        e->at_killed = 0;
    }
    sb_putn(&e->cur, s, n);
    e->started = 1;
}

static const char *ifs_chars(void) {
    const char *v = var_get("IFS");
    return v[0] ? v : " \t\n";
}

static void ex_add_split(Ex *e, const char *s) {
    if (!e->split) {
        ex_add(e, s, strlen(s));
        return;
    }
    const char *ifs = ifs_chars();
    for (const char *p = s; *p; p++) {
        if (strchr(ifs, *p)) {
            ex_flush(e);
            continue;
        }
        ex_add(e, p, 1);
    }
}

static char *capture_command(const char *text) {
    int fds[2];
    if (pipe(fds) != 0) {
        errmsg("cannot create a pipe for $( )", 0, 0);
        return xstrdup("");
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        errmsg("cannot fork for $( )", 0, 0);
        return xstrdup("");
    }
    if (pid == 0) {
        close(fds[0]);
        if (fds[1] != 1) {
            dup2(fds[1], 1);
            close(fds[1]);
        }
        int st = exec_text(text);
        _exit(st);
    }
    close(fds[1]);

    Sbuf b;
    sb_init(&b);
    char buf[512];
    long n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        sb_putn(&b, buf, (size_t)n);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    while (b.len > 0 && b.p[b.len - 1] == '\n') {
        b.p[--b.len] = '\0';
    }
    return b.p ? b.p : xstrdup("");
}

static char *param_value(const char *name) {
    if (strcmp(name, "?") == 0) {
        return xstrdup(num_to_str(last_status));
    }
    if (strcmp(name, "$") == 0) {
        return xstrdup(num_to_str(shell_pid));
    }
    if (strcmp(name, "#") == 0) {
        return xstrdup(num_to_str(pos_count));
    }
    if (strcmp(name, "0") == 0) {
        return xstrdup(script_name);
    }
    if (name[0] >= '1' && name[0] <= '9') {
        int idx = atoi(name);
        if (idx >= 1 && idx <= pos_count) {
            return xstrdup(pos_params[idx - 1]);
        }
        return xstrdup("");
    }
    if (strcmp(name, "*") == 0 || strcmp(name, "@") == 0) {
        Sbuf b;
        sb_init(&b);
        for (int i = 0; i < pos_count; i++) {
            if (i) {
                sb_putc(&b, ' ');
            }
            sb_puts(&b, pos_params[i]);
        }
        return b.p ? b.p : xstrdup("");
    }
    return xstrdup(var_get(name));
}

static char *strip_affix(const char *value, const char *pat, int from_end, int longest) {
    size_t n = strlen(value);
    Sbuf b;
    sb_init(&b);
    for (size_t step = 0; step <= n; step++) {
        size_t len = longest ? n - step : step;
        char *piece;
        if (from_end) {
            piece = xstrdup(value + (n - len));
        } else {
            piece = (char *)malloc(len + 1);
            if (!piece) {
                die_oom();
            }
            memcpy(piece, value, len);
            piece[len] = '\0';
        }
        int hit = glob_match(pat, piece);
        free(piece);
        if (hit) {
            if (from_end) {
                sb_putn(&b, value, n - len);
            } else {
                sb_puts(&b, value + len);
            }
            return b.p ? b.p : xstrdup("");
        }
    }
    return xstrdup(value);
}

static void expand_dollar(Ex *e, const char **pp, int in_quotes);
static void expand_word(const char *raw, Vec *out, int split, int do_glob);

static void expand_braced_word(Ex *e, const char *word, int in_quotes) {
    Vec wv;
    vec_init(&wv);
    expand_word(word, &wv, !in_quotes, 0);
    for (int i = 0; i < wv.n; i++) {
        if (i) {
            ex_flush(e);
        }
        ex_add(e, wv.v[i], strlen(wv.v[i]));
        if (in_quotes) {
            e->quoted_here = 1;
        }
    }
    if (wv.n == 0) {
        e->at_killed = 1;
    }
    vec_free(&wv);
}

static void expand_braced(Ex *e, const char *body, int in_quotes) {
    if (body[0] == '#' && body[1] && !strchr(":-+=?", body[1])) {
        char *v = param_value(body + 1);
        char *len = xstrdup(num_to_str((long)strlen(v)));
        free(v);
        ex_add(e, len, strlen(len));
        free(len);
        return;
    }

    size_t i = 0;
    while (body[i] && (is_name_char(body[i]) || (i == 0 && strchr("?$#*@", body[i])))) {
        if (i == 0 && strchr("?$#*@", body[i])) {
            i++;
            break;
        }
        i++;
    }
    char *name = xstrdup(body);
    name[i] = '\0';
    const char *op = body + i;

    if (!*op) {
        char *v = param_value(name);
        if (strcmp(name, "@") == 0 && in_quotes) {
            free(v);
            free(name);
            for (int p = 0; p < pos_count; p++) {
                if (p) {
                    ex_flush(e);
                }
                ex_add(e, pos_params[p], strlen(pos_params[p]));
                e->quoted_here = 1;
            }
            if (pos_count == 0) {
                e->at_killed = 1;
            }
            return;
        }
        if (in_quotes) {
            ex_add(e, v, strlen(v));
        } else {
            ex_add_split(e, v);
        }
        free(v);
        free(name);
        return;
    }

    int colon = 0;
    if (*op == ':') {
        colon = 1;
        op++;
    }
    char kind = *op;
    const char *word = op + 1;
    if (kind == '#' || kind == '%') {
        int longest = (word[0] == kind);
        if (longest) {
            word++;
        }
        char *v = param_value(name);
        Vec pv;
        vec_init(&pv);
        expand_word(word, &pv, 0, 0);
        char *result = strip_affix(v, pv.n ? pv.v[0] : "", kind == '%', longest);
        vec_free(&pv);
        free(v);
        if (in_quotes) {
            ex_add(e, result, strlen(result));
        } else {
            ex_add_split(e, result);
        }
        free(result);
        free(name);
        return;
    }

    char *v = param_value(name);
    int unset_or_empty = colon ? (v[0] == '\0') : !var_is_set(name);
    if (name[0] >= '1' && name[0] <= '9') {
        unset_or_empty = colon ? (v[0] == '\0') : (atoi(name) > pos_count);
    } else if (name[1] == '\0' && strchr("*@#?$0", name[0])) {
        if (name[0] == '*' || name[0] == '@') {
            unset_or_empty = colon ? (v[0] == '\0') : (pos_count == 0);
        } else {
            unset_or_empty = colon ? (v[0] == '\0') : 0;
        }
    }

    if (kind == '-' || kind == '=' || kind == '+' || kind == '?') {
        int use_word = (kind == '+') ? !unset_or_empty : unset_or_empty;
        if (kind == '?' && unset_or_empty) {
            errmsg(name, ": ", word[0] ? word : "parameter null or not set");
            free(v);
            free(name);
            flow = FLOW_EXIT;
            exit_code = 1;
            return;
        }
        if (use_word) {
            if (kind == '=') {
                Vec wv;
                vec_init(&wv);
                expand_word(word, &wv, 0, 0);
                char *val = xstrdup(wv.n ? wv.v[0] : "");
                vec_free(&wv);
                var_set(name, val);
                if (in_quotes) {
                    ex_add(e, val, strlen(val));
                } else {
                    ex_add_split(e, val);
                }
                free(val);
            } else {
                expand_braced_word(e, word, in_quotes);
            }
        } else if (kind != '+') {
            if (in_quotes) {
                ex_add(e, v, strlen(v));
            } else {
                ex_add_split(e, v);
            }
        }
        free(v);
        free(name);
        return;
    }

    if (in_quotes) {
        ex_add(e, v, strlen(v));
    } else {
        ex_add_split(e, v);
    }
    free(v);
    free(name);
}

typedef struct {
    const char *p;
    int error;
} Arith;

static long arith_expr(Arith *a);
static char *expand_one(const char *raw);

static void arith_skip(Arith *a) {
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') {
        a->p++;
    }
}

static int arith_eat(Arith *a, const char *op, const char *longer) {
    arith_skip(a);
    size_t n = strlen(op);
    if (strncmp(a->p, op, n) != 0) {
        return 0;
    }
    if (longer && strncmp(a->p, longer, strlen(longer)) == 0) {
        return 0;
    }
    a->p += n;
    return 1;
}

static long arith_primary(Arith *a) {
    arith_skip(a);
    if (*a->p == '(') {
        a->p++;
        long v = arith_expr(a);
        arith_skip(a);
        if (*a->p == ')') {
            a->p++;
        } else {
            a->error = 1;
        }
        return v;
    }
    if (*a->p == '-') {
        a->p++;
        return -arith_primary(a);
    }
    if (*a->p == '+') {
        a->p++;
        return arith_primary(a);
    }
    if (*a->p == '!') {
        a->p++;
        return !arith_primary(a);
    }
    if (*a->p == '~') {
        a->p++;
        return ~arith_primary(a);
    }
    if (*a->p >= '0' && *a->p <= '9') {
        char *end = 0;
        long v = strtol(a->p, &end, 0);
        if (end == a->p) {
            a->error = 1;
            return 0;
        }
        a->p = end;
        return v;
    }
    if (is_name_char(*a->p) && !(*a->p >= '0' && *a->p <= '9')) {
        const char *start = a->p;
        while (is_name_char(*a->p)) {
            a->p++;
        }
        char *name = astrndup(start, (size_t)(a->p - start));
        const char *val = var_get(name);
        if (!val || !*val) {
            return 0;
        }
        Arith inner;
        inner.p = val;
        inner.error = 0;
        long v = arith_expr(&inner);
        if (inner.error) {
            a->error = 1;
        }
        return v;
    }
    a->error = 1;
    return 0;
}

static long arith_mul(Arith *a) {
    long v = arith_primary(a);
    for (;;) {
        if (arith_eat(a, "*", 0)) {
            v = v * arith_primary(a);
        } else if (arith_eat(a, "/", 0)) {
            long d = arith_primary(a);
            if (d == 0) {
                a->error = 1;
                return 0;
            }
            v = v / d;
        } else if (arith_eat(a, "%", 0)) {
            long d = arith_primary(a);
            if (d == 0) {
                a->error = 1;
                return 0;
            }
            v = v % d;
        } else {
            return v;
        }
    }
}

static long arith_add(Arith *a) {
    long v = arith_mul(a);
    for (;;) {
        if (arith_eat(a, "+", 0)) {
            v = v + arith_mul(a);
        } else if (arith_eat(a, "-", 0)) {
            v = v - arith_mul(a);
        } else {
            return v;
        }
    }
}

static long arith_shift(Arith *a) {
    long v = arith_add(a);
    for (;;) {
        if (arith_eat(a, "<<", 0)) {
            v = v << arith_add(a);
        } else if (arith_eat(a, ">>", 0)) {
            v = v >> arith_add(a);
        } else {
            return v;
        }
    }
}

static long arith_rel(Arith *a) {
    long v = arith_shift(a);
    for (;;) {
        if (arith_eat(a, "<=", 0)) {
            v = (v <= arith_shift(a));
        } else if (arith_eat(a, ">=", 0)) {
            v = (v >= arith_shift(a));
        } else if (arith_eat(a, "<", "<<")) {
            v = (v < arith_shift(a));
        } else if (arith_eat(a, ">", ">>")) {
            v = (v > arith_shift(a));
        } else {
            return v;
        }
    }
}

static long arith_eq(Arith *a) {
    long v = arith_rel(a);
    for (;;) {
        if (arith_eat(a, "==", 0)) {
            v = (v == arith_rel(a));
        } else if (arith_eat(a, "!=", 0)) {
            v = (v != arith_rel(a));
        } else {
            return v;
        }
    }
}

static long arith_band(Arith *a) {
    long v = arith_eq(a);
    while (arith_eat(a, "&", "&&")) {
        v = v & arith_eq(a);
    }
    return v;
}

static long arith_bxor(Arith *a) {
    long v = arith_band(a);
    while (arith_eat(a, "^", 0)) {
        v = v ^ arith_band(a);
    }
    return v;
}

static long arith_bor(Arith *a) {
    long v = arith_bxor(a);
    while (arith_eat(a, "|", "||")) {
        v = v | arith_bxor(a);
    }
    return v;
}

static long arith_land(Arith *a) {
    long v = arith_bor(a);
    while (arith_eat(a, "&&", 0)) {
        long r = arith_bor(a);
        v = (v && r);
    }
    return v;
}

static long arith_lor(Arith *a) {
    long v = arith_land(a);
    while (arith_eat(a, "||", 0)) {
        long r = arith_land(a);
        v = (v || r);
    }
    return v;
}

static long arith_expr(Arith *a) {
    return arith_lor(a);
}

static char *arith_eval(const char *text) {
    Arith a;
    a.p = text;
    a.error = 0;
    long v = arith_expr(&a);
    arith_skip(&a);
    if (*a.p) {
        a.error = 1;
    }
    if (a.error) {
        errmsg("arithmetic syntax error: ", text, 0);
        v = 0;
    }
    return xstrdup(num_to_str(v));
}

static void expand_dollar(Ex *e, const char **pp, int in_quotes) {
    const char *p = *pp + 1;
    if (p[0] == '(' && p[1] == '(') {
        const char *start = p + 2;
        const char *q = start;
        int depth = 1;
        while (*q) {
            if (*q == '(') {
                depth++;
            } else if (*q == ')') {
                if (--depth == 0) {
                    break;
                }
            }
            q++;
        }
        if (depth != 0 || q[0] != ')' || q[1] != ')') {
        } else {
            size_t len = (size_t)(q - start);
            char *raw = (char *)malloc(len + 1);
            if (!raw) {
                die_oom();
            }
            memcpy(raw, start, len);
            raw[len] = '\0';
            char *expanded = expand_one(raw);
            free(raw);
            char *result = arith_eval(expanded ? expanded : "");
            free(expanded);
            ex_add(e, result, strlen(result));
            free(result);
            *pp = q + 2;
            return;
        }
    }
    if (*p == '(') {
        int depth = 0;
        const char *start = p + 1;
        const char *q = p;
        for (; *q; q++) {
            if (*q == '(') {
                depth++;
            } else if (*q == ')') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        size_t len = (size_t)(q - start);
        char *text = (char *)malloc(len + 1);
        if (!text) {
            die_oom();
        }
        memcpy(text, start, len);
        text[len] = '\0';
        char *result = capture_command(text);
        free(text);
        if (in_quotes) {
            ex_add(e, result, strlen(result));
        } else {
            ex_add_split(e, result);
        }
        free(result);
        *pp = *q ? q + 1 : q;
        return;
    }
    if (*p == '{') {
        int depth = 0;
        const char *start = p + 1;
        const char *q = p;
        for (; *q; q++) {
            if (*q == '{') {
                depth++;
            } else if (*q == '}') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        size_t len = (size_t)(q - start);
        char *body = (char *)malloc(len + 1);
        if (!body) {
            die_oom();
        }
        memcpy(body, start, len);
        body[len] = '\0';
        expand_braced(e, body, in_quotes);
        free(body);
        *pp = *q ? q + 1 : q;
        return;
    }
    if (is_name_char(*p) || strchr("?$#*@", *p)) {
        char name[64];
        size_t n = 0;
        if (strchr("?$#*@", *p) || (*p >= '0' && *p <= '9')) {
            name[n++] = *p++;
        } else {
            while (*p && is_name_char(*p) && n < sizeof(name) - 1) {
                name[n++] = *p++;
            }
        }
        name[n] = '\0';
        if (strcmp(name, "@") == 0 && in_quotes) {
            for (int i = 0; i < pos_count; i++) {
                if (i) {
                    ex_flush(e);
                }
                ex_add(e, pos_params[i], strlen(pos_params[i]));
                e->quoted_here = 1;
            }
            if (pos_count == 0) {
                e->at_killed = 1;
            }
            *pp = p;
            return;
        }
        char *v = param_value(name);
        if (in_quotes) {
            ex_add(e, v, strlen(v));
        } else {
            ex_add_split(e, v);
        }
        free(v);
        *pp = p;
        return;
    }
    ex_add(e, "$", 1);
    *pp = p;
}

static void expand_word(const char *raw, Vec *out, int split, int do_glob) {
    Ex e;
    memset(&e, 0, sizeof(e));
    sb_init(&e.cur);
    e.out = out;
    e.split = split;
    e.glob = do_glob;

    const char *p = raw;
    while (*p) {
        if (*p == '\\') {
            if (p[1]) {
                ex_add(&e, p + 1, 1);
                e.quoted_here = 1;
                p += 2;
                continue;
            }
            p++;
            continue;
        }
        if (*p == '\'') {
            p++;
            e.started = 1;
            e.quoted_here = 1;
            while (*p && *p != '\'') {
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '"') {
            p++;
            e.started = 1;
            e.quoted_here = 1;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1] == '\n') {
                    p += 2;
                    continue;
                }
                if (*p == '\\' && p[1] && strchr("$`\"\\", p[1])) {
                    ex_add(&e, p + 1, 1);
                    p += 2;
                    continue;
                }
                if (*p == '$') {
                    expand_dollar(&e, &p, 1);
                    continue;
                }
                if (*p == '`') {
                    const char *q = ++p;
                    while (*q && *q != '`') {
                        q++;
                    }
                    char *text = (char *)malloc((size_t)(q - p) + 1);
                    if (!text) {
                        die_oom();
                    }
                    memcpy(text, p, (size_t)(q - p));
                    text[q - p] = '\0';
                    char *result = capture_command(text);
                    free(text);
                    ex_add(&e, result, strlen(result));
                    free(result);
                    p = *q ? q + 1 : q;
                    continue;
                }
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '$') {
            expand_dollar(&e, &p, 0);
            continue;
        }
        if (*p == '`') {
            const char *q = ++p;
            while (*q && *q != '`') {
                q++;
            }
            char *text = (char *)malloc((size_t)(q - p) + 1);
            if (!text) {
                die_oom();
            }
            memcpy(text, p, (size_t)(q - p));
            text[q - p] = '\0';
            char *result = capture_command(text);
            free(text);
            ex_add_split(&e, result);
            free(result);
            p = *q ? q + 1 : q;
            continue;
        }
        if (split && strchr(ifs_chars(), *p)) {
            ex_flush(&e);
            p++;
            continue;
        }
        ex_add(&e, p, 1);
        p++;
    }
    ex_flush(&e);
}

static char *expand_one(const char *raw) {
    Vec v;
    vec_init(&v);
    expand_word(raw, &v, 0, 0);
    char *s = v.n ? xstrdup(v.v[0]) : xstrdup("");
    vec_free(&v);
    return s;
}

static char *expand_pattern(const char *raw) {
    Vec v;
    vec_init(&v);
    Ex e;
    memset(&e, 0, sizeof(e));
    sb_init(&e.cur);
    e.out = &v;
    e.split = 0;
    e.glob = 0;
    const char *p = raw;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            e.started = 1;
            while (*p && *p != q) {
                ex_add(&e, p, 1);
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        if (*p == '$') {
            expand_dollar(&e, &p, 1);
            continue;
        }
        ex_add(&e, p, 1);
        p++;
    }
    ex_flush(&e);
    char *s = v.n ? xstrdup(v.v[0]) : xstrdup("");
    vec_free(&v);
    return s;
}

#define SH_SAVE_BASE 20
#define SH_MAX_REDIR 16

typedef struct {
    int n;
    int fd[SH_MAX_REDIR];
    int saved[SH_MAX_REDIR];
    pid_t here[SH_MAX_REDIR];
    int nhere;
} RedirSave;

static int heredoc_fd(Redir *r, RedirSave *save) {
    Sbuf text;
    sb_init(&text);
    if (r->expand) {
        Ex e;
        Vec v;
        vec_init(&v);
        memset(&e, 0, sizeof(e));
        sb_init(&e.cur);
        e.out = &v;
        e.split = 0;
        e.glob = 0;
        const char *p = r->word;
        while (*p) {
            if (*p == '\\' && p[1] == '\n') {
                p += 2;
                continue;
            }
            if (*p == '\\' && p[1] && strchr("$`\\", p[1])) {
                ex_add(&e, p + 1, 1);
                p += 2;
                continue;
            }
            if (*p == '$') {
                expand_dollar(&e, &p, 1);
                continue;
            }
            if (*p == '`') {
                const char *q = ++p;
                while (*q && *q != '`') {
                    if (*q == '\\' && q[1]) {
                        q++;
                    }
                    q++;
                }
                char *body = astrndup(p, (size_t)(q - p));
                char *result = capture_command(body);
                ex_add(&e, result, strlen(result));
                free(result);
                p = *q ? q + 1 : q;
                continue;
            }
            ex_add(&e, p, 1);
            p++;
        }
        e.started = 1;
        ex_flush(&e);
        sb_puts(&text, v.n ? v.v[0] : "");
        vec_free(&v);
    } else {
        sb_puts(&text, r->word);
    }
    int fds[2];
    if (pipe(fds) != 0) {
        sb_free(&text);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        sb_free(&text);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        size_t off = 0;
        while (off < text.len) {
            long n = write(fds[1], text.p + off, text.len - off);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    sb_free(&text);
    if (save && save->nhere < SH_MAX_REDIR) {
        save->here[save->nhere++] = pid;
    }
    return fds[0];
}

static int park_slot(int fd) {
    for (int slot = SH_SAVE_BASE; slot < SH_SAVE_BASE + 64; slot++) {
        if (fcntl(slot, F_GETFD) >= 0) {
            continue;
        }
        if (dup2(fd, slot) == slot) {
            return slot;
        }
        return -1;
    }
    return -1;
}

static int apply_redirs(Redir *list, RedirSave *save) {
    if (save) {
        save->n = 0;
        save->nhere = 0;
    }
    for (Redir *r = list; r; r = r->next) {
        int target = -1;
        if (save && save->n < SH_MAX_REDIR) {
            save->fd[save->n] = r->fd;
            save->saved[save->n] = park_slot(r->fd);
            save->n++;
        }
        if (r->kind == RD_HEREDOC) {
            target = heredoc_fd(r, save);
            if (target < 0) {
                errmsg("cannot set up a here-document", 0, 0);
                return -1;
            }
        } else if (r->kind == RD_DUP_OUT || r->kind == RD_DUP_IN) {
            char *w = expand_one(r->word);
            if (strcmp(w, "-") == 0) {
                free(w);
                close(r->fd);
                continue;
            }
            target = atoi(w);
            free(w);
            int dupd = dup2(target, r->fd);
            if (dupd < 0) {
                errmsg("cannot duplicate descriptor ", num_to_str(target), 0);
                return -1;
            }
            continue;
        } else {
            char *path = expand_one(r->word);
            int flags = 0;
            if (r->kind == RD_IN) {
                flags = O_RDONLY;
            } else if (r->kind == RD_OUT) {
                flags = O_WRONLY | O_CREAT | O_TRUNC;
            } else {
                flags = O_WRONLY | O_CREAT | O_APPEND;
            }
            target = open(path, flags, 0644);
            if (target < 0) {
                char why[48];
                why[0] = '\0';
                if (errno) {
                    strcpy(why, ": ");
                    strncat(why, strerror(errno), sizeof(why) - 3);
                }
                errmsg("cannot open ", path, why[0] ? why : 0);
                free(path);
                return -1;
            }
            free(path);
        }

        if (target != r->fd) {
            dup2(target, r->fd);
            close(target);
        }
    }
    return 0;
}

static void undo_redirs(RedirSave *save) {
    for (int i = save->n - 1; i >= 0; i--) {
        if (save->saved[i] >= 0) {
            dup2(save->saved[i], save->fd[i]);
            close(save->saved[i]);
        } else {
            close(save->fd[i]);
        }
    }
    save->n = 0;
    for (int i = 0; i < save->nhere; i++) {
        int st;
        waitpid(save->here[i], &st, 0);
    }
    save->nhere = 0;
}

typedef struct Func {
    struct Func *next;
    char *name;
    Node *body;
} Func;

static Func *funcs;

static Func *func_find(const char *name) {
    for (Func *f = funcs; f; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return 0;
}

static void func_define(const char *name, Node *body) {
    Func *f = func_find(name);
    if (!f) {
        f = (Func *)malloc(sizeof(Func));
        if (!f) {
            die_oom();
        }
        f->name = xstrdup(name);
        f->next = funcs;
        funcs = f;
    }
    f->body = body;
}

static int background_pids[64];
static int nbackground;

static int bi_test(int argc, char **argv);

static int is_builtin(const char *name) {
    static const char *const names[] = {
        "cd", "pwd", "exit", "echo", "export", "unset", "set", "shift",
        "read", "test", "[", "true", "false", ":", "return", "break",
        "continue", ".", "source", "eval", "wait", "env", "unalias",
        "exec", "trap", 0
    };
    for (int i = 0; names[i]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int read_line_fd(int fd, Sbuf *out) {
    char c;
    int got = 0;
    for (;;) {
        long n = read(fd, &c, 1);
        if (n <= 0) {
            return got ? 1 : 0;
        }
        got = 1;
        if (c == '\n') {
            return 1;
        }
        sb_putc(out, c);
    }
}

#define TRAP_MAX_SIG 32

static char *trap_action[TRAP_MAX_SIG];
static char *trap_exit_action;
static volatile sig_atomic_t trap_pending[TRAP_MAX_SIG];
static volatile sig_atomic_t trap_any_pending;

static void trap_handler(int sig) {
    if (sig > 0 && sig < TRAP_MAX_SIG) {
        trap_pending[sig] = 1;
        trap_any_pending = 1;
    }
}

typedef struct { const char *name; int sig; } TrapName;

static const TrapName TRAP_NAMES[] = {
    {"EXIT", 0},   {"HUP", SIGHUP},   {"INT", SIGINT},
    {"QUIT", SIGQUIT}, {"ILL", SIGILL}, {"ABRT", SIGABRT},
    {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"SEGV", SIGSEGV},
    {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
    {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"CHLD", SIGCHLD},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP},
    {0, 0}
};

static const char *trap_signame(int sig) {
    for (int i = 0; TRAP_NAMES[i].name; i++) {
        if (TRAP_NAMES[i].sig == sig) {
            return TRAP_NAMES[i].name;
        }
    }
    return 0;
}

static int trap_signo(const char *name) {
    const TrapName *NAMES = TRAP_NAMES;
    if (name[0] >= '0' && name[0] <= '9') {
        int n = atoi(name);
        return (n >= 0 && n < TRAP_MAX_SIG) ? n : -1;
    }
    if (strncmp(name, "SIG", 3) == 0) {
        name += 3;
    }
    for (int i = 0; NAMES[i].name; i++) {
        if (strcmp(name, NAMES[i].name) == 0) {
            return NAMES[i].sig;
        }
    }
    return -1;
}

static void trap_set(int sig, const char *action) {
    char **slot = (sig == 0) ? &trap_exit_action : &trap_action[sig];
    free(*slot);
    *slot = 0;
    if (action) {
        *slot = xstrdup(action);
    }
    if (sig == 0) {
        return;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return;
    }
    if (!action) {
        signal(sig, SIG_DFL);
    } else if (action[0] == '\0') {
        signal(sig, SIG_IGN);
    } else {
        signal(sig, trap_handler);
    }
}

static void trap_run_pending(void) {
    if (!trap_any_pending) {
        return;
    }
    trap_any_pending = 0;
    for (int sig = 1; sig < TRAP_MAX_SIG; sig++) {
        if (!trap_pending[sig]) {
            continue;
        }
        trap_pending[sig] = 0;
        if (trap_action[sig] && trap_action[sig][0]) {
            int saved = last_status;
            exec_text(trap_action[sig]);
            last_status = saved;
        }
    }
}

static void trap_run_exit(void) {
    char *action = trap_exit_action;
    if (!action || !action[0]) {
        return;
    }
    trap_exit_action = 0;
    exec_text(action);
    free(action);
}

static int run_builtin(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, ":") == 0 || strcmp(cmd, "true") == 0) {
        return 0;
    }
    if (strcmp(cmd, "false") == 0) {
        return 1;
    }
    if (strcmp(cmd, "exit") == 0) {
        flow = FLOW_EXIT;
        exit_code = argc > 1 ? atoi(argv[1]) : last_status;
        return exit_code;
    }
    if (strcmp(cmd, "exec") == 0) {
        if (argc == 1) {
            exec_keep_redirs = 1;
            return 0;
        }
        char **args = argv + 1;
        execvp(args[0], args);
        errmsg("exec: ", args[0], ": not found");
        if (!interactive) {
            trap_run_exit();
            _exit(127);
        }
        return 127;
    }
    if (strcmp(cmd, "trap") == 0) {
        if (argc == 1) {
            for (int sig = 0; sig < TRAP_MAX_SIG; sig++) {
                const char *action = (sig == 0) ? trap_exit_action
                                                : trap_action[sig];
                if (!action) {
                    continue;
                }
                const char *name = trap_signame(sig);
                out_fd_str(1, "trap -- '");
                out_fd_str(1, action);
                out_fd_str(1, "' ");
                out_fd_str(1, name ? name : num_to_str(sig));
                out_fd_str(1, "\n");
            }
            return 0;
        }
        int first = 1;
        const char *action = argv[1];
        int reset = 0;
        if (strcmp(action, "-") == 0) {
            reset = 1;
            first = 2;
        } else if (argc == 2 && trap_signo(action) >= 0) {
            reset = 1;
            first = 1;
        } else {
            first = 2;
        }
        if (first >= argc) {
            errmsg("trap: ", "no signal given", 0);
            return 1;
        }
        for (int i = first; i < argc; i++) {
            int sig = trap_signo(argv[i]);
            if (sig < 0) {
                errmsg("trap: ", argv[i], ": bad signal");
                return 1;
            }
            trap_set(sig, reset ? 0 : action);
        }
        return 0;
    }
    if (strcmp(cmd, "return") == 0) {
        flow = FLOW_RETURN;
        return argc > 1 ? atoi(argv[1]) : last_status;
    }
    if (strcmp(cmd, "break") == 0 || strcmp(cmd, "continue") == 0) {
        flow = cmd[0] == 'b' ? FLOW_BREAK : FLOW_CONTINUE;
        flow_levels = argc > 1 ? atoi(argv[1]) : 1;
        if (flow_levels < 1) {
            flow_levels = 1;
        }
        return 0;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char *where = argc > 1 ? argv[1] : var_get("HOME");
        if (!where[0]) {
            where = PATH_HOME;
        }
        if (chdir(where) != 0) {
            errmsg("cd: not a directory: ", where, 0);
            return 1;
        }
        char buf[PATH_MAX_LEN];
        if (getcwd(buf, sizeof(buf))) {
            var_set("PWD", buf);
        }
        return 0;
    }
    if (strcmp(cmd, "pwd") == 0) {
        char buf[PATH_MAX_LEN];
        if (!getcwd(buf, sizeof(buf))) {
            return 1;
        }
        out_fd_str(1, buf);
        out_fd_str(1, "\n");
        return 0;
    }
    if (strcmp(cmd, "echo") == 0) {
        int start = 1;
        int newline = 1;
        if (argc > 1 && strcmp(argv[1], "-n") == 0) {
            newline = 0;
            start = 2;
        }
        for (int i = start; i < argc; i++) {
            if (i > start) {
                out_fd_str(1, " ");
            }
            out_fd_str(1, argv[i]);
        }
        if (newline) {
            out_fd_str(1, "\n");
        }
        return 0;
    }
    if (strcmp(cmd, "export") == 0) {
        if (argc == 1) {
            for (Var *v = vars; v; v = v->next) {
                if (v->exported) {
                    out_fd_str(1, "export ");
                    out_fd_str(1, v->name);
                    out_fd_str(1, "=");
                    out_fd_str(1, v->value ? v->value : "");
                    out_fd_str(1, "\n");
                }
            }
            return 0;
        }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                var_set(argv[i], eq + 1);
                var_export(argv[i]);
                *eq = '=';
            } else {
                var_export(argv[i]);
            }
        }
        return 0;
    }
    if (strcmp(cmd, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            var_unset(argv[i]);
        }
        return 0;
    }
    if (strcmp(cmd, "env") == 0) {
        for (int i = 0; environ && environ[i]; i++) {
            out_fd_str(1, environ[i]);
            out_fd_str(1, "\n");
        }
        return 0;
    }
    if (strcmp(cmd, "set") == 0) {
        if (argc == 1) {
            for (Var *v = vars; v; v = v->next) {
                out_fd_str(1, v->name);
                out_fd_str(1, "=");
                out_fd_str(1, v->value ? v->value : "");
                out_fd_str(1, "\n");
            }
            return 0;
        }
        int i = 1;
        int saw_dashdash = 0;
        for (; i < argc; i++) {
            const char *a = argv[i];
            if (strcmp(a, "--") == 0) {
                saw_dashdash = 1;
                i++;
                break;
            }
            if (a[0] != '-' && a[0] != '+') {
                break;
            }
            if (!a[1]) {
                break;
            }
            int on = (a[0] == '-');
            if (a[1] == 'o') {
                if (i + 1 >= argc) {
                    static const struct { const char *name; int *flag; } OPTS[] = {
                        {"errexit", &opt_errexit}, {"nounset", &opt_nounset},
                        {"noglob", &opt_noglob},   {"xtrace", &opt_xtrace},
                        {0, 0}
                    };
                    for (int k = 0; OPTS[k].name; k++) {
                        out_fd_str(1, OPTS[k].name);
                        out_fd_str(1, *OPTS[k].flag ? "\ton\n" : "\toff\n");
                    }
                    out_fd_str(1, "posix\ton\n");
                    continue;
                }
                const char *name = argv[++i];
                if (strcmp(name, "errexit") == 0) { opt_errexit = on; }
                else if (strcmp(name, "nounset") == 0) { opt_nounset = on; }
                else if (strcmp(name, "noglob") == 0) { opt_noglob = on; }
                else if (strcmp(name, "xtrace") == 0) { opt_xtrace = on; }
                else if (strcmp(name, "posix") == 0) {   }
                else {
                    errmsg("set: ", name, ": no such option");
                    return 2;
                }
                continue;
            }
            for (const char *f = a + 1; *f; f++) {
                switch (*f) {
                    case 'e': opt_errexit = on; break;
                    case 'u': opt_nounset = on; break;
                    case 'f': opt_noglob = on; break;
                    case 'x': opt_xtrace = on; break;
                    default: {
                        char one[2] = {*f, 0};
                        errmsg("set: -", one, ": no such option");
                        return 2;
                    }
                }
            }
        }
        if (saw_dashdash || i < argc) {
            set_positional(argv + i, argc - i);
        }
        return 0;
    }
    if (strcmp(cmd, "shift") == 0) {
        int n = argc > 1 ? atoi(argv[1]) : 1;
        if (n > pos_count || n < 0) {
            return 1;
        }
        for (int i = 0; i < n; i++) {
            free(pos_params[i]);
        }
        for (int i = n; i < pos_count; i++) {
            pos_params[i - n] = pos_params[i];
        }
        pos_count -= n;
        return 0;
    }
    if (strcmp(cmd, "read") == 0) {
        Sbuf line;
        sb_init(&line);
        int got = read_line_fd(0, &line);
        const char *text = line.p ? line.p : "";
        if (argc <= 1) {
            var_set("REPLY", text);
        } else {
            const char *p = text;
            const char *ifs = ifs_chars();
            for (int i = 1; i < argc; i++) {
                while (*p && strchr(ifs, *p)) {
                    p++;
                }
                if (i == argc - 1) {
                    var_set(argv[i], p);
                    break;
                }
                const char *start = p;
                while (*p && !strchr(ifs, *p)) {
                    p++;
                }
                char *word = (char *)malloc((size_t)(p - start) + 1);
                if (!word) {
                    die_oom();
                }
                memcpy(word, start, (size_t)(p - start));
                word[p - start] = '\0';
                var_set(argv[i], word);
                free(word);
            }
        }
        sb_free(&line);
        return got ? 0 : 1;
    }
    if (strcmp(cmd, "test") == 0 || strcmp(cmd, "[") == 0) {
        return bi_test(argc, argv);
    }
    if (strcmp(cmd, "eval") == 0) {
        Sbuf b;
        sb_init(&b);
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                sb_putc(&b, ' ');
            }
            sb_puts(&b, argv[i]);
        }
        int st = b.p ? exec_text(b.p) : 0;
        sb_free(&b);
        return st;
    }
    if (strcmp(cmd, ".") == 0 || strcmp(cmd, "source") == 0) {
        if (argc < 2) {
            errmsg(".: needs a file", 0, 0);
            return 2;
        }
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            errmsg(".: cannot read ", argv[1], 0);
            return 1;
        }
        Sbuf text;
        sb_init(&text);
        char buf[512];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            sb_putn(&text, buf, (size_t)n);
        }
        close(fd);
        int st = text.p ? exec_text(text.p) : 0;
        sb_free(&text);
        if (flow == FLOW_RETURN) {
            flow = FLOW_NONE;
        }
        return st;
    }
    if (strcmp(cmd, "wait") == 0) {
        int st = 0;
        if (argc > 1) {
            int status = 0;
            waitpid(atoi(argv[1]), &status, 0);
            st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        } else {
            for (int i = 0; i < nbackground; i++) {
                int status = 0;
                waitpid(background_pids[i], &status, 0);
                st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            nbackground = 0;
        }
        return st;
    }
    if (strcmp(cmd, "unalias") == 0) {
        return 0;
    }
    return 127;
}

static int test_one(int argc, char **argv, int *i);

static int file_is(const char *path, int want_dir) {
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        return want_dir;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return !want_dir;
}

static int file_exists(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static long file_size(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    long end = lseek(fd, 0, SEEK_END);
    close(fd);
    return end;
}

static int test_one(int argc, char **argv, int *i) {
    if (*i >= argc) {
        return 0;
    }
    const char *a = argv[*i];

    if (strcmp(a, "!") == 0) {
        (*i)++;
        return !test_one(argc, argv, i);
    }
    if (a[0] == '-' && a[1] && !a[2] && *i + 1 < argc) {
        const char *arg = argv[*i + 1];
        *i += 2;
        switch (a[1]) {
            case 'e': return file_exists(arg);
            case 'f': return file_is(arg, 0);
            case 'd': return file_is(arg, 1);
            case 'r': return file_exists(arg);
            case 'w': return file_exists(arg);
            case 'x': return file_exists(arg);
            case 's': return file_size(arg) > 0;
            case 'z': return arg[0] == '\0';
            case 'n': return arg[0] != '\0';
            default:  return arg[0] != '\0';
        }
    }
    if (*i + 2 < argc) {
        const char *op = argv[*i + 1];
        const char *b = argv[*i + 2];
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            *i += 3;
            return strcmp(a, b) == 0;
        }
        if (strcmp(op, "!=") == 0) {
            *i += 3;
            return strcmp(a, b) != 0;
        }
        if (op[0] == '-' && strlen(op) == 3) {
            long x = atol(a), y = atol(b);
            *i += 3;
            if (strcmp(op, "-eq") == 0) { return x == y; }
            if (strcmp(op, "-ne") == 0) { return x != y; }
            if (strcmp(op, "-lt") == 0) { return x < y; }
            if (strcmp(op, "-le") == 0) { return x <= y; }
            if (strcmp(op, "-gt") == 0) { return x > y; }
            if (strcmp(op, "-ge") == 0) { return x >= y; }
            *i -= 3;
        }
    }
    (*i)++;
    return a[0] != '\0';
}

static int bi_test(int argc, char **argv) {
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            errmsg("[: missing `]'", 0, 0);
            return 2;
        }
        argc--;
    }
    if (argc <= 1) {
        return 1;
    }
    int i = 1;
    int result = test_one(argc, argv, &i);
    while (i < argc) {
        if (strcmp(argv[i], "-a") == 0) {
            i++;
            result = test_one(argc, argv, &i) && result;
        } else if (strcmp(argv[i], "-o") == 0) {
            i++;
            int rhs = test_one(argc, argv, &i);
            result = result || rhs;
        } else {
            break;
        }
    }
    return result ? 0 : 1;
}

static int exec_list_node(Node *n) {
    return n ? exec_node(n) : last_status;
}

static int exec_external(char **argv, Node *n) {
    pid_t pid = fork();
    if (pid < 0) {
        errmsg("cannot fork", 0, 0);
        return 1;
    }
    if (pid == 0) {
        RedirSave dummy;
        memset(&dummy, 0, sizeof(dummy));
        if (apply_redirs(n->redirs, 0) != 0) {
            _exit(1);
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            setenv(copy, value, 1);
            free(value);
            free(copy);
        }

        if (strchr(argv[0], '/')) {
            execve(argv[0], argv, environ);
        } else {
            const char *path = var_get("PATH");
            if (!path[0]) {
                path = PATH_DEFAULT;
            }
            const char *p = path;
            while (*p) {
                const char *end = strchr(p, ':');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                Sbuf full;
                sb_init(&full);
                sb_putn(&full, p, len);
                if (len == 0 || p[len - 1] != '/') {
                    sb_putc(&full, '/');
                }
                sb_puts(&full, argv[0]);
                execve(full.p, argv, environ);
                sb_free(&full);
                if (!end) {
                    break;
                }
                p = end + 1;
            }
        }
        out_fd_str(2, "sh: ");
        out_fd_str(2, argv[0]);
        out_fd_str(2, ": command not found\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int call_function(Func *f, char **argv, int argc) {
    char **saved = pos_params;
    int saved_n = pos_count;
    char *saved_name = script_name;
    pos_params = 0;
    pos_count = 0;
    set_positional(argv + 1, argc - 1);

    int st = exec_list_node(f->body);
    if (flow == FLOW_RETURN) {
        flow = FLOW_NONE;
    }

    for (int i = 0; i < pos_count; i++) {
        free(pos_params[i]);
    }
    free(pos_params);
    pos_params = saved;
    pos_count = saved_n;
    script_name = saved_name;
    return st;
}

static int exec_simple(Node *n) {
    Vec argv;
    vec_init(&argv);
    for (int i = 0; i < n->nwords; i++) {
        expand_word(n->words[i], &argv, 1, 1);
    }

    if (opt_xtrace && argv.n) {
        out_fd_str(2, "+");
        for (int i = 0; i < argv.n; i++) {
            out_fd_str(2, " ");
            out_fd_str(2, argv.v[i]);
        }
        out_fd_str(2, "\n");
    }

    if (argv.n == 0) {
        RedirSave save;
        memset(&save, 0, sizeof(save));
        int rc = 0;
        if (n->redirs) {
            rc = apply_redirs(n->redirs, &save);
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            var_set(copy, value);
            free(value);
            free(copy);
        }
        if (n->redirs) {
            undo_redirs(&save);
        }
        vec_free(&argv);
        return rc == 0 ? 0 : 1;
    }

    Func *f = func_find(argv.v[0]);
    if (f || is_builtin(argv.v[0])) {
        RedirSave save;
        memset(&save, 0, sizeof(save));
        if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
            vec_free(&argv);
            return 1;
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *copy = xstrdup(n->assigns[i]);
            char *eq = strchr(copy, '=');
            *eq = '\0';
            char *value = expand_one(eq + 1);
            var_set(copy, value);
            free(value);
            free(copy);
        }
        exec_keep_redirs = 0;
        int st = f ? call_function(f, argv.v, argv.n) : run_builtin(argv.n, argv.v);
        if (n->redirs && !exec_keep_redirs) {
            undo_redirs(&save);
        }
        exec_keep_redirs = 0;
        vec_free(&argv);
        return st;
    }

    int st = exec_external(argv.v, n);
    vec_free(&argv);
    return st;
}

static int exec_pipeline(Node *n) {
    Node *stages[32];
    int count = 0;
    Node *cur = n;
    while (cur->kind == N_PIPE && count < 30) {
        stages[count++] = cur->right;
        cur = cur->left;
    }
    stages[count++] = cur;
    for (int i = 0; i < count / 2; i++) {
        Node *t = stages[i];
        stages[i] = stages[count - 1 - i];
        stages[count - 1 - i] = t;
    }

    int in_fd = -1;
    pid_t pids[32];
    for (int i = 0; i < count; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < count && pipe(fds) != 0) {
            errmsg("cannot create a pipe", 0, 0);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            errmsg("cannot fork", 0, 0);
            return 1;
        }
        if (pid == 0) {
            if (in_fd >= 0) {
                dup2(in_fd, 0);
                close(in_fd);
            }
            if (fds[1] >= 0) {
                close(fds[0]);
                dup2(fds[1], 1);
                close(fds[1]);
            }
            int st = exec_node(stages[i]);
            _exit(st);
        }
        pids[i] = pid;
        if (in_fd >= 0) {
            close(in_fd);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
            in_fd = fds[0];
        }
    }

    int st = 0;
    for (int i = 0; i < count; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (i == count - 1) {
            st = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
    return st;
}

static int exec_case(Node *n) {
    char *subject = expand_one(n->name);
    int st = 0;
    for (CaseItem *it = n->cases; it; it = it->next) {
        for (int i = 0; i < it->npats; i++) {
            char *pat = expand_pattern(it->pats[i]);
            int hit = glob_match(pat, subject);
            free(pat);
            if (hit) {
                st = exec_list_node(it->body);
                free(subject);
                return st;
            }
        }
    }
    free(subject);
    return st;
}

static int exec_for(Node *n) {
    Vec items;
    vec_init(&items);
    for (int i = 0; i < n->nwords; i++) {
        expand_word(n->words[i], &items, 1, 1);
    }
    int st = 0;
    for (int i = 0; i < items.n; i++) {
        var_set(n->name, items.v[i]);
        st = exec_list_node(n->right);
        if (flow == FLOW_BREAK) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
            }
            break;
        }
        if (flow == FLOW_CONTINUE) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
                continue;
            }
            break;
        }
        if (flow != FLOW_NONE) {
            break;
        }
    }
    vec_free(&items);
    return st;
}

static int exec_while(Node *n, int until) {
    int st = 0;
    for (;;) {
        errexit_suspend++;
        int cond = exec_list_node(n->left);
        errexit_suspend--;
        if (flow != FLOW_NONE) {
            break;
        }
        int go = until ? (cond != 0) : (cond == 0);
        if (!go) {
            break;
        }
        st = exec_list_node(n->right);
        if (flow == FLOW_BREAK) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
            }
            break;
        }
        if (flow == FLOW_CONTINUE) {
            if (--flow_levels <= 0) {
                flow = FLOW_NONE;
                continue;
            }
            break;
        }
        if (flow != FLOW_NONE) {
            break;
        }
    }
    return st;
}

static int exec_forked(Node *body) {
    pid_t pid = fork();
    if (pid < 0) {
        errmsg("cannot fork", 0, 0);
        return 1;
    }
    if (pid == 0) {
        int st = exec_list_node(body);
        _exit(st);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int exec_node_inner(Node *n);

static int exec_node(Node *n) {
    if (!n || flow != FLOW_NONE) {
        return last_status;
    }
    trap_run_pending();
    int st_ee = exec_node_inner(n);
    if (opt_errexit && st_ee != 0 && !errexit_suspend && flow == FLOW_NONE &&
        (n->kind == N_SIMPLE || n->kind == N_PIPE)) {
        flow = FLOW_EXIT;
        exit_code = st_ee;
    }
    return st_ee;
}

static int exec_node_inner(Node *n) {
    switch (n->kind) {
        case N_SIMPLE:
            return exec_simple(n);
        case N_SEQ: {
            int st = exec_node(n->left);
            last_status = st;
            if (flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_AND: {
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            last_status = st;
            if (st != 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_OR: {
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            last_status = st;
            if (st == 0 || flow != FLOW_NONE) {
                return st;
            }
            return exec_node(n->right);
        }
        case N_NOT: {
            errexit_suspend++;
            int st = exec_node(n->left);
            errexit_suspend--;
            return st == 0 ? 1 : 0;
        }
        case N_PIPE:
            return exec_pipeline(n);
        case N_BG: {
            pid_t pid = fork();
            if (pid < 0) {
                errmsg("cannot fork", 0, 0);
                return 1;
            }
            if (pid == 0) {
                int st = exec_node(n->left);
                _exit(st);
            }
            if (nbackground < (int)(sizeof(background_pids) / sizeof(background_pids[0]))) {
                background_pids[nbackground++] = (int)pid;
            }
            out_fd_str(1, "[");
            out_fd_str(1, num_to_str(pid));
            out_fd_str(1, "]\n");
            return 0;
        }
        case N_IF: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            errexit_suspend++;
            int cond = exec_list_node(n->left);
            errexit_suspend--;
            int st;
            if (flow != FLOW_NONE) {
                st = cond;
            } else if (cond == 0) {
                st = exec_list_node(n->right);
            } else {
                st = n->third ? exec_list_node(n->third) : 0;
            }
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_WHILE:
        case N_UNTIL:
        case N_FOR:
        case N_CASE:
        case N_GROUP: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            int st;
            if (n->kind == N_WHILE) {
                st = exec_while(n, 0);
            } else if (n->kind == N_UNTIL) {
                st = exec_while(n, 1);
            } else if (n->kind == N_FOR) {
                st = exec_for(n);
            } else if (n->kind == N_CASE) {
                st = exec_case(n);
            } else {
                st = exec_list_node(n->left);
            }
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_SUBSHELL: {
            RedirSave save;
            memset(&save, 0, sizeof(save));
            if (n->redirs && apply_redirs(n->redirs, &save) != 0) {
                return 1;
            }
            int st = exec_forked(n->left);
            if (n->redirs) {
                undo_redirs(&save);
            }
            return st;
        }
        case N_FUNC:
            func_define(n->name, n->left);
            return 0;
        default:
            return 0;
    }
}

static int exec_text(const char *text) {
    Lexer save_lx = lx;
    Tok save_tok = tok;
    Arena *save_arena = cur_arena;
    int save_err = parse_error;
    int save_depth = parse_depth;
    int save_interactive = interactive;

    Arena a;
    memset(&a, 0, sizeof(a));
    cur_arena = &a;
    memset(&lx, 0, sizeof(lx));
    lx.src = text;
    parse_error = 0;
    parse_depth = 0;
    interactive = 0;

    advance();
    Node *prog = parse_list(0);
    int st = last_status;
    if (parse_error) {
        st = 2;
    } else if (prog) {
        st = exec_node(prog);
        last_status = st;
    }

    arena_free(&a);
    cur_arena = save_arena;
    lx = save_lx;
    tok = save_tok;
    parse_error = save_err;
    parse_depth = save_depth;
    interactive = save_interactive;
    return st;
}

static int read_line_interactive(Sbuf *buf) {
    for (;;) {
        char c;
        long n = read(0, &c, 1);
        if (n <= 0) {
            return buf->len > 0;
        }
        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            return 1;
        }
        if (c == '\b' || c == 0x7F) {
            if (buf->len > 0) {
                buf->p[--buf->len] = '\0';
                write(1, "\b \b", 3);
            }
            continue;
        }
        sb_putc(buf, c);
        write(1, &c, 1);
    }
}

static void interactive_loop(void) {
    Sbuf pending;
    sb_init(&pending);
    interactive = 1;

    for (;;) {
        char dir[PATH_MAX_LEN];
        if (pending.len == 0) {
            if (getcwd(dir, sizeof(dir))) {
                out_fd_str(1, dir);
            }
            out_fd_str(1, " $ ");
        } else {
            out_fd_str(1, "> ");
        }

        Sbuf line;
        sb_init(&line);
        if (!read_line_interactive(&line)) {
            sb_free(&line);
            break;
        }
        sb_puts(&pending, line.p ? line.p : "");
        sb_putc(&pending, '\n');
        sb_free(&line);

        Arena a;
        memset(&a, 0, sizeof(a));
        cur_arena = &a;
        memset(&lx, 0, sizeof(lx));
        lx.src = pending.p;
        parse_error = 0;
        parse_depth = 0;
        advance();
        Node *prog = parse_list(0);

        if (lx.incomplete) {
            arena_free(&a);
            continue;
        }
        if (!parse_error && prog) {
            last_status = exec_node(prog);
        }
        arena_free(&a);
        sb_free(&pending);
        sb_init(&pending);

        if (flow == FLOW_EXIT) {
            break;
        }
        flow = FLOW_NONE;
    }
    sb_free(&pending);
}

static int run_script_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        errmsg("cannot read ", path, 0);
        return 127;
    }
    Sbuf text;
    sb_init(&text);
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sb_putn(&text, buf, (size_t)n);
    }
    close(fd);
    int st = text.p ? exec_text(text.p) : 0;
    sb_free(&text);
    return flow == FLOW_EXIT ? exit_code : st;
}

static int shell_leaving(int status) {
    last_status = status;
    flow = FLOW_NONE;
    trap_run_exit();
    return status;
}

int main(int argc, char **argv) {
    shell_pid = getpid();

    setenv("PATH", PATH_DEFAULT, 0);

    if (write(2, "", 0) < 0) {
        dup2(1, 2);
    }
    if (!var_is_set("IFS")) {
        var_set("IFS", " \t\n");
    }

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        script_name = argv[0];
        if (argc > 3) {
            set_positional(argv + 3, argc - 3);
        }
        int st = exec_text(argv[2]);
        return shell_leaving(flow == FLOW_EXIT ? exit_code : st);
    }

    if (argc > 1 && argv[1][0]) {
        script_name = argv[1];
        if (argc > 2) {
            set_positional(argv + 2, argc - 2);
        }
        return shell_leaving(run_script_file(argv[1]));
    }

    interactive_loop();
    return shell_leaving(flow == FLOW_EXIT ? exit_code : last_status);
}
