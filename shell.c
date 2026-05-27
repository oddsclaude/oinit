/*
 * oinit shell - minimal POSIX-ish shell
 */
#include "oinit.h"
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/resource.h>
#include <regex.h>

/* ====== types ====== */

enum toktype {
    TT_EOF=0, TT_NEWLINE, TT_SEMI, TT_AMP, TT_PIPE,
    TT_ANDAND, TT_OROR, TT_BANG, TT_DSEMI,
    TT_LESS, TT_GREAT, TT_DLESS, TT_DGREAT,
    TT_LESSAMP, TT_GREATAMP, TT_LESSGREAT, TT_DGREATAMP,
    TT_LPAREN, TT_RPAREN, TT_LBRACE, TT_RBRACE,
    TT_WORD, TT_ASSIGN,
    /* keywords */
    TT_IF, TT_THEN, TT_ELIF, TT_ELSE, TT_FI,
    TT_WHILE, TT_UNTIL, TT_DO, TT_DONE,
    TT_FOR, TT_IN, TT_CASE, TT_ESAC, TT_FUNCTION,
    TT_SELECT,
};

enum nodtype {
    NN_LIST,    /* list of commands, separator stored */
    NN_PIPE,    /* pipeline */
    NN_CMD,     /* simple command */
    NN_IF,      NN_WHILE, NN_UNTIL, NN_FOR, NN_CASE,
    NN_FUNC,    NN_SUBSH, NN_BRACE,
    NN_REDIR,
    NN_BANG,
};

struct tok {
    enum toktype type;
    char *text;     /* heap-allocated word text or NULL */
    int   heredoc;  /* 1 if << heredoc */
};

struct redir {
    int      fd;    /* fd to redirect (-1 = default) */
    enum toktype op; /* TT_LESS, TT_GREAT, etc. */
    char    *word;
    struct redir *next;
};

struct wordlist {
    char **w;
    int    n, cap;
};

struct node {
    enum nodtype type;
    /* NN_CMD */
    struct wordlist args;
    char **assign;  /* var=val pairs, NULL-terminated */
    int    nassign;
    struct redir *redirs;
    /* NN_LIST */
    struct node *left, *right;
    int    sep;     /* TT_SEMI, TT_AMP, TT_ANDAND, TT_OROR */
    /* NN_IF */
    struct node *cond, *body, *elsebody;
    /* NN_WHILE/UNTIL */
    /* cond, body */
    /* NN_FOR */
    char *forvar;
    struct wordlist forwords;
    /* NN_CASE */
    char *caseword;
    struct caseitem *cases;
    /* NN_FUNC */
    char *funcname;
    /* body for func/while/until/for/if, subsh, brace */
    /* NN_PIPE */
    struct node **pipes;
    int npipes;
    int negate; /* ! */
};

struct caseitem {
    char **patterns;
    int npatterns;
    struct node *body;
    struct caseitem *next;
};

/* ====== shell state ====== */

struct shfunc {
    char *name;
    struct node *body;
};

#define MAXDEPTH 512
#define MAXLOCALS 256

static int   last_status = 0;
static pid_t last_bg = 0;
static int   sh_interactive = 0;
static int   sh_exit_code = 0;
static int   sh_exiting = 0;
static jmp_buf break_jmp, continue_jmp, return_jmp;
static int     in_loop = 0, in_func = 0;
static int     break_count = 0, continue_count = 0;

/* functions */
static struct shfunc *sh_funcs = NULL;
static int sh_nfuncs = 0;

/* ====== environment ====== */

extern char **environ;

static char *sh_get(const char *name) {
    return getenv(name);
}
static void sh_set(const char *name, const char *val) {
    setenv(name, val, 1);
}
static void sh_unset(const char *name) {
    unsetenv(name);
}

/* ====== input ====== */

struct input {
    char *buf;
    int   pos, len;
    FILE *fp;
    const char *str;
    int   interactive;
    int   eof;
    char *hist_last;
};


static int inp_getc(struct input *in) {
    if (in->pos < in->len) return (unsigned char)in->buf[in->pos++];
    if (in->eof) return EOF;
    if (in->str) { in->eof = 1; return EOF; }
    if (in->fp) {
        if (in->interactive) {
            const char *ps = in->hist_last ? sh_get("PS2") : sh_get("PS1");
            if (!ps) ps = in->hist_last ? "> " : "$ ";
            fputs(ps, stdout);
            fflush(stdout);
            in->hist_last = NULL;
        }
        char tmp[4096];
        if (!fgets(tmp, sizeof(tmp), in->fp)) { in->eof = 1; return EOF; }
        free(in->buf);
        in->buf = xstrdup(tmp);
        in->len = strlen(in->buf);
        in->pos = 0;
        return (unsigned char)in->buf[in->pos++];
    }
    return EOF;
}

static void inp_ungetc(struct input *in) {
    if (in->pos > 0) in->pos--;
}

/* ====== lexer ====== */

static struct tok *tok_buf = NULL;
static int tok_n = 0, tok_cap = 0;

static void tok_push(struct tok t) {
    if (tok_n >= tok_cap) {
        tok_cap = tok_cap ? tok_cap*2 : 16;
        tok_buf = xrealloc(tok_buf, tok_cap * sizeof(*tok_buf));
    }
    tok_buf[tok_n++] = t;
}

static void tok_free(struct tok *t) { free(t->text); t->text = NULL; }

/* string builder */
struct sb { char *s; int n, cap; };
static void sb_add(struct sb *b, char c) {
    if (b->n >= b->cap) { b->cap = b->cap ? b->cap*2 : 64; b->s = xrealloc(b->s, b->cap+1); }
    b->s[b->n++] = c;
}
static void sb_adds(struct sb *b, const char *s) { while (*s) sb_add(b, *s++); }
static char *sb_done(struct sb *b) { sb_add(b, '\0'); char *r=b->s; *b=(struct sb){0}; return r; }
static void sb_free(struct sb *b) { free(b->s); *b=(struct sb){0}; }

/* expand $var, ${var}, $(), $(()), ` ` in a double-quoted context */
static char *expand_word(const char *s, char **posparams, int npos);
static char *expand_arith(const char *expr);

static char *expand_word(const char *s, char **posparams, int npos) {
    struct sb out = {0};
    const char *p = s;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (*p) sb_add(&out, *p++);
            continue;
        }
        if (*p != '$' && *p != '`') { sb_add(&out, *p++); continue; }
        if (*p == '`') {
            p++;
            struct sb sub = {0};
            while (*p && *p != '`') { sb_add(&sub, *p++); }
            if (*p == '`') p++;
            sb_add(&sub, '\0');
            /* run subcommand */
            FILE *f = popen(sub.s, "r"); free(sub.s);
            if (f) {
                char tmp[256]; int n;
                while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
                    for (int i=0;i<n;i++) if (tmp[i]!='\n'||i<n-1) sb_add(&out,tmp[i]);
                }
                pclose(f);
            }
            continue;
        }
        /* $ */
        p++;
        if (*p == '(') {
            p++;
            if (*p == '(') {
                /* arithmetic $((expr)) */
                p++;
                struct sb expr = {0};
                int depth = 0;
                while (*p) {
                    if (*p == '(') { depth++; sb_add(&expr, *p++); }
                    else if (*p == ')') {
                        if (depth > 0) { depth--; sb_add(&expr, *p++); }
                        else break; /* closing first ) of )) */
                    } else sb_add(&expr, *p++);
                }
                if (*p == ')') p++; /* skip )) */
                if (*p == ')') p++;
                sb_add(&expr, '\0');
                char *val = expand_arith(expr.s); free(expr.s);
                sb_adds(&out, val ? val : "0"); free(val);
            } else {
                /* command substitution $( ) */
                struct sb sub = {0};
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { if (--depth == 0) break; }
                    sb_add(&sub, *p++);
                }
                if (*p == ')') p++;
                sb_add(&sub, '\0');
                FILE *f = popen(sub.s, "r"); free(sub.s);
                if (f) {
                    char tmp[256]; int n;
                    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
                        for (int i=0;i<n;i++) { if(tmp[i]=='\n'&&i==n-1) break; sb_add(&out,tmp[i]); }
                    }
                    pclose(f);
                }
            }
            continue;
        }
        if (*p == '{') {
            p++;
            /* read until } */
            struct sb var = {0};
            while (*p && *p != '}') sb_add(&var, *p++);
            if (*p == '}') p++;
            sb_add(&var, '\0');
            char *vn = var.s;
            /* operators */
            char op = 0;
            int hash = 0;
            char *varg = NULL;
            if (*vn == '#' && vn[1] && !strchr("}", vn[1])) { hash = 1; vn++; }
            char *op_pos = strpbrk(vn, ":-:+:?:=:%#");
            if (op_pos && op_pos != vn) {
                op = *op_pos;
                if (op == '%' || op == '#') {
                    char double_op = (op_pos[1] == op) ? op : 0;
                    varg = op_pos + 1 + (double_op ? 1 : 0);
                    op = double_op ? (char)(op | 0x80) : op;
                } else {
                    char *nullterm = op_pos; /* null-terminate varname here */
                    int colon = (op == ':');
                    if (colon) op_pos++;
                    op = *op_pos;
                    varg = op_pos + 1;
                    *nullterm = '\0'; /* truncate varname at : or operator */
                    goto op_parsed;
                }
                *op_pos = '\0';
                op_parsed:;
            }
            /* get value */
            char *val = NULL;
            if (strcmp(vn, "?") == 0) val = xasprintf("%d", last_status);
            else if (strcmp(vn, "$") == 0) val = xasprintf("%d", (int)getpid());
            else if (strcmp(vn, "!") == 0) val = xasprintf("%d", (int)last_bg);
            else if (strcmp(vn, "#") == 0) val = xasprintf("%d", npos);
            else if (strcmp(vn, "-") == 0) val = xstrdup("");
            else if (isdigit((unsigned char)*vn)) {
                int idx = atoi(vn);
                if (idx >= 0 && idx < npos && posparams) val = xstrdup(posparams[idx]);
            } else if (strcmp(vn, "@") == 0 || strcmp(vn, "*") == 0) {
                struct sb all = {0};
                for (int i = 1; i < npos && posparams; i++) {
                    if (i > 1) sb_add(&all, ' ');
                    sb_adds(&all, posparams[i]);
                }
                val = sb_done(&all);
            } else {
                char *e = sh_get(vn);
                if (e) val = xstrdup(e);
            }
            if (hash) {
                int len = val ? (int)strlen(val) : 0;
                free(val); val = xasprintf("%d", len);
                sb_adds(&out, val); free(val); free(var.s); continue;
            }
            char op_real = op & 0x7f;
            int op_double = (op & 0x80) != 0;
            if (op_real == '-' || op_real == '=') {
                if (!val || !*val) {
                    if (varg) { char *exp = expand_word(varg, posparams, npos); free(val); val = exp; }
                    if (op_real == '=' && vn[0] != '$') sh_set(vn, val ? val : "");
                }
            } else if (op_real == '+') {
                if (val && *val) { free(val); val = varg ? expand_word(varg, posparams, npos) : xstrdup(""); }
                else { free(val); val = NULL; }
            } else if (op_real == '?') {
                if (!val || !*val) {
                    fprintf(stderr, "%s: %s\n", vn, varg ? varg : "parameter null or not set");
                    free(val); free(var.s); exit(1);
                }
            } else if (op_real == '%') {
                if (val && varg) {
                    /* suffix strip */
                    char *pat = expand_word(varg, posparams, npos);
                    if (op_double) {
                        /* longest match from end */
                        int l = strlen(val);
                        for (int i = 0; i <= l; i++) {
                            if (fnmatch(pat, val+i, 0) == 0) { char *nv = xstrdup(val+i); free(val); val = nv; break; }
                        }
                    } else {
                        for (int i = strlen(val); i >= 0; i--) {
                            char save = val[i]; val[i] = '\0';
                            int m = fnmatch(pat, val, 0);
                            val[i] = save;
                            if (m == 0) {
                                break;
                            }
                        }
                        /* redo: % strips shortest suffix */
                        int l = strlen(val);
                        for (int i = l; i >= 0; i--) {
                            if (fnmatch(pat, val+i, 0) == 0) { val[i] = '\0'; break; }
                        }
                    }
                    free(pat);
                }
            } else if (op_real == '#') {
                if (val && varg) {
                    char *pat = expand_word(varg, posparams, npos);
                    int l = strlen(val);
                    if (op_double) {
                        for (int i = l; i >= 0; i--) {
                            char save = val[i]; val[i] = '\0';
                            int m = fnmatch(pat, val, 0);
                            val[i] = save;
                            if (m == 0) { char *nv = xstrdup(val+i); free(val); val = nv; break; }
                        }
                    } else {
                        for (int i = 0; i <= l; i++) {
                            char save = val[i]; val[i] = '\0';
                            int m = fnmatch(pat, val, 0);
                            val[i] = save;
                            if (m == 0) { char *nv = xstrdup(val+i); free(val); val = nv; break; }
                        }
                    }
                    free(pat);
                }
            }
            sb_adds(&out, val ? val : "");
            free(val); free(var.s);
            continue;
        }
        /* plain $var */
        if (*p == '?') { char *v=xasprintf("%d",last_status); sb_adds(&out,v); free(v); p++; continue; }
        if (*p == '$') { char *v=xasprintf("%d",(int)getpid()); sb_adds(&out,v); free(v); p++; continue; }
        if (*p == '!') { char *v=xasprintf("%d",(int)last_bg); sb_adds(&out,v); free(v); p++; continue; }
        if (*p == '#') { char *v=xasprintf("%d",npos); sb_adds(&out,v); free(v); p++; continue; }
        if (*p == '@' || *p == '*') {
            for (int i=1; i<npos && posparams; i++) {
                if (i>1) sb_add(&out,' ');
                sb_adds(&out, posparams[i]);
            }
            p++; continue;
        }
        if (isdigit((unsigned char)*p)) {
            int idx = *p - '0'; p++;
            if (idx >= 0 && idx < npos && posparams) sb_adds(&out, posparams[idx]);
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            struct sb var = {0};
            while (isalnum((unsigned char)*p) || *p == '_') sb_add(&var, *p++);
            sb_add(&var, '\0');
            char *val = sh_get(var.s);
            if (val) sb_adds(&out, val);
            free(var.s);
            continue;
        }
        sb_add(&out, '$');
    }
    return sb_done(&out);
}

/* arithmetic evaluator */
static long arith_expr(const char **p);
static long arith_primary(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
    long v;
    if (**p == '(') {
        (*p)++;
        v = arith_expr(p);
        while (**p == ' ' || **p == '\t') (*p)++;
        if (**p == ')') (*p)++;
        return v;
    }
    if (**p == '-') { (*p)++; return -arith_primary(p); }
    if (**p == '+') { (*p)++; return  arith_primary(p); }
    if (**p == '!') { (*p)++; return !arith_primary(p); }
    if (**p == '~') { (*p)++; return ~arith_primary(p); }
    /* bare variable name */
    if (isalpha((unsigned char)**p) || **p == '_') {
        const char *start = *p;
        while (isalnum((unsigned char)**p) || **p == '_') (*p)++;
        char *name = xstrndup(start, *p - start);
        const char *val = sh_get(name); free(name);
        if (val && *val) { const char *vp = val; return arith_expr(&vp); }
        return 0;
    }
    char *end;
    v = strtol(*p, &end, 0);
    *p = end;
    return v;
}
static long arith_mul(const char **p) {
    long v = arith_primary(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '*' || **p == '/' || **p == '%') {
        char op = *(*p)++;
        long r = arith_primary(p);
        if (op == '*') v *= r;
        else if (op == '/') v = r ? v/r : 0;
        else v = r ? v%r : 0;
        while (**p == ' ' || **p == '\t') (*p)++;
    }
    return v;
}
static long arith_add(const char **p) {
    long v = arith_mul(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '+' || (**p == '-' && (*p)[1] != '-')) {
        char op = *(*p)++;
        long r = arith_mul(p);
        v = op == '+' ? v+r : v-r;
        while (**p == ' ' || **p == '\t') (*p)++;
    }
    return v;
}
static long arith_shift(const char **p) {
    long v = arith_add(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while ((**p == '<' && (*p)[1] == '<') || (**p == '>' && (*p)[1] == '>')) {
        int left = **p == '<'; (*p) += 2;
        long r = arith_add(p);
        v = left ? v<<r : v>>r;
        while (**p == ' ' || **p == '\t') (*p)++;
    }
    return v;
}
static long arith_cmp(const char **p) {
    long v = arith_shift(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    for (;;) {
        if (**p == '<' && (*p)[1] == '=') { (*p)+=2; v = v <= arith_shift(p); }
        else if (**p == '>' && (*p)[1] == '=') { (*p)+=2; v = v >= arith_shift(p); }
        else if (**p == '<' && (*p)[1] != '<') { (*p)++; v = v < arith_shift(p); }
        else if (**p == '>' && (*p)[1] != '>') { (*p)++; v = v > arith_shift(p); }
        else break;
        while (**p == ' ' || **p == '\t') (*p)++;
    }
    return v;
}
static long arith_eq(const char **p) {
    long v = arith_cmp(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    for (;;) {
        if (**p == '=' && (*p)[1] == '=') { (*p)+=2; v = v == arith_cmp(p); }
        else if (**p == '!' && (*p)[1] == '=') { (*p)+=2; v = v != arith_cmp(p); }
        else break;
        while (**p == ' ' || **p == '\t') (*p)++;
    }
    return v;
}
static long arith_and(const char **p) {
    long v = arith_eq(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '&' && (*p)[1] != '&') { (*p)++; v &= arith_eq(p); while(**p==' '||**p=='\t')(*p)++; }
    return v;
}
static long arith_or(const char **p) {
    long v = arith_and(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '|' && (*p)[1] != '|') { (*p)++; v |= arith_and(p); while(**p==' '||**p=='\t')(*p)++; }
    return v;
}
static long arith_land(const char **p) {
    long v = arith_or(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '&' && (*p)[1] == '&') { (*p)+=2; long r=arith_or(p); v = v && r; while(**p==' '||**p=='\t')(*p)++; }
    return v;
}
static long arith_lor(const char **p) {
    long v = arith_land(p);
    while (**p == ' ' || **p == '\t') (*p)++;
    while (**p == '|' && (*p)[1] == '|') { (*p)+=2; long r=arith_land(p); v = v || r; while(**p==' '||**p=='\t')(*p)++; }
    return v;
}
static long arith_expr(const char **p) {
    return arith_lor(p);
}
static char *expand_arith(const char *expr) {
    char *e = expand_word(expr, NULL, 0);
    const char *p = e;
    long val = arith_expr(&p);
    free(e);
    return xasprintf("%ld", val);
}

/* expand a word (not in quotes): tilde, $, glob, word-split */
static void expand_into(const char *word, struct wordlist *wl, char **pos, int npos, int do_split, int do_glob);

static void wl_add(struct wordlist *wl, char *s) {
    if (wl->n >= wl->cap) { wl->cap = wl->cap ? wl->cap*2 : 8; wl->w = xrealloc(wl->w, (wl->cap+1)*sizeof(char*)); }
    wl->w[wl->n++] = s;
    wl->w[wl->n] = NULL;
}

static void expand_into(const char *word, struct wordlist *wl, char **pos, int npos, int do_split, int do_glob) {
    /* tilde expansion */
    struct sb out = {0};
    const char *p = word;
    if (*p == '~') {
        p++;
        if (!*p || *p=='/' || *p==':') {
            const char *home = sh_get("HOME");
            if (!home) home = "/root";
            sb_adds(&out, home);
        } else {
            /* ~user: look up in passwd */
            struct sb un = {0};
            while (*p && *p != '/' && *p != ':') sb_add(&un, *p++);
            sb_add(&un, '\0');
            struct passwd *pw = getpwnam(un.s);
            free(un.s);
            if (pw) sb_adds(&out, pw->pw_dir);
            else { sb_add(&out, '~'); /* restore */ }
        }
    }
    /* expand rest */
    while (*p) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') sb_add(&out, *p++);
            if (*p == '\'') p++;
        } else if (*p == '"') {
            p++;
            struct sb dq = {0};
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1]=='"'||p[1]=='\\'||p[1]=='$'||p[1]=='`'||p[1]=='\n')) {
                    p++;
                    if (*p != '\n') sb_add(&dq, *p);
                    p++;
                } else {
                    sb_add(&dq, *p++);
                }
            }
            if (*p == '"') p++;
            sb_add(&dq, '\0');
            char *exp = expand_word(dq.s, pos, npos);
            free(dq.s);
            sb_adds(&out, exp);
            free(exp);
        } else if (*p == '\\') {
            p++;
            if (*p && *p != '\n') sb_add(&out, *p++);
            else if (*p == '\n') p++;
        } else if (*p == '$') {
            /* embed ${...} or $var etc. in out via expand_word */
            /* collect the $... token */
            struct sb dollar = {0};
            sb_add(&dollar, '$');
            p++;
            if (*p == '{') {
                sb_add(&dollar, '{'); p++;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) { sb_add(&dollar, '}'); p++; break; } }
                    sb_add(&dollar, *p++);
                }
            } else if (*p == '(') {
                sb_add(&dollar, '('); p++;
                if (*p == '(') { sb_add(&dollar, '('); p++; int d=2; while(*p&&d>0){if(*p=='(')d++;else if(*p==')')d--;sb_add(&dollar,*p++);}  }
                else { int d=1; while(*p&&d>0){if(*p=='(')d++;else if(*p==')')d--;sb_add(&dollar,*p++);} }
            } else {
                while (isalnum((unsigned char)*p) || *p=='_' || *p=='?'||*p=='$'||*p=='!'||*p=='#'||*p=='@'||*p=='*'||isdigit((unsigned char)*p)) {
                    sb_add(&dollar, *p++);
                    if (*(p-1)=='?'||*(p-1)=='$'||*(p-1)=='!'||*(p-1)=='#'||*(p-1)=='@'||*(p-1)=='*'||isdigit((unsigned char)*(p-1))) break;
                }
            }
            sb_add(&dollar, '\0');
            char *exp = expand_word(dollar.s, pos, npos);
            free(dollar.s);
            if (do_split) {
                /* word-split on IFS */
                const char *ifs = sh_get("IFS");
                if (!ifs) ifs = " \t\n";
                char *ep = exp, *tok;
                while ((tok = strsep(&ep, ifs)) != NULL) {
                    if (*tok) {
                        if (out.n) { wl_add(wl, sb_done(&out)); }
                        wl_add(wl, xstrdup(tok));
                    }
                }
                free(exp);
            } else {
                sb_adds(&out, exp);
                free(exp);
            }
        } else {
            sb_add(&out, *p++);
        }
    }
    char *result = sb_done(&out);
    if (do_glob && strpbrk(result, "*?[")) {
        glob_t g;
        if (glob(result, GLOB_NOCHECK|GLOB_TILDE, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) wl_add(wl, xstrdup(g.gl_pathv[i]));
            globfree(&g);
        } else {
            wl_add(wl, result); result = NULL;
        }
        free(result);
    } else {
        if (*result || !do_split) wl_add(wl, result);
        else free(result);
    }
}

/* ====== tokenizer ====== */

static struct tok lex_one(struct input *in) {
    struct tok t = {0};
    int c;
retry:
    c = inp_getc(in);
    if (c == EOF) { t.type = TT_EOF; return t; }
    /* skip whitespace (not newline) */
    if (c == ' ' || c == '\t') goto retry;
    /* comment */
    if (c == '#') { while ((c=inp_getc(in)) != EOF && c != '\n'); if (c=='\n'){t.type=TT_NEWLINE;return t;} t.type=TT_EOF; return t; }
    /* newline */
    if (c == '\n') { t.type = TT_NEWLINE; return t; }
    if (c == ';') { int d=inp_getc(in); if(d==';'){t.type=TT_DSEMI;return t;} inp_ungetc(in); t.type=TT_SEMI; return t; }
    if (c == '(') { t.type = TT_LPAREN; return t; }
    if (c == ')') { t.type = TT_RPAREN; return t; }
    if (c == '{') { t.type = TT_LBRACE; return t; }
    if (c == '}') { t.type = TT_RBRACE; return t; }
    if (c == '!') { t.type = TT_BANG; return t; }
    if (c == '|') {
        int d = inp_getc(in);
        if (d == '|') { t.type = TT_OROR; return t; }
        inp_ungetc(in); t.type = TT_PIPE; return t;
    }
    if (c == '&') {
        int d = inp_getc(in);
        if (d == '&') { t.type = TT_ANDAND; return t; }
        if (d == '>') {
            int e = inp_getc(in);
            if (e == '>') { t.type = TT_DGREATAMP; return t; }
            inp_ungetc(in); t.type = TT_GREATAMP; return t; }
        inp_ungetc(in); t.type = TT_AMP; return t;
    }
    if (c == '<') {
        int d = inp_getc(in);
        if (d == '<') { t.type = TT_DLESS; return t; }
        if (d == '&') { t.type = TT_LESSAMP; return t; }
        if (d == '>') { t.type = TT_LESSGREAT; return t; }
        inp_ungetc(in); t.type = TT_LESS; return t;
    }
    if (c == '>') {
        int d = inp_getc(in);
        if (d == '>') { t.type = TT_DGREAT; return t; }
        if (d == '&') { t.type = TT_GREATAMP; return t; }
        inp_ungetc(in); t.type = TT_GREAT; return t;
    }
    /* word */
    struct sb word = {0};
    int in_sq = 0, in_dq = 0, brace_depth = 0, paren_depth = 0, in_btick = 0;
    while (1) {
        if (c == EOF) break;
        if (!in_sq && !in_dq && brace_depth == 0 && paren_depth == 0 && !in_btick) {
            if (c==' '||c=='\t'||c=='\n'||c==';'||c=='('||c==')'||c=='{'||c=='}'||c=='|'||c=='&'||c=='<'||c=='>') {
                inp_ungetc(in); break;
            }
        }
        if (c == '\'' && !in_dq && brace_depth == 0 && paren_depth == 0 && !in_btick) { in_sq = !in_sq; sb_add(&word, '\''); c = inp_getc(in); continue; }
        if (c == '"'  && !in_sq && brace_depth == 0 && paren_depth == 0 && !in_btick) { in_dq = !in_dq; sb_add(&word, '"');  c = inp_getc(in); continue; }
        if (c == '\\' && !in_sq) {
            sb_add(&word, '\\');
            c = inp_getc(in);
            if (c != EOF) { sb_add(&word, c); c = inp_getc(in); }
            continue;
        }
        /* backtick command substitution */
        if (!in_sq && !in_dq && c == '`') {
            in_btick = !in_btick; sb_add(&word, c); c = inp_getc(in); continue;
        }
        /* track ${...} and $(...) depth */
        if (!in_sq) {
            if (c == '$') {
                sb_add(&word, c); c = inp_getc(in);
                if (c == '{') { brace_depth++; sb_add(&word, c); c = inp_getc(in); continue; }
                if (c == '(') { paren_depth++; sb_add(&word, c); c = inp_getc(in); continue; }
                continue;
            }
            if (c == '{' && brace_depth > 0) { brace_depth++; }
            if (c == '}' && brace_depth > 0) { brace_depth--; }
            if (c == '(' && paren_depth > 0) { paren_depth++; }
            if (c == ')' && paren_depth > 0) { paren_depth--; }
        }
        sb_add(&word, c);
        c = inp_getc(in);
    }
    t.type = TT_WORD;
    t.text = sb_done(&word);
    /* detect keywords */
    static const struct { const char *s; enum toktype t; } kws[] = {
        {"if",TT_IF},{"then",TT_THEN},{"elif",TT_ELIF},{"else",TT_ELSE},{"fi",TT_FI},
        {"while",TT_WHILE},{"until",TT_UNTIL},{"do",TT_DO},{"done",TT_DONE},
        {"for",TT_FOR},{"in",TT_IN},{"case",TT_CASE},{"esac",TT_ESAC},
        {"function",TT_FUNCTION},{NULL,0}
    };
    for (int i = 0; kws[i].s; i++) {
        if (strcmp(t.text, kws[i].s) == 0) { t.type = kws[i].t; break; }
    }
    /* assignment? VAR=... */
    if (t.type == TT_WORD) {
        char *eq = strchr(t.text, '=');
        if (eq && eq != t.text) {
            int is_assign = 1;
            for (char *q = t.text; q < eq; q++) {
                if (!isalnum((unsigned char)*q) && *q != '_') { is_assign = 0; break; }
            }
            if (is_assign) t.type = TT_ASSIGN;
        }
    }
    return t;
}

/* ====== parser ====== */

static struct tok *gtok = NULL;
static int gtok_n = 0, gtok_pos2 = 0;

static struct tok *next_tok(void);
static struct tok *peek_tok(void);
static void consume(void);

#define MAX_TOKENS 16384
static struct tok token_arena[MAX_TOKENS];
static int token_arena_n = 0;

static void tokenize_all(struct input *in) {
    token_arena_n = 0;
    gtok_pos2 = 0;
    while (1) {
        struct tok t = lex_one(in);
        token_arena[token_arena_n++] = t;
        if (t.type == TT_EOF || token_arena_n >= MAX_TOKENS-1) break;
    }
    gtok = token_arena;
    gtok_n = token_arena_n;
}

static struct tok *peek_tok(void) {
    return &gtok[gtok_pos2 < gtok_n ? gtok_pos2 : gtok_n-1];
}

static struct tok *next_tok(void) {
    struct tok *t = peek_tok();
    if (gtok_pos2 < gtok_n) gtok_pos2++;
    return t;
}

static void consume(void) { next_tok(); }

static void skip_newlines(void) {
    while (peek_tok()->type == TT_NEWLINE) consume();
}

static struct node *parse_list(void);
static struct node *parse_and_or(void);
static struct node *parse_pipeline(void);
static struct node *parse_command(void);
static struct node *parse_simple_cmd(void);
static struct node *parse_compound_cmd(void);
static struct node *new_node(enum nodtype t) {
    struct node *n = xcalloc(1, sizeof(*n));
    n->type = t;
    return n;
}

static struct node *parse_list(void) {
    skip_newlines();
    struct node *n = parse_and_or();
    while (1) {
        struct tok *t = peek_tok();
        if (t->type == TT_SEMI || t->type == TT_AMP || t->type == TT_NEWLINE) {
            int sep = t->type;
            consume(); skip_newlines();
            struct tok *t2 = peek_tok();
            if (t2->type == TT_EOF || t2->type == TT_RBRACE ||
                t2->type == TT_DONE || t2->type == TT_FI ||
                t2->type == TT_ESAC || t2->type == TT_ELIF ||
                t2->type == TT_ELSE || t2->type == TT_DO ||
                t2->type == TT_THEN || t2->type == TT_DSEMI) break;
            struct node *right = parse_and_or();
            if (!right) break;
            struct node *list = new_node(NN_LIST);
            list->left = n; list->right = right; list->sep = sep;
            n = list;
        } else break;
    }
    return n;
}

static struct node *parse_and_or(void) {
    struct node *n = parse_pipeline();
    while (1) {
        struct tok *t = peek_tok();
        if (t->type != TT_ANDAND && t->type != TT_OROR) break;
        int op = t->type; consume(); skip_newlines();
        struct node *right = parse_pipeline();
        struct node *ao = new_node(NN_LIST);
        ao->left = n; ao->right = right; ao->sep = op;
        n = ao;
    }
    return n;
}

static struct node *parse_pipeline(void) {
    int negate = 0;
    if (peek_tok()->type == TT_BANG) { consume(); negate = 1; }
    struct node **cmds = NULL;
    int ncmds = 0, cap = 0;
    do {
        skip_newlines();
        struct node *c = parse_command();
        if (!c) break;
        if (ncmds >= cap) { cap = cap ? cap*2 : 4; cmds = xrealloc(cmds, cap*sizeof(*cmds)); }
        cmds[ncmds++] = c;
    } while (peek_tok()->type == TT_PIPE && (consume(), 1));
    if (ncmds == 1 && !negate) { struct node *only = cmds[0]; free(cmds); return only; }
    if (ncmds == 0) { free(cmds); return NULL; }
    if (ncmds == 1) {
        struct node *p = new_node(NN_BANG);
        p->body = cmds[0]; p->negate = negate;
        free(cmds); return p;
    }
    struct node *p = new_node(NN_PIPE);
    p->pipes = cmds; p->npipes = ncmds; p->negate = negate;
    return p;
}

static struct redir *parse_redir(void) {
    struct tok *t = peek_tok();
    if (t->type != TT_LESS && t->type != TT_GREAT && t->type != TT_DLESS &&
        t->type != TT_DGREAT && t->type != TT_LESSAMP && t->type != TT_GREATAMP &&
        t->type != TT_LESSGREAT && t->type != TT_DGREATAMP) return NULL;
    struct redir *r = xcalloc(1, sizeof(*r));
    r->fd = -1; r->op = t->type; consume();
    struct tok *w = next_tok();
    r->word = w->text ? xstrdup(w->text) : xstrdup("");
    return r;
}

static struct node *parse_command(void) {
    struct tok *t = peek_tok();
    if (t->type == TT_LBRACE) { consume(); skip_newlines(); struct node *n=new_node(NN_BRACE); n->body=parse_list(); skip_newlines(); if(peek_tok()->type==TT_RBRACE)consume(); return n; }
    if (t->type == TT_LPAREN) { consume(); skip_newlines(); struct node *n=new_node(NN_SUBSH); n->body=parse_list(); skip_newlines(); if(peek_tok()->type==TT_RPAREN)consume(); return n; }
    if (t->type == TT_IF)     return parse_compound_cmd();
    if (t->type == TT_WHILE)  return parse_compound_cmd();
    if (t->type == TT_UNTIL)  return parse_compound_cmd();
    if (t->type == TT_FOR)    return parse_compound_cmd();
    if (t->type == TT_CASE)   return parse_compound_cmd();
    if (t->type == TT_FUNCTION) {
        consume();
        struct tok *name = next_tok();
        skip_newlines();
        /* optional () */
        if (peek_tok()->type == TT_LPAREN) { consume(); if(peek_tok()->type==TT_RPAREN)consume(); }
        skip_newlines();
        struct node *body = parse_command();
        struct node *fn = new_node(NN_FUNC);
        fn->funcname = xstrdup(name->text ? name->text : "");
        fn->body = body;
        return fn;
    }
    /* function definition: name() { ... } */
    if (t->type == TT_WORD) {
        int saved = gtok_pos2;
        consume(); /* name */
        if (peek_tok()->type == TT_LPAREN) {
            consume();
            if (peek_tok()->type == TT_RPAREN) {
                consume(); skip_newlines();
                struct node *body = parse_command();
                struct node *fn = new_node(NN_FUNC);
                fn->funcname = xstrdup(t->text ? t->text : "");
                fn->body = body;
                return fn;
            }
        }
        gtok_pos2 = saved;
    }
    return parse_simple_cmd();
}

static struct node *parse_compound_cmd(void) {
    struct tok *t = peek_tok();
    if (t->type == TT_IF) {
        consume(); skip_newlines();
        struct node *n = new_node(NN_IF);
        n->cond = parse_list();
        skip_newlines(); if(peek_tok()->type==TT_THEN)consume();
        skip_newlines(); n->body = parse_list();
        skip_newlines();
        struct node *cur = n;
        while (peek_tok()->type == TT_ELIF) {
            consume(); skip_newlines();
            struct node *ei = new_node(NN_IF);
            ei->cond = parse_list();
            skip_newlines(); if(peek_tok()->type==TT_THEN)consume();
            skip_newlines(); ei->body = parse_list();
            skip_newlines();
            cur->elsebody = ei; cur = ei;
        }
        if (peek_tok()->type == TT_ELSE) {
            consume(); skip_newlines();
            cur->elsebody = parse_list(); skip_newlines();
        }
        if (peek_tok()->type == TT_FI) consume();
        return n;
    }
    if (t->type == TT_WHILE || t->type == TT_UNTIL) {
        int is_until = (t->type == TT_UNTIL);
        consume(); skip_newlines();
        struct node *n = new_node(is_until ? NN_UNTIL : NN_WHILE);
        n->cond = parse_list();
        skip_newlines(); if(peek_tok()->type==TT_DO)consume();
        skip_newlines(); n->body = parse_list();
        skip_newlines(); if(peek_tok()->type==TT_DONE)consume();
        return n;
    }
    if (t->type == TT_FOR) {
        consume();
        struct tok *var = next_tok();
        struct node *n = new_node(NN_FOR);
        n->forvar = xstrdup(var->text ? var->text : "");
        skip_newlines();
        if (peek_tok()->type == TT_IN) {
            consume();
            while (peek_tok()->type == TT_WORD || peek_tok()->type == TT_ASSIGN) {
                struct tok *w = next_tok();
                wl_add(&n->forwords, xstrdup(w->text ? w->text : ""));
            }
            if(peek_tok()->type==TT_SEMI||peek_tok()->type==TT_NEWLINE) consume();
        }
        skip_newlines(); if(peek_tok()->type==TT_DO)consume();
        skip_newlines(); n->body = parse_list();
        skip_newlines(); if(peek_tok()->type==TT_DONE)consume();
        return n;
    }
    if (t->type == TT_CASE) {
        consume();
        struct tok *word = next_tok();
        struct node *n = new_node(NN_CASE);
        n->caseword = xstrdup(word->text ? word->text : "");
        skip_newlines();
        if (peek_tok()->type == TT_IN) consume();
        skip_newlines();
        struct caseitem *head = NULL, *tail = NULL;
        while (peek_tok()->type != TT_ESAC && peek_tok()->type != TT_EOF) {
            struct caseitem *ci = xcalloc(1, sizeof(*ci));
            /* patterns: pat1|pat2) */
            if (peek_tok()->type == TT_LPAREN) consume();
            int np = 0, npc = 4;
            ci->patterns = xmalloc(npc * sizeof(char*));
            while (peek_tok()->type != TT_RPAREN && peek_tok()->type != TT_EOF) {
                struct tok *pw = next_tok();
                if (np >= npc) { npc*=2; ci->patterns=xrealloc(ci->patterns,npc*sizeof(char*)); }
                ci->patterns[np++] = xstrdup(pw->text ? pw->text : "");
                if (peek_tok()->type == TT_PIPE) consume();
            }
            ci->npatterns = np;
            if (peek_tok()->type == TT_RPAREN) consume();
            skip_newlines();
            if (peek_tok()->type != TT_ESAC && peek_tok()->type != TT_EOF) {
                int saved2 = gtok_pos2;
                (void)saved2;
                ci->body = parse_list();
            }
            skip_newlines();
            /* consume ;; */
            if (peek_tok()->type == TT_DSEMI) consume();
            else if (peek_tok()->type == TT_SEMI) { consume(); if(peek_tok()->type==TT_SEMI) consume(); }
            skip_newlines();
            if (!head) head=tail=ci; else { tail->next=ci; tail=ci; }
        }
        if (peek_tok()->type == TT_ESAC) consume();
        n->cases = head;
        return n;
    }
    return parse_simple_cmd();
}

static struct node *parse_simple_cmd(void) {
    struct node *n = new_node(NN_CMD);
    n->assign = xmalloc(sizeof(char*)); n->assign[0] = NULL; n->nassign = 0;
    int assign_cap = 1;
    while (1) {
        struct tok *t = peek_tok();
        if (t->type == TT_EOF || t->type == TT_NEWLINE || t->type == TT_SEMI ||
            t->type == TT_DSEMI || t->type == TT_AMP  || t->type == TT_PIPE   ||
            t->type == TT_ANDAND || t->type == TT_OROR ||
            t->type == TT_DONE || t->type == TT_FI || t->type == TT_ESAC ||
            t->type == TT_ELIF || t->type == TT_ELSE || t->type == TT_THEN ||
            t->type == TT_DO   || t->type == TT_RBRACE || t->type == TT_RPAREN)
            break;
        /* redirection */
        struct redir *r = parse_redir();
        if (r) { r->next = n->redirs; n->redirs = r; continue; }
        /* check io_number before redir */
        if (t->type == TT_WORD && t->text) {
            char *end; long num = strtol(t->text, &end, 10);
            if (*end == '\0' && (peek_tok()+1)->type >= TT_LESS && (peek_tok()+1)->type <= TT_DGREATAMP) {
                consume();
                struct redir *r2 = parse_redir();
                if (r2) { r2->fd = (int)num; r2->next = n->redirs; n->redirs = r2; continue; }
                gtok_pos2--;
            }
        }
        if (t->type == TT_ASSIGN && n->args.n == 0) {
            consume();
            if (n->nassign >= assign_cap) { assign_cap*=2; n->assign=xrealloc(n->assign,(assign_cap+1)*sizeof(char*)); }
            n->assign[n->nassign++] = xstrdup(t->text);
            n->assign[n->nassign] = NULL;
            continue;
        }
        if (t->type == TT_WORD || t->type == TT_ASSIGN) {
            consume();
            wl_add(&n->args, xstrdup(t->text ? t->text : ""));
            continue;
        }
        break;
    }
    return n;
}

/* ====== built-ins ====== */

static char **cur_posparams = NULL;
static int    cur_npos = 0;

static int builtin_cd(int argc, char **argv) {
    const char *path;
    if (argc < 2) { path = sh_get("HOME"); if (!path) path = "/root"; }
    else path = argv[1];
    if (chdir(path) < 0) { perror(argv[0]); return 1; }
    char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) sh_set("PWD", cwd);
    return 0;
}

static int builtin_export(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { char *name=xstrndup(argv[i],eq-argv[i]); setenv(name,eq+1,1); free(name); }
        else { char *v=sh_get(argv[i]); if(v) setenv(argv[i],v,1); }
    }
    return 0;
}

static int builtin_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) sh_unset(argv[i]);
    return 0;
}

static int builtin_set(int argc, char **argv) {
    if (argc == 1) {
        for (char **e = environ; *e; e++) printf("%s\n", *e);
        return 0;
    }
    if (argc >= 2 && argv[1][0] != '-' && argv[1][0] != '+') {
        cur_posparams = argv;
        cur_npos = argc;
    }
    return 0;
}

static int builtin_shift(int argc, char **argv) {
    int n = 1;
    if (argc >= 2) n = atoi(argv[1]);
    if (cur_npos > 1 && n > 0) {
        int shift = n < cur_npos-1 ? n : cur_npos-1;
        for (int i = 1; i < cur_npos-shift; i++) cur_posparams[i] = cur_posparams[i+shift];
        cur_npos -= shift;
    }
    return 0;
}

static int builtin_echo(int argc, char **argv) {
    int n = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) { n = 1; i++; }
    for (; i < argc; i++) { if (i > 1+(n?1:0)-1+(n?0:0) && i > 1) putchar(' '); fputs(argv[i], stdout); }
    if (!n) putchar('\n');
    return 0;
}

int builtin_printf_fn(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *fmt = argv[1];
    int ai = 2;
    const char *p = fmt;
    while (*p) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break; case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break; case '\\': putchar('\\'); break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                int v=0, cnt=0; while(cnt<3 && *p>='0' && *p<='7') { v=v*8+(*p-'0'); p++; cnt++; } p--; putchar(v); break; }
            default: putchar('\\'); putchar(*p); break;
            }
            p++;
        } else if (*p == '%') {
            p++;
            if (*p == '%') { putchar('%'); p++; continue; }
            char spec[32]; int si=0;
            while (*p && !isalpha((unsigned char)*p)) spec[si++]=*p++;
            spec[si] = '\0';
            char fmtbuf[64]; snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%c", spec, *p);
            char *arg = (ai < argc) ? argv[ai++] : "";
            switch (*p) {
            case 'd': case 'i': printf(fmtbuf, (int)strtol(arg,NULL,0)); break;
            case 'u': case 'o': case 'x': case 'X': printf(fmtbuf,(unsigned)(strtoul(arg,NULL,0))); break;
            case 'f': case 'e': case 'g': printf(fmtbuf, strtod(arg,NULL)); break;
            case 's': printf(fmtbuf, arg); break;
            case 'c': printf(fmtbuf, (char)arg[0]); break;
            default: putchar('%'); break;
            }
            p++;
        } else {
            putchar(*p++);
        }
    }
    return 0;
}

static int builtin_read(int argc, char **argv) {
    char line[4096]; int len = 0;
    char ch;
    while (len < (int)sizeof(line)-1) {
        int n = read(0, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n') break;
        line[len++] = ch;
    }
    line[len] = '\0';
    if (len == 0) return 1;
    if (argc < 2) { sh_set("REPLY", line); return 0; }
    const char *ifs = sh_get("IFS"); if (!ifs) ifs = " \t\n";
    char *p = line;
    for (int i = 1; i < argc; i++) {
        while (*p && strchr(ifs, *p)) p++;
        char *start = p;
        if (i == argc-1) { sh_set(argv[i], p); break; }
        while (*p && !strchr(ifs, *p)) p++;
        char save = *p; *p = '\0'; sh_set(argv[i], start); *p = save;
    }
    return 0;
}

int builtin_test(int argc, char **argv);

/* ====== executor ====== */

extern int run_applet(const char *name, int argc, char **argv);
static int execute(struct node *n, char **pos, int npos);

static int do_redir(struct redir *r, int *saved_fds) {
    for (struct redir *rr = r; rr; rr = rr->next) {
        int fd = rr->fd;
        int to_fd = -1;
        if (fd < 0) fd = (rr->op == TT_LESS || rr->op == TT_DLESS) ? 0 : 1;
        if (saved_fds) { saved_fds[fd] = dup(fd); fcntl(saved_fds[fd], F_SETFD, FD_CLOEXEC); }
        switch (rr->op) {
        case TT_LESS:    to_fd = open(rr->word, O_RDONLY); break;
        case TT_GREAT:   to_fd = open(rr->word, O_WRONLY|O_CREAT|O_TRUNC, 0666); break;
        case TT_DGREAT:  to_fd = open(rr->word, O_WRONLY|O_CREAT|O_APPEND, 0666); break;
        case TT_DGREATAMP: { int afd=open(rr->word,O_WRONLY|O_CREAT|O_APPEND,0666); if(afd>=0){dup2(afd,1);dup2(afd,2);close(afd);} continue; }
        case TT_GREATAMP: {
            if (strcmp(rr->word, "-") == 0) { close(fd); continue; }
            int target = atoi(rr->word);
            dup2(target, fd); continue;
        }
        case TT_LESSAMP: {
            if (strcmp(rr->word, "-") == 0) { close(fd); continue; }
            int target = atoi(rr->word);
            dup2(target, fd); continue;
        }
        case TT_DLESS: {
            int pfd[2]; pipe(pfd);
            write(pfd[1], rr->word, strlen(rr->word));
            close(pfd[1]);
            dup2(pfd[0], fd); close(pfd[0]);
            continue;
        }
        case TT_LESSGREAT: to_fd = open(rr->word, O_RDWR|O_CREAT, 0666); break;
        default: break;
        }
        if (to_fd < 0) { perror(rr->word); return -1; }
        dup2(to_fd, fd);
        close(to_fd);
    }
    return 0;
}

static void undo_redir(struct redir *r, int *saved_fds) {
    for (struct redir *rr = r; rr; rr = rr->next) {
        int fd = rr->fd;
        if (fd < 0) fd = (rr->op == TT_LESS || rr->op == TT_DLESS) ? 0 : 1;
        if (saved_fds[fd] >= 0) { dup2(saved_fds[fd], fd); close(saved_fds[fd]); saved_fds[fd] = -1; }
    }
}

static struct shfunc *find_func(const char *name) {
    for (int i = 0; i < sh_nfuncs; i++)
        if (strcmp(sh_funcs[i].name, name) == 0) return &sh_funcs[i];
    return NULL;
}

static void add_func(const char *name, struct node *body) {
    for (int i = 0; i < sh_nfuncs; i++) {
        if (strcmp(sh_funcs[i].name, name) == 0) { sh_funcs[i].body = body; return; }
    }
    sh_funcs = xrealloc(sh_funcs, (sh_nfuncs+1)*sizeof(*sh_funcs));
    sh_funcs[sh_nfuncs].name = xstrdup(name);
    sh_funcs[sh_nfuncs].body = body;
    sh_nfuncs++;
}

static int exec_simple(struct node *n, char **pos, int npos) {
    struct wordlist args = {0};
    for (int i = 0; i < n->nassign; i++) {
        char *eq = strchr(n->assign[i], '=');
        if (!eq) continue;
        char *name = xstrndup(n->assign[i], eq - n->assign[i]);
        char *val  = expand_word(eq+1, pos, npos);
        if (n->args.n == 0) sh_set(name, val);
        free(name); free(val);
    }
    for (int i = 0; i < n->args.n; i++) {
        expand_into(n->args.w[i], &args, pos, npos, 1, 1);
    }
    if (args.n == 0) return 0;
    char **av = args.w;
    int    ac = args.n;
    struct { const char *name; int (*fn)(int,char**); } builtins[] = {
        {":",       NULL},
        {"true",    NULL},
        {"false",   NULL},
        {"cd",      builtin_cd},
        {"export",  builtin_export},
        {"unset",   builtin_unset},
        {"set",     builtin_set},
        {"shift",   builtin_shift},
        {"echo",    builtin_echo},
        {"printf",  builtin_printf_fn},
        {"read",    builtin_read},
        {"test",    builtin_test},
        {"[",       builtin_test},
        {NULL,NULL}
    };
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(av[0], builtins[i].name) == 0) {
            if (!builtins[i].fn) return (strcmp(av[0],"false")==0) ? 1 : 0;
            int saved[32]; memset(saved,-1,sizeof(saved));
            if (n->redirs) do_redir(n->redirs, saved);
            int r = builtins[i].fn(ac, av);
            if (n->redirs) undo_redir(n->redirs, saved);
            return r;
        }
    }
    if (strcmp(av[0],"exit") == 0)     { sh_exiting=1; sh_exit_code=(ac>1?atoi(av[1]):last_status); return sh_exit_code; }
    if (strcmp(av[0],"return") == 0)   { if(in_func){sh_exit_code=(ac>1?atoi(av[1]):last_status);longjmp(return_jmp,1);} return 0; }
    if (strcmp(av[0],"break") == 0)    { if(in_loop){break_count=(ac>1?atoi(av[1]):1);longjmp(break_jmp,1);}    return 0; }
    if (strcmp(av[0],"continue") == 0) { if(in_loop){continue_count=(ac>1?atoi(av[1]):1);longjmp(continue_jmp,1);} return 0; }
    if (strcmp(av[0],"source") == 0 || strcmp(av[0],".") == 0) {
        if (ac < 2) return 0;
        FILE *f = fopen(av[1], "r");
        if (!f) { perror(av[1]); return 1; }
        struct input in2 = {0}; in2.fp = f;
        tokenize_all(&in2);
        int saved_pos = gtok_pos2;
        struct tok saved_arena[MAX_TOKENS]; int saved_n = token_arena_n;
        memcpy(saved_arena, token_arena, saved_n * sizeof(struct tok));
        int saved_pos2 = gtok_pos2;
        tokenize_all(&in2);
        struct node *prog = parse_list();
        int r = execute(prog, pos, npos);
        memcpy(token_arena, saved_arena, saved_n * sizeof(struct tok));
        token_arena_n = saved_n;
        gtok_pos2 = saved_pos2;
        fclose(f);
        (void)saved_pos;
        return r;
    }
    if (strcmp(av[0],"eval") == 0) {
        struct sb s = {0};
        for (int i = 1; i < ac; i++) { if(i>1)sb_add(&s,' '); sb_adds(&s,av[i]); }
        char *src = sb_done(&s);
        struct input in2 = {.str=src, .buf=src, .len=(int)strlen(src)};
        struct tok saved_arena[MAX_TOKENS]; int saved_n = token_arena_n;
        memcpy(saved_arena, token_arena, saved_n * sizeof(struct tok));
        int saved_pos2 = gtok_pos2;
        tokenize_all(&in2);
        struct node *prog = parse_list();
        int r = execute(prog, pos, npos);
        memcpy(token_arena, saved_arena, saved_n * sizeof(struct tok));
        token_arena_n = saved_n;
        gtok_pos2 = saved_pos2;
        free(src);
        return r;
    }
    if (strcmp(av[0],"wait") == 0) {
        int st; if(ac>1) waitpid(atoi(av[1]),&st,0); else waitpid(-1,&st,0);
        return WIFEXITED(st)?WEXITSTATUS(st):1;
    }
    if (strcmp(av[0],"kill") == 0) {
        int sig = SIGTERM;
        int i = 1;
        if (ac > 1 && av[1][0] == '-') { sig = atoi(av[1]+1); i++; }
        for (; i < ac; i++) kill(atoi(av[i]), sig);
        return 0;
    }
    if (strcmp(av[0],"type") == 0) {
        for (int i = 1; i < ac; i++) {
            struct shfunc *f = find_func(av[i]);
            if (f) { printf("%s is a function\n", av[i]); continue; }
            int found = 0;
            for (int j = 0; j < n_applets; j++) {
                if (strcmp(applets[j].name, av[i])==0) { printf("%s is a built-in\n",av[i]); found=1; break; }
            }
            if (!found) {
                char path[PATH_MAX]; const char *PATH = sh_get("PATH"); if(!PATH) PATH="/bin:/usr/bin";
                char *pp = xstrdup(PATH), *tok2, *rest=pp;
                while ((tok2=strsep(&rest,":"))!=NULL) {
                    snprintf(path,sizeof(path),"%s/%s",tok2,av[i]);
                    if (access(path,X_OK)==0) { printf("%s is %s\n",av[i],path); found=1; break; }
                }
                free(pp);
                if (!found) { fprintf(stderr,"%s: not found\n",av[i]); }
            }
        }
        return 0;
    }
    if (strcmp(av[0],"pwd") == 0) {
        char cwd[PATH_MAX]; getcwd(cwd, sizeof(cwd)); printf("%s\n", cwd); return 0;
    }
    if (strcmp(av[0],"umask") == 0) {
        if (ac == 1) { mode_t m=umask(0); umask(m); printf("%04o\n",(unsigned)m); return 0; }
        umask((mode_t)strtol(av[1],NULL,8)); return 0;
    }
    if (strcmp(av[0],"readonly") == 0) { return builtin_export(ac,av); }
    if (strcmp(av[0],"local") == 0) { return 0; }
    if (strcmp(av[0],"trap") == 0)   { return 0; }
    if (strcmp(av[0],"getopts") == 0){ return 1; }
    if (strcmp(av[0],"exec") == 0) {
        if (ac < 2) return 0;
        if (n->redirs) do_redir(n->redirs, NULL);
        execvp(av[1], av+1);
        perror(av[1]); _exit(127);
    }
    if (strcmp(av[0],"unalias") == 0 || strcmp(av[0],"alias") == 0) { return 0; }
    /* check shell function */
    struct shfunc *fn = find_func(av[0]);
    if (fn) {
        char **old_pos = cur_posparams;
        int old_npos = cur_npos;
        cur_posparams = av; cur_npos = ac;
        int old_in_func = in_func; in_func = 1;
        jmp_buf old_ret; memcpy(old_ret, return_jmp, sizeof(jmp_buf));
        int ret = 0;
        if (setjmp(return_jmp) == 0) {
            int saved[32]; memset(saved,-1,sizeof(saved));
            if (n->redirs) do_redir(n->redirs, saved);
            ret = execute(fn->body, av, ac);
            if (n->redirs) undo_redir(n->redirs, saved);
        } else {
            ret = sh_exit_code;
        }
        memcpy(return_jmp, old_ret, sizeof(jmp_buf));
        in_func = old_in_func;
        cur_posparams = old_pos; cur_npos = old_npos;
        return ret;
    }
    /* try applet */
    {
        int r = run_applet(av[0], ac, av);
        if (r >= 0) return r;
    }
    /* external command */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        for (int i = 0; i < n->nassign; i++) {
            char *eq = strchr(n->assign[i], '=');
            if (!eq) continue;
            char *name = xstrndup(n->assign[i], eq-n->assign[i]);
            char *val  = expand_word(eq+1, pos, npos);
            setenv(name, val, 1);
            free(name); free(val);
        }
        if (n->redirs) do_redir(n->redirs, NULL);
        execvp(av[0], av);
        perror(av[0]); _exit(127);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : (128 + WTERMSIG(st));
}

static int execute(struct node *n, char **pos, int npos) {
    if (!n || sh_exiting) return sh_exit_code;
    int r = 0;
    switch (n->type) {
    case NN_LIST:
        r = execute(n->left, pos, npos);
        last_status = r;
        if (sh_exiting) return sh_exit_code;
        if (n->sep == TT_ANDAND && r != 0) return r;
        if (n->sep == TT_OROR  && r == 0) return r;
        if (n->sep == TT_AMP) { /* background: simplified sync */ }
        r = execute(n->right, pos, npos);
        break;
    case NN_BANG:
        r = execute(n->body, pos, npos); r = !r;
        break;
    case NN_PIPE: {
        int npipes = n->npipes;
        int **pfds = xmalloc(npipes * sizeof(int*));
        for (int i = 0; i < npipes-1; i++) {
            pfds[i] = xmalloc(2*sizeof(int));
            pipe(pfds[i]);
        }
        pid_t *pids = xmalloc(npipes * sizeof(pid_t));
        for (int i = 0; i < npipes; i++) {
            pids[i] = fork();
            if (pids[i] == 0) {
                if (i > 0) { dup2(pfds[i-1][0], 0); close(pfds[i-1][1]); }
                if (i < npipes-1) { dup2(pfds[i][1], 1); close(pfds[i][0]); }
                for (int j = 0; j < npipes-1; j++) { if(j!=i-1){close(pfds[j][0]);close(pfds[j][1]);} }
                int pr = execute(n->pipes[i], pos, npos);
                fflush(NULL);
                _exit(pr);
            }
        }
        for (int i = 0; i < npipes-1; i++) { close(pfds[i][0]); close(pfds[i][1]); free(pfds[i]); }
        free(pfds);
        int st;
        for (int i = 0; i < npipes; i++) { waitpid(pids[i], &st, 0); r = WIFEXITED(st)?WEXITSTATUS(st):(128+WTERMSIG(st)); }
        free(pids);
        if (n->negate) r = !r;
        break;
    }
    case NN_CMD:
        r = exec_simple(n, pos, npos);
        break;
    case NN_SUBSH: {
        pid_t pid = fork();
        if (pid == 0) { r=execute(n->body,pos,npos); _exit(r); }
        int st; waitpid(pid,&st,0); r=WIFEXITED(st)?WEXITSTATUS(st):1;
        break;
    }
    case NN_BRACE:
        r = execute(n->body, pos, npos);
        break;
    case NN_FUNC:
        add_func(n->funcname, n->body);
        r = 0;
        break;
    case NN_IF: {
        int cr = execute(n->cond, pos, npos);
        if (cr == 0) r = execute(n->body, pos, npos);
        else if (n->elsebody) r = execute(n->elsebody, pos, npos);
        else r = 0;
        break;
    }
    case NN_WHILE:
    case NN_UNTIL: {
        jmp_buf old_brk, old_cont;
        memcpy(old_brk, break_jmp, sizeof(jmp_buf));
        memcpy(old_cont, continue_jmp, sizeof(jmp_buf));
        int old_loop = in_loop; in_loop = 1;
        r = 0;
        for (;;) {
            if (sh_exiting) break;
            int cr = execute(n->cond, pos, npos);
            if (n->type == NN_WHILE && cr != 0) break;
            if (n->type == NN_UNTIL && cr == 0) break;
            int jumped = setjmp(break_jmp);
            if (!jumped) {
                int cjumped = setjmp(continue_jmp);
                if (!cjumped) r = execute(n->body, pos, npos);
                else { if(--continue_count > 0) break; }
            } else { if(--break_count > 0) break; break; }
        }
        memcpy(break_jmp, old_brk, sizeof(jmp_buf));
        memcpy(continue_jmp, old_cont, sizeof(jmp_buf));
        in_loop = old_loop;
        break;
    }
    case NN_FOR: {
        struct wordlist words = {0};
        if (n->forwords.n > 0) {
            for (int i = 0; i < n->forwords.n; i++)
                expand_into(n->forwords.w[i], &words, pos, npos, 1, 1);
        } else {
            for (int i = 1; i < npos && pos; i++) wl_add(&words, xstrdup(pos[i]));
        }
        jmp_buf old_brk, old_cont;
        memcpy(old_brk, break_jmp, sizeof(jmp_buf));
        memcpy(old_cont, continue_jmp, sizeof(jmp_buf));
        int old_loop = in_loop; in_loop = 1;
        r = 0;
        for (int i = 0; i < words.n && !sh_exiting; i++) {
            sh_set(n->forvar, words.w[i]);
            int jumped = setjmp(break_jmp);
            if (!jumped) {
                int cjumped = setjmp(continue_jmp);
                if (!cjumped) r = execute(n->body, pos, npos);
                else { if(--continue_count > 0) { i = words.n; } continue; }
            } else { if(--break_count>0){i=words.n;} break; }
        }
        memcpy(break_jmp, old_brk, sizeof(jmp_buf));
        memcpy(continue_jmp, old_cont, sizeof(jmp_buf));
        in_loop = old_loop;
        for (int i = 0; i < words.n; i++) free(words.w[i]);
        free(words.w);
        break;
    }
    case NN_CASE: {
        char *cw = expand_word(n->caseword, pos, npos);
        r = 0;
        for (struct caseitem *ci = n->cases; ci; ci = ci->next) {
            int matched = 0;
            for (int i = 0; i < ci->npatterns; i++) {
                char *pat = expand_word(ci->patterns[i], pos, npos);
                if (fnmatch(pat, cw, 0) == 0) matched = 1;
                free(pat);
                if (matched) break;
            }
            if (matched) { r = execute(ci->body, pos, npos); break; }
        }
        free(cw);
        break;
    }
    }
    last_status = r;
    char rs[16]; snprintf(rs, sizeof(rs), "%d", r);
    sh_set("?", rs);
    return r;
}

/* ====== test builtin ====== */

int builtin_test(int argc, char **argv) {
    int bracket = (argc > 0 && strcmp(argv[0],"[") == 0);
    if (bracket && argc > 1 && strcmp(argv[argc-1],"]") == 0) argc--;
    argv++; argc--;
    if (argc == 0) return 1;
    if (argc == 2 && strcmp(argv[0],"-z") == 0) return (strlen(argv[1]) == 0) ? 0 : 1;
    if (argc == 2 && strcmp(argv[0],"-n") == 0) return (strlen(argv[1]) != 0) ? 0 : 1;
    if (argc == 1) return (strlen(argv[0]) != 0) ? 0 : 1;
    if (argc == 2) {
        const char *flag = argv[0], *path = argv[1];
        struct stat st;
        if (strcmp(flag,"-e")==0) return stat(path,&st)==0?0:1;
        if (strcmp(flag,"-f")==0) return (stat(path,&st)==0 && S_ISREG(st.st_mode))?0:1;
        if (strcmp(flag,"-d")==0) return (stat(path,&st)==0 && S_ISDIR(st.st_mode))?0:1;
        if (strcmp(flag,"-r")==0) return access(path,R_OK)==0?0:1;
        if (strcmp(flag,"-w")==0) return access(path,W_OK)==0?0:1;
        if (strcmp(flag,"-x")==0) return access(path,X_OK)==0?0:1;
        if (strcmp(flag,"-s")==0) return (stat(path,&st)==0 && st.st_size>0)?0:1;
        if (strcmp(flag,"-L")==0||strcmp(flag,"-h")==0) return lstat(path,&st)==0&&S_ISLNK(st.st_mode)?0:1;
        if (strcmp(flag,"-p")==0) return (stat(path,&st)==0 && S_ISFIFO(st.st_mode))?0:1;
        if (strcmp(flag,"-b")==0) return (stat(path,&st)==0 && S_ISBLK(st.st_mode))?0:1;
        if (strcmp(flag,"-c")==0) return (stat(path,&st)==0 && S_ISCHR(st.st_mode))?0:1;
        if (strcmp(flag,"-t")==0) return isatty(atoi(path))?0:1;
    }
    if (argc == 3) {
        if (strcmp(argv[1],"=") ==0||strcmp(argv[1],"==")==0) return strcmp(argv[0],argv[2])==0?0:1;
        if (strcmp(argv[1],"!=")== 0) return strcmp(argv[0],argv[2])!=0?0:1;
        if (strcmp(argv[1],"-eq")==0) return atol(argv[0])==atol(argv[2])?0:1;
        if (strcmp(argv[1],"-ne")==0) return atol(argv[0])!=atol(argv[2])?0:1;
        if (strcmp(argv[1],"-lt")==0) return atol(argv[0])< atol(argv[2])?0:1;
        if (strcmp(argv[1],"-le")==0) return atol(argv[0])<=atol(argv[2])?0:1;
        if (strcmp(argv[1],"-gt")==0) return atol(argv[0])> atol(argv[2])?0:1;
        if (strcmp(argv[1],"-ge")==0) return atol(argv[0])>=atol(argv[2])?0:1;
        if (strcmp(argv[1],"-nt")==0) { struct stat a,b; stat(argv[0],&a);stat(argv[2],&b); return a.st_mtime>b.st_mtime?0:1; }
        if (strcmp(argv[1],"-ot")==0) { struct stat a,b; stat(argv[0],&a);stat(argv[2],&b); return a.st_mtime<b.st_mtime?0:1; }
        if (strcmp(argv[1],"-ef")==0) { struct stat a,b; stat(argv[0],&a);stat(argv[2],&b); return (a.st_dev==b.st_dev&&a.st_ino==b.st_ino)?0:1; }
    }
    if (argc == 3 && strcmp(argv[1],"-a")==0) {
        char *a1[2]={argv[0],NULL},*a2[2]={argv[2],NULL};
        return builtin_test(2,a1-1)==0 && builtin_test(2,a2-1)==0 ? 0:1;
    }
    if (argc == 3 && strcmp(argv[1],"-o")==0) {
        char *a1[2]={argv[0],NULL},*a2[2]={argv[2],NULL};
        return builtin_test(2,a1-1)==0 || builtin_test(2,a2-1)==0 ? 0:1;
    }
    if (argc >= 2 && strcmp(argv[0],"!")==0) {
        return builtin_test(argc-1, argv) == 0 ? 1 : 0;
    }
    return strlen(argv[0]) != 0 ? 0 : 1;
}


/* ====== shell main ====== */

int shell_main(int argc, char **argv) {
    int c_flag = 0;
    const char *c_str = NULL;
    const char *script = NULL;

    sh_set("0", argv[0]);
    cur_posparams = argv;
    cur_npos = argc;

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i+1 < argc) { c_flag = 1; c_str = argv[++i]; }
        else if (strcmp(argv[i], "-s") == 0) { /* read from stdin */ }
        else if (argv[i][0] != '-') { script = argv[i]; break; }
    }

    if (!sh_get("PATH")) sh_set("PATH", "/bin:/sbin:/usr/bin:/usr/sbin");
    if (!sh_get("PS1")) {
        const char *user = sh_get("USER");
        if (!user) { uid_t u=getuid(); sh_set("PS1", u==0 ? "# " : "$ "); }
        else sh_set("PS1", getuid()==0 ? "# " : "$ ");
    }
    if (!sh_get("PS2")) sh_set("PS2", "> ");

    struct input inp = {0};

    if (c_flag) {
        inp.str = c_str;
        inp.buf = (char *)c_str;
        inp.len = strlen(c_str);
    } else if (script) {
        inp.fp = fopen(script, "r");
        if (!inp.fp) { perror(script); return 1; }
    } else {
        inp.fp = stdin;
        inp.interactive = isatty(0);
        sh_interactive = inp.interactive;
    }

    int ret = 0;
    if (c_flag) {
        tokenize_all(&inp);
        struct node *prog = parse_list();
        ret = execute(prog, argv, argc);
    } else {
        if (inp.interactive) {
            for (;;) {
                char line[4096];
                const char *ps1 = sh_get("PS1"); if (!ps1) ps1 = "$ ";
                fputs(ps1, stdout); fflush(stdout);
                if (!fgets(line, sizeof(line), inp.fp)) break;
                struct input lin = {.str=line,.buf=line,.len=(int)strlen(line)};
                tokenize_all(&lin);
                if (peek_tok()->type == TT_EOF) continue;
                struct node *prog = parse_list();
                ret = execute(prog, argv, argc);
                if (sh_exiting) { ret = sh_exit_code; break; }
            }
        } else {
            tokenize_all(&inp);
            struct node *prog = parse_list();
            ret = execute(prog, argv, argc);
        }
    }
    if (inp.fp && inp.fp != stdin) fclose(inp.fp);
    return ret;
}
