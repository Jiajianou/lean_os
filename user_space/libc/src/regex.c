#include <regex.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    N_CHAR,
    N_ANY,
    N_CLASS,
    N_CAT,
    N_ALT,
    N_REP,
    N_GROUP,
    N_BOL,
    N_EOL,
    N_BACKREF,
    N_EMPTY,
};

typedef struct node {
    int type;
    int n;
    int min, max;
    unsigned char set[32];
    struct node *a, *b;
} node_t;

typedef struct {
    const char *p;
    int cflags;
    int ngroup;
    int error;
    node_t **arena;
    size_t narena, arena_cap;
} parser_t;

static node_t *node_new(parser_t *ps, int type) {
    if (ps->error) {
        return (node_t *)0;
    }
    if (ps->narena == ps->arena_cap) {
        size_t cap = ps->arena_cap ? ps->arena_cap * 2 : 32;
        node_t **grown = realloc(ps->arena, cap * sizeof(node_t *));
        if (!grown) {
            ps->error = REG_ESPACE;
            return (node_t *)0;
        }
        ps->arena = grown;
        ps->arena_cap = cap;
    }
    node_t *nd = calloc(1, sizeof(node_t));
    if (!nd) {
        ps->error = REG_ESPACE;
        return (node_t *)0;
    }
    nd->type = type;
    ps->arena[ps->narena++] = nd;
    return nd;
}

static void set_add(node_t *nd, unsigned char c) {
    nd->set[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static int set_has(const node_t *nd, unsigned char c) {
    return (nd->set[c >> 3] >> (c & 7)) & 1;
}

static int add_named_class(node_t *nd, const char *name, size_t length) {
    static const struct { const char *name; int (*function)(int); } table[] = {
        {"alpha", isalpha}, {"digit", isdigit},  {"alnum", isalnum},
        {"upper", isupper}, {"lower", islower},  {"space", isspace},
        {"blank", isblank}, {"punct", ispunct},  {"print", isprint},
        {"graph", isgraph}, {"cntrl", iscntrl},  {"xdigit", isxdigit},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) != length || strncmp(table[i].name, name, length) != 0) {
            continue;
        }
        for (int c = 0; c < 256; c++) {
            if (table[i].function(c)) {
                set_add(nd, (unsigned char)c);
            }
        }
        return 1;
    }
    return 0;
}

static node_t *parse_class(parser_t *ps) {
    node_t *nd = node_new(ps, N_CLASS);
    if (!nd) {
        return (node_t *)0;
    }
    int negate = 0;
    if (*ps->p == '^') {
        negate = 1;
        ps->p++;
    }
    int first = 1;
    while (*ps->p && (*ps->p != ']' || first)) {
        first = 0;
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            const char *start = ps->p + 2;
            const char *end = strstr(start, ":]");
            if (!end) {
                ps->error = REG_ECTYPE;
                return (node_t *)0;
            }
            if (!add_named_class(nd, start, (size_t)(end - start))) {
                ps->error = REG_ECTYPE;
                return (node_t *)0;
            }
            ps->p = end + 2;
            continue;
        }
        unsigned char lo = (unsigned char)*ps->p++;
        if (lo == '\\' && *ps->p) {
            lo = (unsigned char)*ps->p++;
            switch (lo) {
            case 'n': lo = '\n'; break;
            case 't': lo = '\t'; break;
            case 'r': lo = '\r'; break;
            default: break;
            }
        }
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            unsigned char hi = (unsigned char)ps->p[1];
            ps->p += 2;
            if (hi == '\\' && *ps->p) {
                hi = (unsigned char)*ps->p++;
            }
            if (hi < lo) {
                ps->error = REG_ERANGE;
                return (node_t *)0;
            }
            for (int c = lo; c <= (int)hi; c++) {
                set_add(nd, (unsigned char)c);
            }
        } else {
            set_add(nd, lo);
        }
    }
    if (*ps->p != ']') {
        ps->error = REG_EBRACK;
        return (node_t *)0;
    }
    ps->p++;
    if (ps->cflags & REG_ICASE) {
        for (int c = 0; c < 256; c++) {
            if (set_has(nd, (unsigned char)c)) {
                set_add(nd, (unsigned char)tolower(c));
                set_add(nd, (unsigned char)toupper(c));
            }
        }
    }
    if (negate) {
        for (size_t i = 0; i < sizeof(nd->set); i++) {
            nd->set[i] = (unsigned char)~nd->set[i];
        }
        if (ps->cflags & REG_NEWLINE) {
            nd->set['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
        }
    }
    return nd;
}

static node_t *parse_alt(parser_t *ps);

static int is_op(parser_t *ps, const char *p, char op) {
    if (ps->cflags & REG_EXTENDED) {
        return *p == op ? 1 : 0;
    }
    return (p[0] == '\\' && p[1] == op) ? 2 : 0;
}

static node_t *parse_atom(parser_t *ps) {
    if (ps->error) {
        return (node_t *)0;
    }
    const char *p = ps->p;
    int w;

    if ((w = is_op(ps, p, '(')) != 0) {
        ps->p += w;
        int index = ++ps->ngroup;
        node_t *inner = parse_alt(ps);
        if (ps->error) {
            return (node_t *)0;
        }
        if ((w = is_op(ps, ps->p, ')')) == 0) {
            ps->error = REG_EPAREN;
            return (node_t *)0;
        }
        ps->p += w;
        node_t *nd = node_new(ps, N_GROUP);
        if (!nd) {
            return (node_t *)0;
        }
        nd->n = index;
        nd->a = inner;
        return nd;
    }

    if (*p == '[') {
        ps->p++;
        return parse_class(ps);
    }

    if (*p == '.') {
        ps->p++;
        node_t *nd = node_new(ps, N_ANY);
        return nd;
    }

    if (*p == '\\' && p[1]) {
        if (p[1] >= '1' && p[1] <= '9') {
            int index = p[1] - '0';
            if (index > ps->ngroup) {
                ps->error = REG_ESUBREG;
                return (node_t *)0;
            }
            ps->p += 2;
            node_t *nd = node_new(ps, N_BACKREF);
            if (nd) {
                nd->n = index;
            }
            return nd;
        }
        node_t *nd = node_new(ps, N_CHAR);
        if (!nd) {
            return (node_t *)0;
        }
        switch (p[1]) {
        case 'n': nd->n = '\n'; break;
        case 't': nd->n = '\t'; break;
        case 'r': nd->n = '\r'; break;
        default:  nd->n = (unsigned char)p[1]; break;
        }
        ps->p += 2;
        return nd;
    }

    if (*p == '\\' && !p[1]) {
        ps->error = REG_EESCAPE;
        return (node_t *)0;
    }

    node_t *nd = node_new(ps, N_CHAR);
    if (!nd) {
        return (node_t *)0;
    }
    nd->n = (unsigned char)*p;
    ps->p++;
    return nd;
}

static int parse_interval(parser_t *ps, int *min, int *max) {
    const char *p = ps->p;
    if (!isdigit((unsigned char)*p)) {
        return REG_BADBR;
    }
    int lo = 0;
    while (isdigit((unsigned char)*p)) {
        lo = lo * 10 + (*p++ - '0');
        if (lo > 65535) {
            return REG_BADBR;
        }
    }
    int hi = lo;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) {
            hi = 0;
            while (isdigit((unsigned char)*p)) {
                hi = hi * 10 + (*p++ - '0');
                if (hi > 65535) {
                    return REG_BADBR;
                }
            }
        } else {
            hi = -1;
        }
    }
    if (hi >= 0 && hi < lo) {
        return REG_BADBR;
    }
    ps->p = p;
    *min = lo;
    *max = hi;
    return 0;
}

static node_t *parse_piece(parser_t *ps) {
    node_t *atom = parse_atom(ps);
    if (ps->error) {
        return (node_t *)0;
    }
    for (;;) {
        int min, max, w;
        if (*ps->p == '*') {
            min = 0;
            max = -1;
            ps->p++;
        } else if ((w = is_op(ps, ps->p, '+')) != 0) {
            min = 1;
            max = -1;
            ps->p += w;
        } else if ((w = is_op(ps, ps->p, '?')) != 0) {
            min = 0;
            max = 1;
            ps->p += w;
        } else if ((w = is_op(ps, ps->p, '{')) != 0 &&
                   !isdigit((unsigned char)ps->p[w]) &&
                   !(ps->cflags & REG_EXTENDED)) {
            ps->error = REG_EBRACE;
            return (node_t *)0;
        } else if ((w = is_op(ps, ps->p, '{')) != 0 &&
                   isdigit((unsigned char)ps->p[w])) {
            const char *save = ps->p;
            ps->p += w;
            int e = parse_interval(ps, &min, &max);
            if (e) {
                ps->error = e;
                return (node_t *)0;
            }
            if ((w = is_op(ps, ps->p, '}')) == 0) {
                ps->p = save;
                ps->error = REG_EBRACE;
                return (node_t *)0;
            }
            ps->p += w;
        } else {
            return atom;
        }
        if (!atom) {
            ps->error = REG_BADRPT;
            return (node_t *)0;
        }
        node_t *rep = node_new(ps, N_REP);
        if (!rep) {
            return (node_t *)0;
        }
        rep->a = atom;
        rep->min = min;
        rep->max = max;
        atom = rep;
    }
}

static int at_branch_end(parser_t *ps) {
    if (!*ps->p) {
        return 1;
    }
    return is_op(ps, ps->p, '|') || is_op(ps, ps->p, ')');
}

static node_t *parse_branch(parser_t *ps) {
    node_t *left = (node_t *)0;
    while (!ps->error && !at_branch_end(ps)) {
        node_t *piece;
        if (!left && (ps->cflags & REG_EXTENDED) &&
            (*ps->p == '*' || *ps->p == '+' || *ps->p == '?')) {
            ps->error = REG_BADRPT;
            return (node_t *)0;
        }
        if (*ps->p == '^' && !left) {
            ps->p++;
            piece = node_new(ps, N_BOL);
        } else if (*ps->p == '$' && (!ps->p[1] || at_branch_end(ps))) {
            const char *save = ps->p;
            ps->p++;
            if (at_branch_end(ps)) {
                piece = node_new(ps, N_EOL);
            } else {
                ps->p = save;
                piece = parse_piece(ps);
            }
        } else {
            piece = parse_piece(ps);
        }
        if (ps->error) {
            return (node_t *)0;
        }
        if (!left) {
            left = piece;
            continue;
        }
        node_t *cat = node_new(ps, N_CAT);
        if (!cat) {
            return (node_t *)0;
        }
        cat->a = left;
        cat->b = piece;
        left = cat;
    }
    if (!left) {
        left = node_new(ps, N_EMPTY);
    }
    return left;
}

static node_t *parse_alt(parser_t *ps) {
    node_t *left = parse_branch(ps);
    int w;
    while (!ps->error && (w = is_op(ps, ps->p, '|')) != 0) {
        ps->p += w;
        node_t *right = parse_branch(ps);
        if (ps->error) {
            return (node_t *)0;
        }
        node_t *alt = node_new(ps, N_ALT);
        if (!alt) {
            return (node_t *)0;
        }
        alt->a = left;
        alt->b = right;
        left = alt;
    }
    return left;
}

typedef struct cont cont_t;

enum { K_NODE, K_REP, K_GEND };

struct cont {
    int kind;
    const node_t *node;
    const cont_t *next;
    int count;
    const char *from;
    int index;
};

typedef struct {
    const char *s;
    size_t length;
    int cflags, eflags;
    regmatch_t *caps;
    regmatch_t *best_caps;
    int ncaps;
    const char *best;
    int want_longest;
} matcher_t;

static int m_node(matcher_t *m, const node_t *nd, const char *position, const cont_t *k);

static int m_cont(matcher_t *m, const cont_t *k, const char *position);

static int m_accept(matcher_t *m, const char *position) {
    if (!m->best || position > m->best) {
        m->best = position;
        if (m->best_caps && m->caps) {
            for (int i = 0; i <= m->ncaps; i++) {
                m->best_caps[i] = m->caps[i];
            }
        }
    }
    if (!m->want_longest) {
        return 1;
    }
    return position == m->s + m->length;
}

static int m_rep(matcher_t *m, const node_t *nd, const char *position,
                 const cont_t *after, int count, const char *from) {
    if (from && position == from) {
        return count <= nd->min ? m_cont(m, after, position) : 0;
    }
    if (nd->max < 0 || count < nd->max) {
        cont_t again;
        again.kind = K_REP;
        again.node = nd;
        again.next = after;
        again.count = count + 1;
        again.from = position;
        again.index = 0;
        if (m_node(m, nd->a, position, &again)) {
            return 1;
        }
    }
    if (count >= nd->min) {
        return m_cont(m, after, position);
    }
    return 0;
}

static int m_cont(matcher_t *m, const cont_t *k, const char *position) {
    if (!k) {
        return m_accept(m, position);
    }
    switch (k->kind) {
    case K_NODE:
        return m_node(m, k->node, position, k->next);
    case K_REP:
        return m_rep(m, k->node, position, k->next, k->count, k->from);
    case K_GEND: {
        int idx = k->index;
        if (!m->caps || idx > m->ncaps) {
            return m_cont(m, k->next, position);
        }
        regmatch_t saved = m->caps[idx];
        m->caps[idx].rm_eo = (regoff_t)(position - m->s);
        int stop = m_cont(m, k->next, position);
        if (!stop) {
            m->caps[idx] = saved;
        }
        return stop;
    }
    default:
        return 0;
    }
}

static int chr_eq(const matcher_t *m, unsigned char a, unsigned char b) {
    if (a == b) {
        return 1;
    }
    return (m->cflags & REG_ICASE) && tolower(a) == tolower(b);
}

static int m_node(matcher_t *m, const node_t *nd, const char *position, const cont_t *k) {
    const char *end = m->s + m->length;
    switch (nd->type) {
    case N_EMPTY:
        return m_cont(m, k, position);

    case N_CHAR:
        if (position < end && chr_eq(m, (unsigned char)*position, (unsigned char)nd->n)) {
            return m_cont(m, k, position + 1);
        }
        return 0;

    case N_ANY:
        if (position < end) {
            if ((m->cflags & REG_NEWLINE) && *position == '\n') {
                return 0;
            }
            return m_cont(m, k, position + 1);
        }
        return 0;

    case N_CLASS:
        if (position < end && set_has(nd, (unsigned char)*position)) {
            return m_cont(m, k, position + 1);
        }
        return 0;

    case N_BOL:
        if (position == m->s) {
            return (m->eflags & REG_NOTBOL) ? 0 : m_cont(m, k, position);
        }
        if ((m->cflags & REG_NEWLINE) && position[-1] == '\n') {
            return m_cont(m, k, position);
        }
        return 0;

    case N_EOL:
        if (position == end) {
            return (m->eflags & REG_NOTEOL) ? 0 : m_cont(m, k, position);
        }
        if ((m->cflags & REG_NEWLINE) && *position == '\n') {
            return m_cont(m, k, position);
        }
        return 0;

    case N_CAT: {
        cont_t k2;
        k2.kind = K_NODE;
        k2.node = nd->b;
        k2.next = k;
        k2.count = 0;
        k2.from = (const char *)0;
        k2.index = 0;
        return m_node(m, nd->a, position, &k2);
    }

    case N_ALT:
        if (m_node(m, nd->a, position, k)) {
            return 1;
        }
        return m_node(m, nd->b, position, k);

    case N_GROUP: {
        int idx = nd->n;
        cont_t gend;
        gend.kind = K_GEND;
        gend.node = (const node_t *)0;
        gend.next = k;
        gend.count = 0;
        gend.from = (const char *)0;
        gend.index = idx;
        if (!m->caps || idx > m->ncaps) {
            return m_node(m, nd->a, position, &gend);
        }
        regmatch_t saved = m->caps[idx];
        m->caps[idx].rm_so = (regoff_t)(position - m->s);
        int stop = m_node(m, nd->a, position, &gend);
        if (!stop) {
            m->caps[idx] = saved;
        }
        return stop;
    }

    case N_BACKREF: {
        int idx = nd->n;
        if (!m->caps || idx > m->ncaps) {
            return 0;
        }
        regoff_t so = m->caps[idx].rm_so, eo = m->caps[idx].rm_eo;
        if (so < 0 || eo < so) {
            return 0;
        }
        size_t n = (size_t)(eo - so);
        if ((size_t)(end - position) < n) {
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            if (!chr_eq(m, (unsigned char)position[i], (unsigned char)m->s[so + i])) {
                return 0;
            }
        }
        return m_cont(m, k, position + n);
    }

    case N_REP:
        return m_rep(m, nd, position, k, 0, (const char *)0);

    default:
        return 0;
    }
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) {
        return REG_BADPAT;
    }
    parser_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = pattern;
    ps.cflags = cflags;

    node_t *root = parse_alt(&ps);
    if (!ps.error && *ps.p) {
        ps.error = REG_EPAREN;
    }
    if (ps.error) {
        for (size_t i = 0; i < ps.narena; i++) {
            free(ps.arena[i]);
        }
        free(ps.arena);
        return ps.error;
    }
    preg->re_nsub = (size_t)ps.ngroup;
    preg->re_cflags = cflags;
    struct prog {
        node_t *root;
        node_t **arena;
        size_t narena;
    } *prog = malloc(sizeof(*prog));
    if (!prog) {
        for (size_t i = 0; i < ps.narena; i++) {
            free(ps.arena[i]);
        }
        free(ps.arena);
        return REG_ESPACE;
    }
    prog->root = root;
    prog->arena = ps.arena;
    prog->narena = ps.narena;
    preg->re_prog = prog;
    return 0;
}

void regfree(regex_t *preg) {
    if (!preg || !preg->re_prog) {
        return;
    }
    struct prog {
        node_t *root;
        node_t **arena;
        size_t narena;
    } *prog = preg->re_prog;
    for (size_t i = 0; i < prog->narena; i++) {
        free(prog->arena[i]);
    }
    free(prog->arena);
    free(prog);
    preg->re_prog = (void *)0;
    preg->re_nsub = 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    if (!preg || !preg->re_prog || !string) {
        return REG_NOMATCH;
    }
    struct prog {
        node_t *root;
        node_t **arena;
        size_t narena;
    } *prog = preg->re_prog;

    int ngroups = (int)preg->re_nsub;
    matcher_t m;
    memset(&m, 0, sizeof(m));
    m.s = string;
    m.length = strlen(string);
    m.cflags = preg->re_cflags;
    m.eflags = eflags;
    m.ncaps = ngroups;
    m.want_longest = !(preg->re_cflags & REG_NOSUB);

    regmatch_t *live = (regmatch_t *)0, *best = (regmatch_t *)0;
    if (ngroups > 0) {
        live = malloc((size_t)(ngroups + 1) * sizeof(regmatch_t));
        best = malloc((size_t)(ngroups + 1) * sizeof(regmatch_t));
        if (!live || !best) {
            free(live);
            free(best);
            return REG_ESPACE;
        }
        m.caps = live;
        m.best_caps = best;
    }

    for (const char *start = string; start <= string + m.length; start++) {
        if (live) {
            for (int i = 0; i <= ngroups; i++) {
                live[i].rm_so = live[i].rm_eo = -1;
                best[i].rm_so = best[i].rm_eo = -1;
            }
        }
        m.best = (const char *)0;
        m_node(&m, prog->root, start, (const cont_t *)0);
        if (!m.best) {
            continue;
        }
        if (pmatch && nmatch > 0 && !(preg->re_cflags & REG_NOSUB)) {
            pmatch[0].rm_so = (regoff_t)(start - string);
            pmatch[0].rm_eo = (regoff_t)(m.best - string);
            for (size_t i = 1; i < nmatch; i++) {
                if (best && (int)i <= ngroups) {
                    pmatch[i] = best[i];
                } else {
                    pmatch[i].rm_so = pmatch[i].rm_eo = -1;
                }
            }
        }
        free(live);
        free(best);
        return 0;
    }
    free(live);
    free(best);
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *message;
    switch (errcode) {
    case 0:            message = "success"; break;
    case REG_NOMATCH:  message = "no match"; break;
    case REG_BADPAT:   message = "invalid regular expression"; break;
    case REG_ECOLLATE: message = "invalid collating element"; break;
    case REG_ECTYPE:   message = "invalid character class"; break;
    case REG_EESCAPE:  message = "trailing backslash"; break;
    case REG_ESUBREG:  message = "invalid backreference"; break;
    case REG_EBRACK:   message = "unbalanced ["; break;
    case REG_EPAREN:   message = "unbalanced ("; break;
    case REG_EBRACE:   message = "unbalanced {"; break;
    case REG_BADBR:    message = "invalid repetition count"; break;
    case REG_ERANGE:   message = "invalid range endpoint"; break;
    case REG_ESPACE:   message = "out of memory"; break;
    case REG_BADRPT:   message = "repetition with nothing to repeat"; break;
    default:           message = "unknown error"; break;
    }
    size_t length = strlen(message);
    if (errbuf && errbuf_size > 0) {
        size_t n = length < errbuf_size - 1 ? length : errbuf_size - 1;
        memcpy(errbuf, message, n);
        errbuf[n] = '\0';
    }
    return length + 1;
}
