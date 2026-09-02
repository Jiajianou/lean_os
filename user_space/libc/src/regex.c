/* user_space/libc/src/regex.c - M89
 *
 * A POSIX regular expression engine. See <regex.h> for the three
 * properties a caller has to know about - leftmost-longest semantics,
 * the exponential worst case, and what the capture set means.
 *
 * ---- the shape of it -------------------------------------------------
 *
 * Two passes. `parse` turns the pattern into a tree of nodes, and
 * `match` walks that tree against the subject with a continuation, which
 * is what makes concatenation and repetition composable without an
 * explicit stack machine.
 *
 * The continuation is the part worth understanding before changing
 * anything here. `match(node, pos, cont)` asks "can `node` match at
 * `pos`, such that whatever comes after also matches?" - and `cont` is
 * that "whatever comes after". A star node therefore tries: match one
 * more repetition and recurse, or stop here and run the continuation.
 * Without the continuation, `a*a` cannot work: the star would swallow
 * every `a` and the trailing `a` would have nothing left, with no way to
 * hand a character back.
 *
 * ---- BRE and ERE ----------------------------------------------------
 *
 * One parser, one flag. The two grammars differ in which characters are
 * special rather than in what they mean:
 *
 *   ERE:  ( ) | + ? { }        are operators; \( is a literal paren
 *   BRE:  \( \) \| \+ \? \{ \} are operators; ( is a literal paren
 *
 * so the parser asks `is_op()` rather than branching on the flag at
 * every site. `\|`, `\+` and `\?` in BRE are GNU extensions rather than
 * POSIX, and they are accepted because every BRE anything writes today
 * assumes them - sed scripts in particular.
 */
#include <regex.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- the node tree --------------------------------------------------- */

enum {
    N_CHAR,    /* one literal character */
    N_ANY,     /* . */
    N_CLASS,   /* [...] */
    N_CAT,     /* left then right */
    N_ALT,     /* left or right */
    N_REP,     /* left, repeated min..max times (max < 0 means unbounded) */
    N_GROUP,   /* a capturing ( ), index in `n` */
    N_BOL,     /* ^ */
    N_EOL,     /* $ */
    N_BACKREF, /* \1 .. \9, index in `n` */
    N_EMPTY,   /* matches nothing, successfully - an empty alternative */
};

typedef struct node {
    int type;
    int n;              /* group/backref index; the character for N_CHAR */
    int min, max;       /* N_REP */
    unsigned char set[32]; /* N_CLASS: a 256-bit membership table */
    struct node *a, *b;
} node_t;

typedef struct {
    const char *p;      /* the cursor into the pattern */
    int cflags;
    int ngroup;         /* how many capturing groups have been opened */
    int err;            /* the first error, which is the one reported */
    node_t **arena;     /* every node allocated, so regfree is one loop */
    size_t narena, arena_cap;
} parser_t;

static node_t *node_new(parser_t *ps, int type) {
    if (ps->err) {
        return (node_t *)0;
    }
    if (ps->narena == ps->arena_cap) {
        size_t cap = ps->arena_cap ? ps->arena_cap * 2 : 32;
        node_t **grown = realloc(ps->arena, cap * sizeof(node_t *));
        if (!grown) {
            ps->err = REG_ESPACE;
            return (node_t *)0;
        }
        ps->arena = grown;
        ps->arena_cap = cap;
    }
    node_t *nd = calloc(1, sizeof(node_t));
    if (!nd) {
        ps->err = REG_ESPACE;
        return (node_t *)0;
    }
    nd->type = type;
    ps->arena[ps->narena++] = nd;
    return nd;
}

/* ---- character classes ----------------------------------------------- */

static void set_add(node_t *nd, unsigned char c) {
    nd->set[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static int set_has(const node_t *nd, unsigned char c) {
    return (nd->set[c >> 3] >> (c & 7)) & 1;
}

/* The named classes POSIX puts inside brackets: [[:digit:]] and friends.
 * Recognised by name rather than by a table of characters, because
 * `isalpha` and the rest are already this libc's answer to the same
 * question and two answers would be one too many. */
static int add_named_class(node_t *nd, const char *name, size_t len) {
    static const struct { const char *name; int (*fn)(int); } table[] = {
        {"alpha", isalpha}, {"digit", isdigit},  {"alnum", isalnum},
        {"upper", isupper}, {"lower", islower},  {"space", isspace},
        {"blank", isblank}, {"punct", ispunct},  {"print", isprint},
        {"graph", isgraph}, {"cntrl", iscntrl},  {"xdigit", isxdigit},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strlen(table[i].name) != len || strncmp(table[i].name, name, len) != 0) {
            continue;
        }
        for (int c = 0; c < 256; c++) {
            if (table[i].fn(c)) {
                set_add(nd, (unsigned char)c);
            }
        }
        return 1;
    }
    return 0;
}

/* Parses a bracket expression, with the cursor just past the '['. */
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
        /* [:name:] inside the brackets. */
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            const char *start = ps->p + 2;
            const char *end = strstr(start, ":]");
            if (!end) {
                ps->err = REG_ECTYPE;
                return (node_t *)0;
            }
            if (!add_named_class(nd, start, (size_t)(end - start))) {
                ps->err = REG_ECTYPE;
                return (node_t *)0;
            }
            ps->p = end + 2;
            continue;
        }
        unsigned char lo = (unsigned char)*ps->p++;
        /* A backslash inside a bracket expression is NOT special in
         * POSIX - `[\]` is a class containing a backslash. Every real
         * program written in the last thirty years assumes the GNU
         * behaviour instead, where `[\]]` is a class containing ']', so
         * that is what this does. Written down because it is a
         * deliberate departure from the standard rather than an
         * oversight. */
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
                /* [z-a]. An error rather than an empty class, because
                 * POSIX says so and because it is almost always a typo
                 * for something the author meant to work. */
                ps->err = REG_ERANGE;
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
        ps->err = REG_EBRACK;
        return (node_t *)0;
    }
    ps->p++;
    if (ps->cflags & REG_ICASE) {
        /* Fold after building, so [a-z] also matches 'Q' and the range
         * arithmetic above stays simple. */
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
        /* REG_NEWLINE: a negated class does not match a newline, so that
         * `grep -v` style patterns cannot run past the end of a line. */
        if (ps->cflags & REG_NEWLINE) {
            nd->set['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
        }
    }
    return nd;
}

/* ---- the grammar ----------------------------------------------------- */

static node_t *parse_alt(parser_t *ps);

/* Is the character at `p` the operator `op`, given BRE or ERE spelling?
 * Returns the number of pattern bytes it occupies, or 0. */
static int is_op(parser_t *ps, const char *p, char op) {
    if (ps->cflags & REG_EXTENDED) {
        return *p == op ? 1 : 0;
    }
    return (p[0] == '\\' && p[1] == op) ? 2 : 0;
}

static node_t *parse_atom(parser_t *ps) {
    if (ps->err) {
        return (node_t *)0;
    }
    const char *p = ps->p;
    int w;

    if ((w = is_op(ps, p, '(')) != 0) {
        ps->p += w;
        int index = ++ps->ngroup;
        node_t *inner = parse_alt(ps);
        if (ps->err) {
            return (node_t *)0;
        }
        if ((w = is_op(ps, ps->p, ')')) == 0) {
            ps->err = REG_EPAREN;
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
        /* A backreference, and the reason this engine backtracks at all.
         * `\1` matches whatever group 1 matched, which no finite
         * automaton can express. */
        if (p[1] >= '1' && p[1] <= '9') {
            int index = p[1] - '0';
            if (index > ps->ngroup) {
                ps->err = REG_ESUBREG;
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
        ps->err = REG_EESCAPE; /* a trailing backslash escapes nothing */
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

/* {n}, {n,} or {n,m}. The cursor is just past the opening brace. */
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
            hi = -1; /* {n,} - unbounded */
        }
    }
    if (hi >= 0 && hi < lo) {
        return REG_BADBR; /* {3,1} */
    }
    ps->p = p;
    *min = lo;
    *max = hi;
    return 0;
}

static node_t *parse_piece(parser_t *ps) {
    node_t *atom = parse_atom(ps);
    if (ps->err) {
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
            /* BRE: `\{` always opens an interval, so one that is not
             * followed by a count is an unterminated brace rather than a
             * literal. ERE's bare `{` is different and is handled below -
             * `a{b` is a literal brace there, in this engine and in
             * every other one, and rejecting it would break patterns
             * that have worked for decades. */
            ps->err = REG_EBRACE;
            return (node_t *)0;
        } else if ((w = is_op(ps, ps->p, '{')) != 0 &&
                   isdigit((unsigned char)ps->p[w])) {
            const char *save = ps->p;
            ps->p += w;
            int e = parse_interval(ps, &min, &max);
            if (e) {
                ps->err = e;
                return (node_t *)0;
            }
            if ((w = is_op(ps, ps->p, '}')) == 0) {
                ps->p = save;
                ps->err = REG_EBRACE;
                return (node_t *)0;
            }
            ps->p += w;
        } else {
            return atom;
        }
        if (!atom) {
            ps->err = REG_BADRPT; /* `*` with nothing before it */
            return (node_t *)0;
        }
        node_t *rep = node_new(ps, N_REP);
        if (!rep) {
            return (node_t *)0;
        }
        rep->a = atom;
        rep->min = min;
        rep->max = max;
        atom = rep; /* a** is legal and means the same as a* */
    }
}

/* Is the cursor at something that ends the current branch? */
static int at_branch_end(parser_t *ps) {
    if (!*ps->p) {
        return 1;
    }
    return is_op(ps, ps->p, '|') || is_op(ps, ps->p, ')');
}

static node_t *parse_branch(parser_t *ps) {
    node_t *left = (node_t *)0;
    while (!ps->err && !at_branch_end(ps)) {
        node_t *piece;
        /* A repeat operator at the start of a branch has nothing to
         * repeat. In ERE that is an error - `*a` and `(|*)` are rejected
         * by every implementation. In BRE it is NOT: POSIX says a `*`
         * first in a pattern or just after `\(` is an ordinary
         * character, so `*a` there matches a literal asterisk. The two
         * grammars genuinely disagree and this is the one place it
         * shows. */
        if (!left && (ps->cflags & REG_EXTENDED) &&
            (*ps->p == '*' || *ps->p == '+' || *ps->p == '?')) {
            ps->err = REG_BADRPT;
            return (node_t *)0;
        }
        /* Anchors. `^` is an anchor only where a pattern can start and
         * `$` only where one can end; elsewhere both are literals, which
         * is what BRE specifies and what every ERE implementation does
         * in practice. */
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
        if (ps->err) {
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
        left = node_new(ps, N_EMPTY); /* an empty branch matches the empty string */
    }
    return left;
}

static node_t *parse_alt(parser_t *ps) {
    node_t *left = parse_branch(ps);
    int w;
    while (!ps->err && (w = is_op(ps, ps->p, '|')) != 0) {
        ps->p += w;
        node_t *right = parse_branch(ps);
        if (ps->err) {
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

/* ---- the matcher -----------------------------------------------------
 *
 * A continuation-passing backtracker. `m_node(nd, pos, k)` asks whether
 * `nd` can match at `pos` such that the continuation `k` also matches;
 * `m_cont(k, pos)` runs a continuation. The two call each other, and
 * every backtrack is an ordinary return.
 *
 * The continuation is a tagged struct rather than a bare node, because
 * two of the three things that can come next carry state a node cannot:
 * a repetition has to remember its count and where the current iteration
 * began, and a closing group has to remember which group it closes. All
 * three live on the C stack - a continuation is only ever referenced by
 * frames below the one that created it, so nothing here allocates.
 *
 * ---- how a match is chosen -------------------------------------------
 *
 * Every function returns 1 for "stop searching" and 0 for "keep going",
 * which is not the same as success and failure and is the part to read
 * twice. A complete match does NOT stop the search: POSIX wants the
 * longest match from a given start, so a match is recorded and the
 * search continues to see whether a longer one exists. The two cases
 * that genuinely stop are a match reaching the end of the subject (no
 * longer one can exist) and REG_NOSUB (the caller asked only whether it
 * matched, so the first answer is as good as the best one).
 */

typedef struct cont cont_t;

enum { K_NODE, K_REP, K_GEND };

struct cont {
    int kind;
    const node_t *node;  /* K_NODE: match this next.  K_REP: the repetition */
    const cont_t *next;
    int count;           /* K_REP: iterations so far */
    const char *from;    /* K_REP: where this iteration began - the empty-body guard */
    int index;           /* K_GEND: the group closing here */
};

typedef struct {
    const char *s;
    size_t len;
    int cflags, eflags;
    regmatch_t *caps;   /* live capture state, 1-based */
    regmatch_t *best_caps;
    int ncaps;
    const char *best;   /* longest end found from this start, or NULL */
    int want_longest;   /* 0 under REG_NOSUB - see the note above */
} matcher_t;

static int m_node(matcher_t *m, const node_t *nd, const char *pos, const cont_t *k);

static int m_cont(matcher_t *m, const cont_t *k, const char *pos);

/* A complete match. Record it if it beats what we have. */
static int m_accept(matcher_t *m, const char *pos) {
    if (!m->best || pos > m->best) {
        m->best = pos;
        if (m->best_caps && m->caps) {
            for (int i = 0; i <= m->ncaps; i++) {
                m->best_caps[i] = m->caps[i];
            }
        }
    }
    if (!m->want_longest) {
        return 1; /* the caller only asked whether it matched */
    }
    /* Nothing can beat a match that reaches the end of the subject. */
    return pos == m->s + m->len;
}

static int m_rep(matcher_t *m, const node_t *nd, const char *pos,
                 const cont_t *after, int count, const char *from) {
    /* An iteration that consumed nothing means the body matches the
     * empty string; repeating it again would not terminate. `(a*)*` is
     * the pattern that makes this necessary rather than theoretical. */
    if (from && pos == from) {
        /* The body matched the empty string. Going round again would not
         * terminate, so this is where the repetition stops - but whether
         * to run the continuation from here needs care, and getting it
         * wrong is invisible in the overall match and wrong in the
         * captures.
         *
         * At count > min, running it is REDUNDANT: the enclosing
         * iteration already tried this exact position with one fewer
         * repetition. Worse than redundant - the empty pass has just
         * written its own (empty) offsets over the group, so the path
         * that wins reports `(a*)*` against "aaab" capturing 3-3 rather
         * than 0-3. The host's engine says 0-3 and it is right.
         *
         * At count <= min it is necessary: the minimum has not been met
         * any other way, and since the body matches empty, every
         * remaining required iteration is empty too and adds nothing. */
        return count <= nd->min ? m_cont(m, after, pos) : 0;
    }
    /* Greedy: one more repetition before considering stopping. With
     * leftmost-longest the order does not change the answer, only how
     * quickly the best one is found - and finding it early is what lets
     * m_accept's end-of-subject test cut the search short. */
    if (nd->max < 0 || count < nd->max) {
        cont_t again;
        again.kind = K_REP;
        again.node = nd;
        again.next = after;
        again.count = count + 1;
        again.from = pos;
        again.index = 0;
        if (m_node(m, nd->a, pos, &again)) {
            return 1;
        }
    }
    if (count >= nd->min) {
        return m_cont(m, after, pos);
    }
    return 0;
}

static int m_cont(matcher_t *m, const cont_t *k, const char *pos) {
    if (!k) {
        return m_accept(m, pos);
    }
    switch (k->kind) {
    case K_NODE:
        return m_node(m, k->node, pos, k->next);
    case K_REP:
        return m_rep(m, k->node, pos, k->next, k->count, k->from);
    case K_GEND: {
        /* The group ends here. Written, then restored on the way out, so
         * that a path which is abandoned does not leave its offsets
         * behind for a different path to report. */
        int idx = k->index;
        if (!m->caps || idx > m->ncaps) {
            return m_cont(m, k->next, pos);
        }
        regmatch_t saved = m->caps[idx];
        m->caps[idx].rm_eo = (regoff_t)(pos - m->s);
        int stop = m_cont(m, k->next, pos);
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

static int m_node(matcher_t *m, const node_t *nd, const char *pos, const cont_t *k) {
    const char *end = m->s + m->len;
    switch (nd->type) {
    case N_EMPTY:
        return m_cont(m, k, pos);

    case N_CHAR:
        if (pos < end && chr_eq(m, (unsigned char)*pos, (unsigned char)nd->n)) {
            return m_cont(m, k, pos + 1);
        }
        return 0;

    case N_ANY:
        if (pos < end) {
            /* REG_NEWLINE: '.' does not cross a line boundary, which is
             * what keeps a line-oriented tool's pattern on one line. */
            if ((m->cflags & REG_NEWLINE) && *pos == '\n') {
                return 0;
            }
            return m_cont(m, k, pos + 1);
        }
        return 0;

    case N_CLASS:
        if (pos < end && set_has(nd, (unsigned char)*pos)) {
            return m_cont(m, k, pos + 1);
        }
        return 0;

    case N_BOL:
        if (pos == m->s) {
            return (m->eflags & REG_NOTBOL) ? 0 : m_cont(m, k, pos);
        }
        if ((m->cflags & REG_NEWLINE) && pos[-1] == '\n') {
            return m_cont(m, k, pos);
        }
        return 0;

    case N_EOL:
        if (pos == end) {
            return (m->eflags & REG_NOTEOL) ? 0 : m_cont(m, k, pos);
        }
        if ((m->cflags & REG_NEWLINE) && *pos == '\n') {
            return m_cont(m, k, pos);
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
        return m_node(m, nd->a, pos, &k2);
    }

    case N_ALT:
        /* Both branches, always - the longest wins rather than the
         * first, which is the whole of the POSIX/Perl difference. The
         * only reason to stop after the first is m_accept saying so. */
        if (m_node(m, nd->a, pos, k)) {
            return 1;
        }
        return m_node(m, nd->b, pos, k);

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
            return m_node(m, nd->a, pos, &gend);
        }
        regmatch_t saved = m->caps[idx];
        m->caps[idx].rm_so = (regoff_t)(pos - m->s);
        int stop = m_node(m, nd->a, pos, &gend);
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
            return 0; /* the group never participated */
        }
        size_t n = (size_t)(eo - so);
        if ((size_t)(end - pos) < n) {
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            if (!chr_eq(m, (unsigned char)pos[i], (unsigned char)m->s[so + i])) {
                return 0;
            }
        }
        return m_cont(m, k, pos + n);
    }

    case N_REP:
        return m_rep(m, nd, pos, k, 0, (const char *)0);

    default:
        return 0;
    }
}

/* ---- the public four -------------------------------------------------- */

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) {
        return REG_BADPAT;
    }
    parser_t ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = pattern;
    ps.cflags = cflags;

    node_t *root = parse_alt(&ps);
    if (!ps.err && *ps.p) {
        /* Something was left over, which for a well-formed pattern can
         * only be a ')' with no '(' - parse_alt stops at one. */
        ps.err = REG_EPAREN;
    }
    if (ps.err) {
        for (size_t i = 0; i < ps.narena; i++) {
            free(ps.arena[i]);
        }
        free(ps.arena);
        return ps.err;
    }
    preg->re_nsub = (size_t)ps.ngroup;
    preg->re_cflags = cflags;
    /* The arena is kept, not the tree: regfree has to free every node
     * and the tree has no parent pointers. Two allocations to remember
     * rather than a recursive walk that could be wrong about sharing. */
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
    m.len = strlen(string);
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

    /* Leftmost: try each start position in order and take the first that
     * matches at all. The longest-from-there is m_accept's job. */
    for (const char *start = string; start <= string + m.len; start++) {
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
    const char *msg;
    switch (errcode) {
    case 0:            msg = "success"; break;
    case REG_NOMATCH:  msg = "no match"; break;
    case REG_BADPAT:   msg = "invalid regular expression"; break;
    case REG_ECOLLATE: msg = "invalid collating element"; break;
    case REG_ECTYPE:   msg = "invalid character class"; break;
    case REG_EESCAPE:  msg = "trailing backslash"; break;
    case REG_ESUBREG:  msg = "invalid backreference"; break;
    case REG_EBRACK:   msg = "unbalanced ["; break;
    case REG_EPAREN:   msg = "unbalanced ("; break;
    case REG_EBRACE:   msg = "unbalanced {"; break;
    case REG_BADBR:    msg = "invalid repetition count"; break;
    case REG_ERANGE:   msg = "invalid range endpoint"; break;
    case REG_ESPACE:   msg = "out of memory"; break;
    case REG_BADRPT:   msg = "repetition with nothing to repeat"; break;
    default:           msg = "unknown error"; break;
    }
    size_t len = strlen(msg);
    if (errbuf && errbuf_size > 0) {
        size_t n = len < errbuf_size - 1 ? len : errbuf_size - 1;
        memcpy(errbuf, msg, n);
        errbuf[n] = '\0';
    }
    return len + 1; /* the size a buffer would need, terminator included */
}
