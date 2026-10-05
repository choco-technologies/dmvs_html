#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Scripts: what a page's JavaScript does when it is used, made the view's
 * variables and handlers - not by running the script on the device, but by
 * working out at conversion what each handler does.
 *
 * What is understood is what switches screens and toggles things:
 *
 *   const x = document.getElementById('id');      an element
 *   let current = null;                            a variable (a global the handlers assign)
 *   function open(id) { ... }                      called from onclick="open('a')": inlined, its arguments known
 *   el.classList.add / remove / toggle('c')        a class changes
 *   el.style.opacity = '0.3'                       a style changes
 *   if (current) / if (!current) / if (a === b)    on a variable, at run time
 *   el.classList.contains('c')                     ... on a class
 *
 * Every change is laid out: the page with that class (or that style), how
 * the element moved and faded is what its variables become - x, y,
 * opacity of the group it is painted in - set at once, or animated by the
 * element's CSS transition. A change that does more (the element's inside
 * laid out anew, shown or hidden) is reported and left as it is.
 *
 * What is not understood (timers, Date, innerText, loops, ...) is reported
 * and left out: the page stays as it is there.
 */

#define MAX_TOKEN_TEXT      1024u
#define MAX_SCOPE           64u
#define MAX_GLOBALS         64u
#define MAX_RUNTIME         16u
#define MAX_DOMAIN          32u
#define MAX_MODS            64u
#define MAX_CLASSVARS       16u
#define MAX_ACTIONS         256u
#define MAX_INLINE          8u
#define MAX_PARSE_DEPTH     64u

/* ---- Tokens ---- */

#define T_END       0u
#define T_IDENT     1u
#define T_NUMBER    2u
#define T_STRING    3u
#define T_PUNCT     4u
#define T_DYNAMIC   5u          /* A template with ${...}: not a constant */

typedef struct
{
    uint8_t     kind;
    const char* s;              /* T_STRING: decoded (in the arena) */
    size_t      n;
    int32_t     num;            /* x 1000 */
} token_t;

typedef struct
{
    conv_t*     c;
    const char* p;
    const char* end;
    token_t     tok;            /* The current one */
    uint32_t    depth;
} lexer_t;

static bool is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }
static bool ident_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '$'; }
static bool ident_char(char ch) { return ident_start(ch) || (ch >= '0' && ch <= '9'); }

static void next(lexer_t* l)
{
    const char* p = l->p;
    for (;;)
    {
        while (p < l->end && is_space(*p))
            p++;
        if (p + 1 < l->end && p[0] == '/' && p[1] == '/')
        {
            while (p < l->end && *p != '\n')
                p++;
            continue;
        }
        if (p + 1 < l->end && p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (p + 1 < l->end && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = (p + 1 < l->end) ? p + 2 : l->end;
            continue;
        }
        break;
    }
    token_t* t = &l->tok;
    memset(t, 0, sizeof(*t));
    if (p >= l->end)
    {
        t->kind = T_END;
        l->p = p;
        return;
    }
    t->s = p;
    if (ident_start(*p))
    {
        while (p < l->end && ident_char(*p))
            p++;
        t->kind = T_IDENT;
        t->n = (size_t)(p - t->s);
    }
    else if ((*p >= '0' && *p <= '9') || (*p == '.' && p + 1 < l->end && p[1] >= '0' && p[1] <= '9'))
    {
        int32_t v = 0, frac = 0, scale = 1000;
        for (; p < l->end && *p >= '0' && *p <= '9'; p++)
            v = (v < 1000000) ? v * 10 + (*p - '0') : v;
        if (p < l->end && *p == '.')
        {
            for (p++; p < l->end && *p >= '0' && *p <= '9'; p++)
            {
                if (scale > 1)
                {
                    scale /= 10;
                    frac += (*p - '0') * scale;
                }
            }
        }
        while (p < l->end && ident_char(*p))
            p++;
        t->kind = T_NUMBER;
        t->num = v * 1000 + frac;
        t->n = (size_t)(p - t->s);
    }
    else if (*p == '"' || *p == '\'' || *p == '`')
    {
        char q = *p++;
        char buffer[MAX_TOKEN_TEXT];
        size_t n = 0;
        bool dynamic = false;
        while (p < l->end && *p != q)
        {
            char ch = *p++;
            if (q == '`' && ch == '$' && p < l->end && *p == '{')
                dynamic = true;
            if (ch == '\\' && p < l->end)
            {
                ch = *p++;
                ch = (ch == 'n') ? '\n' : (ch == 't') ? '\t' : ch;
            }
            if (n + 1U < sizeof(buffer))
                buffer[n++] = ch;
        }
        if (p < l->end)
            p++;
        t->kind = dynamic ? T_DYNAMIC : T_STRING;
        t->s = arena_strndup(&l->c->arena, buffer, n);
        t->n = n;
    }
    else
    {
        /* Punctuators: the longest first */
        static const char three[] = "|===|!==|>>>|...|**=|";
        static const char two[] = "|==|!=|<=|>=|&&|=>|||++|--|+=|-=|*=|/=|?.|??|";
        t->kind = T_PUNCT;
        t->n = 1;
        for (const char* q = three; *q != '\0'; q++)
        {
            if (*q == '|' && q[1] != '\0' && p + 3 <= l->end && strncmp(q + 1, p, 3) == 0 && q[4] == '|')
                t->n = 3;
        }
        for (const char* q = two; *q != '\0' && t->n == 1; q++)
        {
            if (*q == '|' && q[1] != '\0' && p + 2 <= l->end && strncmp(q + 1, p, 2) == 0 && q[3] == '|')
                t->n = 2;
        }
        p += t->n;
    }
    l->p = p;
}

static bool is(const lexer_t* l, const char* text)
{
    size_t n = strlen(text);
    return (l->tok.kind == T_PUNCT || l->tok.kind == T_IDENT) && l->tok.n == n && strncmp(l->tok.s, text, n) == 0;
}

static bool accept(lexer_t* l, const char* text)
{
    if (!is(l, text))
        return false;
    next(l);
    return true;
}

/* ---- Syntax ---- */

#define J_UNKNOWN   0u          /* Something not understood */
#define J_NUMBER    1u
#define J_STRING    2u
#define J_IDENT     3u
#define J_NULL      4u
#define J_TRUE      5u
#define J_FALSE     6u
#define J_THIS      7u
#define J_MEMBER    8u          /* a.name */
#define J_CALL      9u          /* a(list) */
#define J_ASSIGN    10u         /* a = b */
#define J_NOT       11u         /* !a */
#define J_EQ        12u         /* a == b, a === b */
#define J_NE        13u
#define J_AND       14u
#define J_OR        15u
#define J_UNDEFINED 16u
#define J_FUNC      17u         /* (list) => b, a; function (list) { b } */
#define J_ADD       18u         /* a + b */

#define S_EXPR      32u
#define S_VAR       33u         /* name = a */
#define S_FUNC      34u         /* name(list) { b } */
#define S_IF        35u         /* if (a) b else c */
#define S_BLOCK     36u         /* { list } */
#define S_RETURN    37u
#define S_UNKNOWN   38u
#define S_EMPTY     39u

typedef struct js js_t;
struct js
{
    uint8_t     kind;
    const char* name;           /* J_IDENT, J_MEMBER, S_VAR, S_FUNC; J_STRING's text */
    size_t      length;
    int32_t     num;
    js_t*       a;
    js_t*       b;
    js_t*       c;
    js_t*       list;           /* Arguments, parameters, statements */
    js_t*       next;
    const char* source;         /* Where it starts (for reports) */
};

static js_t* node(lexer_t* l, uint8_t kind)
{
    js_t* n = arena_alloc(&l->c->arena, sizeof(*n));
    if (n != NULL)
    {
        n->kind = kind;
        n->source = l->tok.s;
    }
    return n;
}

static js_t* expression(lexer_t* l);
static js_t* assignment(lexer_t* l);
static js_t* statement(lexer_t* l);
static js_t* block(lexer_t* l);
static void skip_balanced(lexer_t* l);

/* (a, b = 1, c): the parameters' names into *tail - at "(" */
static void parameters(lexer_t* l, js_t** tail)
{
    if (!accept(l, "("))
        return;
    while (!is(l, ")") && l->tok.kind != T_END)
    {
        if (l->tok.kind == T_IDENT && tail != NULL)
        {
            js_t* p = node(l, J_IDENT);
            if (p != NULL)
            {
                p->name = l->tok.s;
                p->length = l->tok.n;
                *tail = p;
                tail = &p->next;
            }
        }
        if (is(l, "[") || is(l, "{"))
            skip_balanced(l);           /* ({ a }) => ...: not understood, passed as it is */
        else
            next(l);
        if (is(l, "="))
        {
            next(l);
            (void)assignment(l);        /* A default: not understood, the argument is passed */
        }
        (void)accept(l, ",");
    }
    (void)accept(l, ")");
}

/* An arrow function's body - after "=>": a block, or an expression */
static void arrow_body(lexer_t* l, js_t* f)
{
    if (is(l, "{"))
    {
        js_t* b = block(l);
        if (f != NULL)
            f->b = b;
    }
    else
    {
        js_t* e = assignment(l);
        if (f != NULL)
            f->a = e;
    }
}

/* Past a balanced (...), [...] or {...} starting at the current token */
static void skip_balanced(lexer_t* l)
{
    int depth = 0;
    do
    {
        if (is(l, "(") || is(l, "[") || is(l, "{"))
            depth++;
        else if (is(l, ")") || is(l, "]") || is(l, "}"))
            depth--;
        next(l);
    } while (depth > 0 && l->tok.kind != T_END);
}

static js_t* primary(lexer_t* l)
{
    js_t* n = NULL;
    if (l->tok.kind == T_NUMBER)
    {
        n = node(l, J_NUMBER);
        if (n != NULL)
            n->num = l->tok.num;
        next(l);
        return n;
    }
    if (l->tok.kind == T_STRING)
    {
        n = node(l, J_STRING);
        if (n != NULL)
        {
            n->name = l->tok.s;
            n->length = l->tok.n;
        }
        next(l);
        return n;
    }
    if (l->tok.kind == T_IDENT)
    {
        static const struct { char word[10]; uint8_t kind; } words[] = {
            { "null", J_NULL }, { "true", J_TRUE }, { "false", J_FALSE }, { "this", J_THIS }, { "undefined", J_UNDEFINED },
        };
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        {
            if (is(l, words[i].word))
            {
                n = node(l, words[i].kind);
                next(l);
                return n;
            }
        }
        if (is(l, "function"))
        {
            /* function (a) { ... }: a function as a value */
            n = node(l, J_FUNC);
            next(l);
            if (l->tok.kind == T_IDENT)
                next(l);
            parameters(l, (n != NULL) ? &n->list : NULL);
            js_t* b = block(l);
            if (n != NULL)
                n->b = b;
            return n;
        }
        if (is(l, "new") || is(l, "class") || is(l, "async") || is(l, "await"))
        {
            /* A function expression, an object made: not understood - past it */
            n = node(l, J_UNKNOWN);
            next(l);
            while (l->tok.kind == T_IDENT)
                next(l);
            if (is(l, "("))
                skip_balanced(l);
            if (is(l, "{"))
                skip_balanced(l);
            return n;
        }
        n = node(l, J_IDENT);
        if (n != NULL)
        {
            n->name = l->tok.s;
            n->length = l->tok.n;
        }
        next(l);
        if (is(l, "=>"))
        {
            /* x => ...: an arrow function, x its parameter */
            next(l);
            js_t* f = node(l, J_FUNC);
            if (f != NULL)
            {
                f->list = n;
                f->source = (n != NULL) ? n->source : f->source;
            }
            arrow_body(l, f);
            return f;
        }
        return n;
    }
    if (is(l, "("))
    {
        /* (a) - or (a, b) => ...: an arrow function */
        const char* p = l->p;
        token_t t = l->tok;
        skip_balanced(l);
        if (is(l, "=>"))
        {
            l->p = p;
            l->tok = t;
            js_t* f = node(l, J_FUNC);
            parameters(l, (f != NULL) ? &f->list : NULL);
            (void)accept(l, "=>");
            arrow_body(l, f);
            return f;
        }
        l->p = p;
        l->tok = t;
        next(l);
        n = expression(l);
        (void)accept(l, ")");
        return n;
    }
    if (is(l, "[") || is(l, "{"))
    {
        n = node(l, J_UNKNOWN);
        skip_balanced(l);
        return n;
    }
    if (l->tok.kind == T_DYNAMIC)
    {
        n = node(l, J_UNKNOWN);
        next(l);
        return n;
    }
    n = node(l, J_UNKNOWN);
    if (l->tok.kind != T_END)
        next(l);
    return n;
}

static js_t* postfix(lexer_t* l)
{
    js_t* n = primary(l);
    for (;;)
    {
        if (is(l, ".") || is(l, "?."))
        {
            next(l);
            js_t* m = node(l, J_MEMBER);
            if (m == NULL)
                return n;
            m->a = n;
            m->source = (n != NULL) ? n->source : m->source;
            if (l->tok.kind == T_IDENT)
            {
                m->name = l->tok.s;
                m->length = l->tok.n;
                next(l);
            }
            n = m;
        }
        else if (is(l, "("))
        {
            next(l);
            js_t* call = node(l, J_CALL);
            if (call == NULL)
                return n;
            call->a = n;
            call->source = (n != NULL) ? n->source : call->source;
            js_t** tail = &call->list;
            while (!is(l, ")") && l->tok.kind != T_END)
            {
                js_t* arg = assignment(l);
                if (arg != NULL)
                {
                    *tail = arg;
                    tail = &arg->next;
                }
                if (!accept(l, ","))
                    break;
            }
            (void)accept(l, ")");
            n = call;
        }
        else if (is(l, "["))
        {
            js_t* u = node(l, J_UNKNOWN);
            skip_balanced(l);
            n = u;
        }
        else if (is(l, "++") || is(l, "--"))
        {
            next(l);
            n = node(l, J_UNKNOWN);
        }
        else
            return n;
    }
}

static js_t* unary(lexer_t* l)
{
    if (accept(l, "!"))
    {
        js_t* n = node(l, J_NOT);
        if (n != NULL)
            n->a = unary(l);
        return n;
    }
    if (is(l, "-") || is(l, "+") || is(l, "typeof") || is(l, "void") || is(l, "delete") || is(l, "++") || is(l, "--") ||
        is(l, "~"))
    {
        next(l);
        (void)unary(l);
        return node(l, J_UNKNOWN);
    }
    return postfix(l);
}

/* The binary operators: those understood make a node, the others an unknown one */
static js_t* binary(lexer_t* l, int level)
{
    static const char ops[][28] = {
        "|??|||", "|&&|", "|==|===|!=|!==|", "|<|>|<=|>=|instanceof|in|", "|+|-|", "|*|/|%|**|",
    };
    if (level >= (int)(sizeof(ops) / sizeof(ops[0])))
        return unary(l);
    js_t* left = binary(l, level + 1);
    for (;;)
    {
        bool found = false;
        size_t n = l->tok.n;
        if (l->tok.kind == T_PUNCT || l->tok.kind == T_IDENT)
        {
            for (const char* q = ops[level]; *q != '\0' && !found; q++)
                found = *q == '|' && strncmp(q + 1, l->tok.s, n) == 0 && q[1 + n] == '|';
        }
        if (!found)
            return left;
        uint8_t kind = J_UNKNOWN;
        if (is(l, "==") || is(l, "==="))
            kind = J_EQ;
        else if (is(l, "!=") || is(l, "!=="))
            kind = J_NE;
        else if (is(l, "&&"))
            kind = J_AND;
        else if (is(l, "||"))
            kind = J_OR;
        else if (is(l, "+"))
            kind = J_ADD;
        next(l);
        js_t* right = binary(l, level + 1);
        js_t* b = node(l, kind);
        if (b == NULL)
            return left;
        b->a = left;
        b->source = (left != NULL) ? left->source : b->source;
        b->b = right;
        left = b;
    }
}

/* An expression but a list of them (a, b): an argument, a variable's value */
static js_t* assignment(lexer_t* l)
{
    if (++l->depth > MAX_PARSE_DEPTH)
    {
        l->depth--;
        skip_balanced(l);
        return node(l, J_UNKNOWN);
    }
    js_t* left = binary(l, 0);
    if (accept(l, "?"))
    {
        /* a ? b : c - not understood */
        (void)assignment(l);
        (void)accept(l, ":");
        (void)assignment(l);
        left = node(l, J_UNKNOWN);
    }
    else if (is(l, "=") || is(l, "+=") || is(l, "-=") || is(l, "*=") || is(l, "/=") || is(l, "**="))
    {
        bool plain = is(l, "=");
        next(l);
        js_t* a = node(l, plain ? J_ASSIGN : J_UNKNOWN);
        if (a != NULL)
        {
            a->a = left;
            a->source = (left != NULL) ? left->source : a->source;
            a->b = assignment(l);
        }
        left = a;
    }
    l->depth--;
    return left;
}

static js_t* expression(lexer_t* l)
{
    js_t* left = assignment(l);
    while (accept(l, ","))
        (void)assignment(l);         /* a, b: only the first is kept */
    return left;
}

/* Past a statement not understood: up to its ';', or its block */
static void skip_statement(lexer_t* l)
{
    while (l->tok.kind != T_END && !is(l, ";") && !is(l, "}"))
    {
        if (is(l, "{"))
        {
            skip_balanced(l);
            if (!is(l, "else") && !is(l, "catch") && !is(l, "finally") && !is(l, "while"))
                return;
            continue;
        }
        if (is(l, "(") || is(l, "["))
        {
            skip_balanced(l);
            continue;
        }
        next(l);
    }
    (void)accept(l, ";");
}

static js_t* block(lexer_t* l)
{
    js_t* b = node(l, S_BLOCK);
    if (!accept(l, "{") || b == NULL)
        return b;
    js_t** tail = &b->list;
    while (!is(l, "}") && l->tok.kind != T_END)
    {
        js_t* s = statement(l);
        if (s != NULL)
        {
            *tail = s;
            tail = &s->next;
        }
    }
    (void)accept(l, "}");
    return b;
}

static js_t* statement(lexer_t* l)
{
    if (++l->depth > MAX_PARSE_DEPTH)
    {
        l->depth--;
        skip_statement(l);
        return node(l, S_UNKNOWN);
    }
    js_t* s = NULL;
    if (is(l, "{"))
        s = block(l);
    else if (accept(l, ";"))
        s = node(l, S_EMPTY);
    else if (is(l, "function"))
    {
        s = node(l, S_FUNC);
        next(l);
        if (s != NULL && l->tok.kind == T_IDENT)
        {
            s->name = l->tok.s;
            s->length = l->tok.n;
            next(l);
        }
        parameters(l, (s != NULL) ? &s->list : NULL);
        if (s != NULL)
            s->b = block(l);
        else
            skip_balanced(l);
    }
    else if (is(l, "const") || is(l, "let") || is(l, "var"))
    {
        next(l);
        js_t** tail = &s;
        do
        {
            js_t* v = node(l, S_VAR);
            if (l->tok.kind == T_IDENT && v != NULL)
            {
                v->name = l->tok.s;
                v->length = l->tok.n;
                next(l);
                if (accept(l, "="))
                    v->a = assignment(l);
                *tail = v;
                tail = &v->next;
            }
            else
            {
                /* const { a, b } = ...: not understood */
                skip_statement(l);
                l->depth--;
                return node(l, S_UNKNOWN);
            }
        } while (accept(l, ","));
        (void)accept(l, ";");
        /* Several declarators: in a block of their own */
        if (s != NULL && s->next != NULL)
        {
            js_t* b = node(l, S_BLOCK);
            if (b != NULL)
            {
                b->list = s;
                s = b;
            }
        }
    }
    else if (is(l, "if"))
    {
        s = node(l, S_IF);
        next(l);
        (void)accept(l, "(");
        js_t* cond = expression(l);
        (void)accept(l, ")");
        js_t* then = statement(l);
        js_t* other = accept(l, "else") ? statement(l) : NULL;
        if (s != NULL)
        {
            s->a = cond;
            s->b = then;
            s->c = other;
        }
    }
    else if (is(l, "return"))
    {
        s = node(l, S_RETURN);
        next(l);
        if (!is(l, ";") && !is(l, "}") && s != NULL)
            s->a = expression(l);
        (void)accept(l, ";");
    }
    else if (l->tok.kind == T_IDENT &&
             (is(l, "for") || is(l, "while") || is(l, "do") || is(l, "switch") || is(l, "try") || is(l, "class") ||
              is(l, "throw") || is(l, "import") || is(l, "export") || is(l, "async") || is(l, "break") || is(l, "continue")))
    {
        s = node(l, S_UNKNOWN);
        skip_statement(l);
    }
    else
    {
        s = node(l, S_EXPR);
        js_t* e = expression(l);
        if (s != NULL)
            s->a = e;
        (void)accept(l, ";");
    }
    l->depth--;
    return s;
}

static js_t* parse(conv_t* c, const char* text, size_t length)
{
    lexer_t l = { c, text, text + length, { 0 }, 0 };
    js_t* first = NULL;
    js_t** tail = &first;
    next(&l);
    while (l.tok.kind != T_END && !c->arena.failed)
    {
        const char* before = l.p;
        js_t* s = statement(&l);
        if (s != NULL)
        {
            *tail = s;
            while (*tail != NULL)
                tail = &(*tail)->next;
        }
        if (l.p == before && l.tok.kind != T_END)
            next(&l);               /* Not a step further: past it */
    }
    return first;
}

/* ---- Values ---- */

#define V_UNKNOWN   0u
#define V_UNDEFINED 1u
#define V_NULL      2u
#define V_BOOL      3u
#define V_NUMBER    4u
#define V_STRING    5u
#define V_ELEMENT   6u
#define V_RUNTIME   7u          /* A variable holding an element (or null) at run time */
#define V_DOCUMENT  8u
#define V_FUNCTION  9u
#define V_CLASSES   10u         /* x.classList */
#define V_STYLE     11u         /* x.style */
#define V_LIST      12u         /* document.querySelectorAll(...): elements known at conversion */

typedef struct binding binding_t;

typedef struct
{
    uint8_t         kind;
    node_t*         element;    /* V_ELEMENT; V_CLASSES, V_STYLE of one */
    uint32_t        runtime;    /* V_RUNTIME (1 ...); V_CLASSES, V_STYLE of one */
    const char*     text;
    int32_t         num;
    const js_t*     function;   /* V_FUNCTION: S_FUNC or J_FUNC */
    binding_t*      closure;    /* ... the variables it sees (J_FUNC) */
    uint32_t        closure_count;
    node_t**        items;      /* V_LIST */
    uint32_t        count;
} val_t;

struct binding
{
    const char*     name;
    size_t          length;
    val_t           value;
};

/* A global variable a handler assigns: the view's variable of the element it holds */
typedef struct
{
    const char*     name;
    size_t          length;
    dmvsi_var_t     var;
    int32_t         initial;            /* The element's index, 0: null */
    node_t*         domain[MAX_DOMAIN]; /* What it may hold */
    uint32_t        domain_count;
} runtime_t;

/* A change a script makes, and what it does to the element (when laid out) */
typedef struct
{
    mod_t           mod;
    node_t*         element;
    bool            laid_out;
    bool            same;               /* The page is as it is: nothing changes */
    bool            changed;            /* It does more than move and fade the element */
    int32_t         dx, dy;             /* How the element moves */
    int32_t         opacity;
    uint16_t        move_ms, fade_ms;
    int16_t         move_easing[4], fade_easing[4];
    node_t*         look;               /* changed: the element as it looks then (NULL: not shown) */
    uint32_t        look_index;         /* MOD_CLASSES: its look of the element's lookvar (0: as it is) */
    node_t*         reveals;            /* The element as it is then, shown - hidden as the page is */
} change_t;

/* A class of an element a script asks about (contains, toggle): its variable */
typedef struct
{
    node_t*         element;
    const char*     name;
    dmvsi_var_t     var;
} classvar_t;

/* el.addEventListener('click', f) of the page as it loads: f, run when el is clicked */
#define MAX_LISTENERS   32u

typedef struct
{
    node_t*         element;
    val_t           function;
} listener_t;

/* The classes a handler changes of an element, until what it does is known (a condition, its
 * end): one change of them all - the look they make together */
#define MAX_PENDING     16u
#define MAX_PENDING_OPS 12u

typedef struct
{
    node_t*         element;
    uint32_t        count;
    const char*     names[MAX_PENDING_OPS];
    bool            add[MAX_PENDING_OPS];
} pending_t;

/* An element's looks of the classes changed at once (MOD_CLASSES): 0 as it is, 1 ... */
typedef struct
{
    node_t*         element;
    dmvsi_var_t     var;
    node_t*         looks[MAX_VARIANTS - 1U];
    uint32_t        digests[MAX_VARIANTS - 1U];
    uint32_t        count;
} lookvar_t;

#define MAX_LOOKVARS    16u

#define PASS_DOMAINS    0u      /* What the variables may hold */
#define PASS_CHANGES    1u      /* Which changes there are */
#define PASS_EMIT       2u      /* The actions */

typedef struct
{
    conv_t*         c;
    uint32_t        pass;
    binding_t       globals[MAX_GLOBALS];
    uint32_t        global_count;
    binding_t       scope[MAX_SCOPE];
    uint32_t        scope_count;
    runtime_t       runtime[MAX_RUNTIME];
    uint32_t        runtime_count;
    change_t        changes[MAX_MODS];
    uint32_t        change_count;
    classvar_t      classvars[MAX_CLASSVARS];
    uint32_t        classvar_count;
    dmvsi_action_t  actions[MAX_ACTIONS];
    uint32_t        action_count;
    node_t*         self;               /* `this` of a handler */
    node_t*         clickables[MAX_MODS];
    uint32_t        clickable_count;
    uint32_t        inline_depth;
    bool            returned;           /* A return reached: the rest of the function is not run */
    uint32_t        conditional;        /* Inside an IF of run time */
    val_t           result;             /* What a return gave */
    bool            loading;            /* The page's top level: what it does as it loads */
    listener_t      listeners[MAX_LISTENERS];
    uint32_t        listener_count;
    pending_t       pending[MAX_PENDING];
    uint32_t        pending_count;
    lookvar_t       lookvars[MAX_LOOKVARS];
    uint32_t        lookvar_count;
    uint32_t        click_depth;
} script_t;

static bool same_name(const char* a, size_t an, const char* b, size_t bn)
{
    return an == bn && strncmp(a, b, an) == 0;
}

static bool is_name(const js_t* n, const char* name)
{
    return n != NULL && (n->kind == J_IDENT || n->kind == J_MEMBER) && same_name(n->name, n->length, name, strlen(name));
}

/* A report of what a script does that is not converted - once, in the last pass */
static void report(script_t* sc, const js_t* where, const char* what)
{
    if (sc->pass != PASS_EMIT)
        return;
    char excerpt[48];
    size_t n = 0;
    for (const char* s = (where != NULL && where->source != NULL) ? where->source : ""; *s != '\0' && *s != '\n' && n + 1U < sizeof(excerpt); s++)
        excerpt[n++] = *s;
    excerpt[n] = '\0';
    WARN(sc->c, "script: %s - not converted: %s\n", what, excerpt);
}

static val_t make(uint8_t kind)
{
    val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = kind;
    return v;
}

static val_t* lookup(script_t* sc, const char* name, size_t n)
{
    for (uint32_t i = sc->scope_count; i > 0; i--)
    {
        if (same_name(sc->scope[i - 1U].name, sc->scope[i - 1U].length, name, n))
            return &sc->scope[i - 1U].value;
    }
    for (uint32_t i = 0; i < sc->global_count; i++)
    {
        if (same_name(sc->globals[i].name, sc->globals[i].length, name, n))
            return &sc->globals[i].value;
    }
    return NULL;
}

/* ---- Changes ---- */

static change_t* change(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    for (uint32_t i = 0; i < sc->change_count; i++)
    {
        change_t* ch = &sc->changes[i];
        if (ch->element == e && ch->mod.kind == kind && strcmp(ch->mod.name, name) == 0 &&
            ((value == NULL && ch->mod.value == NULL) || (value != NULL && ch->mod.value != NULL && strcmp(ch->mod.value, value) == 0)))
            return ch;
    }
    if (sc->pass != PASS_CHANGES || sc->change_count >= MAX_MODS)
        return NULL;
    change_t* ch = &sc->changes[sc->change_count++];
    memset(ch, 0, sizeof(*ch));
    ch->element = e;
    ch->mod.element = e->index;
    ch->mod.kind = kind;
    ch->mod.name = name;
    ch->mod.value = value;
    return ch;
}

static classvar_t* classvar(script_t* sc, node_t* e, const char* name, bool make_it)
{
    for (uint32_t i = 0; i < sc->classvar_count; i++)
    {
        if (sc->classvars[i].element == e && strcmp(sc->classvars[i].name, name) == 0)
            return &sc->classvars[i];
    }
    if (!make_it || sc->pass != PASS_CHANGES || sc->classvar_count >= MAX_CLASSVARS)
        return NULL;
    classvar_t* cv = &sc->classvars[sc->classvar_count++];
    cv->element = e;
    cv->name = name;
    cv->var = 0;
    return cv;
}

static void emit(script_t* sc, uint8_t kind, dmvsi_var_t var, int32_t value)
{
    if (sc->pass != PASS_EMIT || var == 0 || sc->action_count >= MAX_ACTIONS)
        return;
    dmvsi_action_t* a = &sc->actions[sc->action_count++];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->var = var;
    a->value = value;
}

static void emit_end(script_t* sc)
{
    if (sc->pass != PASS_EMIT || sc->action_count >= MAX_ACTIONS)
        return;
    memset(&sc->actions[sc->action_count], 0, sizeof(dmvsi_action_t));
    sc->actions[sc->action_count++].kind = DMVSI_ACT_END;
}

/* A variable set, or animated (its transition) */
static void emit_to(script_t* sc, dmvsi_var_t var, int32_t value, uint16_t ms, const int16_t* easing)
{
    emit(sc, (ms > 0) ? DMVSI_ACT_ANIMATE : DMVSI_ACT_SET, var, value);
    if (ms > 0 && sc->pass == PASS_EMIT && sc->action_count > 0 && sc->actions[sc->action_count - 1U].var == var)
    {
        sc->actions[sc->action_count - 1U].duration = ms;
        memcpy(sc->actions[sc->action_count - 1U].easing, easing, 4 * sizeof(int16_t));
    }
}

/* The actions of a change of one element: its variables to where the change puts it */
static void apply(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    change_t* ch = change(sc, e, kind, name, value);
    if (sc->pass != PASS_EMIT || ch == NULL)
        return;
    if (kind == MOD_CLASSES)
    {
        for (uint32_t i = 0; i < sc->lookvar_count; i++)
        {
            if (sc->lookvars[i].element == e)
                emit(sc, DMVSI_ACT_SET, sc->lookvars[i].var, (int32_t)ch->look_index);
        }
    }
    else if (kind != MOD_STYLE)
    {
        classvar_t* cv = classvar(sc, e, name, false);
        if (cv != NULL)
            emit(sc, DMVSI_ACT_SET, cv->var, (kind == MOD_CLASS_ADD) ? 1 : 0);
    }
    if (e->dynamic == NULL || ch->changed)
        return;
    int32_t ox, oy;
    view_origin(sc->c, &ox, &oy);
    dmvsi_rect_t r = group_rect(sc->c, e, ox, oy);
    if (e->dynamic->bind[DMVSI_BIND_X] != 0)
        emit_to(sc, e->dynamic->bind[DMVSI_BIND_X], r.x + ch->dx, ch->move_ms, ch->move_easing);
    if (e->dynamic->bind[DMVSI_BIND_Y] != 0)
        emit_to(sc, e->dynamic->bind[DMVSI_BIND_Y], r.y + ch->dy, ch->move_ms, ch->move_easing);
    if (e->dynamic->bind[DMVSI_BIND_OPACITY] != 0)
        emit_to(sc, e->dynamic->bind[DMVSI_BIND_OPACITY], ch->opacity, ch->fade_ms, ch->fade_easing);
}

/* The classes changed of each element so far, as their changes */
static void flush(script_t* sc)
{
    uint32_t count = sc->pending_count;
    sc->pending_count = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        pending_t* p = &sc->pending[i];
        if (p->count == 1)
        {
            apply(sc, p->element, p->add[0] ? MOD_CLASS_ADD : MOD_CLASS_REMOVE, p->names[0], NULL);
            continue;
        }
        /* "+a -b ...", by the classes' names: the same changes, the same text */
        char spec[512];
        size_t n = 0;
        bool used[MAX_PENDING_OPS] = { false };
        for (uint32_t k = 0; k < p->count; k++)
        {
            uint32_t best = p->count;
            for (uint32_t j = 0; j < p->count; j++)
            {
                if (!used[j] && (best == p->count || strcmp(p->names[j], p->names[best]) < 0))
                    best = j;
            }
            used[best] = true;
            n += (size_t)Dmod_SnPrintf(spec + n, sizeof(spec) - n, "%s%c%s", (n > 0) ? " " : "", p->add[best] ? '+' : '-', p->names[best]);
            if (n >= sizeof(spec))
                n = sizeof(spec) - 1U;
        }
        const char* name = arena_strndup(&sc->c->arena, spec, n);
        if (name != NULL)
            apply(sc, p->element, MOD_CLASSES, name, NULL);
    }
}

/* A class added or removed, kept with the other classes changed of the element */
static void class_op(script_t* sc, node_t* e, const char* name, bool add)
{
    pending_t* p = NULL;
    for (uint32_t i = 0; i < sc->pending_count && p == NULL; i++)
        p = (sc->pending[i].element == e) ? &sc->pending[i] : NULL;
    if (p == NULL)
    {
        if (sc->pending_count >= MAX_PENDING)
            flush(sc);
        p = &sc->pending[sc->pending_count++];
        p->element = e;
        p->count = 0;
    }
    for (uint32_t i = 0; i < p->count; i++)
    {
        if (strcmp(p->names[i], name) == 0)
        {
            p->add[i] = add;            /* The last one holds */
            return;
        }
    }
    if (p->count < MAX_PENDING_OPS)
    {
        p->names[p->count] = name;
        p->add[p->count++] = add;
    }
}

/* What the handler does next depends on run time: what it changed so far first */
static void flow(script_t* sc, uint8_t kind, dmvsi_var_t var, int32_t value)
{
    flush(sc);
    if (kind == DMVSI_ACT_END)
        emit_end(sc);
    else
        emit(sc, kind, var, value);
}

static void change_one(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    if (kind == MOD_CLASS_ADD || kind == MOD_CLASS_REMOVE)
        class_op(sc, e, name, kind == MOD_CLASS_ADD);
    else
    {
        flush(sc);
        apply(sc, e, kind, name, value);
    }
}

/* A change of what a value is: one element, or each a variable may hold (at run time) */
static void apply_to(script_t* sc, const val_t* target, uint8_t kind, const char* name, const char* value)
{
    if (sc->loading)
        return;
    if (target->element != NULL)
    {
        change_one(sc, target->element, kind, name, value);
        return;
    }
    runtime_t* rt = &sc->runtime[target->runtime - 1U];
    for (uint32_t i = 0; i < rt->domain_count; i++)
    {
        flow(sc, DMVSI_ACT_IF_EQ, rt->var, (int32_t)rt->domain[i]->index);
        change_one(sc, rt->domain[i], kind, name, value);
        flow(sc, DMVSI_ACT_END, 0, 0);
    }
}

/* ---- Running a script, abstractly ---- */

static val_t eval(script_t* sc, const js_t* n);
static void run(script_t* sc, const js_t* s);

static const char* text_of(script_t* sc, const val_t* v)
{
    char buffer[24];
    if (v->kind == V_STRING)
        return v->text;
    if (v->kind == V_NUMBER)
    {
        int32_t whole = v->num / 1000, frac = (v->num < 0 ? -v->num : v->num) % 1000;
        if (frac == 0)
            Dmod_SnPrintf(buffer, sizeof(buffer), "%d", (int)whole);
        else
            Dmod_SnPrintf(buffer, sizeof(buffer), "%d.%03d", (int)whole, (int)frac);
        return arena_strndup(&sc->c->arena, buffer, strlen(buffer));
    }
    return NULL;
}

/* f(args): a function of the page inlined, with its arguments' values (and, an arrow
 * function, the variables it saw) */
static val_t call_values(script_t* sc, const val_t* fn, const val_t* args, uint32_t count)
{
    const js_t* f = fn->function;
    if (sc->inline_depth >= MAX_INLINE || f == NULL)
        return make(V_UNKNOWN);
    uint32_t saved_scope = sc->scope_count;
    bool saved_returned = sc->returned;
    val_t saved_result = sc->result;
    for (uint32_t i = 0; i < fn->closure_count && sc->scope_count < MAX_SCOPE; i++)
        sc->scope[sc->scope_count++] = fn->closure[i];
    uint32_t i = 0;
    for (const js_t* p = f->list; p != NULL; p = p->next, i++)
    {
        if (sc->scope_count < MAX_SCOPE)
        {
            sc->scope[sc->scope_count].name = p->name;
            sc->scope[sc->scope_count].length = p->length;
            sc->scope[sc->scope_count].value = (i < count) ? args[i] : make(V_UNDEFINED);
            sc->scope_count++;
        }
    }
    sc->inline_depth++;
    sc->returned = false;
    sc->result = make(V_UNDEFINED);
    uint32_t saved_conditional = sc->conditional;
    sc->conditional = 0;
    val_t result = make(V_UNDEFINED);
    if (f->kind == J_FUNC && f->b == NULL)
        result = eval(sc, f->a);        /* x => expression */
    else if (f->b != NULL)
    {
        run(sc, f->b);
        result = sc->result;
    }
    sc->conditional = saved_conditional;
    sc->inline_depth--;
    sc->returned = saved_returned;
    sc->result = saved_result;
    sc->scope_count = saved_scope;
    return result;
}

static val_t call_function(script_t* sc, const val_t* fn, const js_t* args)
{
    val_t values[8];
    uint32_t n = 0;
    for (const js_t* a = args; a != NULL; a = a->next)
    {
        val_t v = eval(sc, a);
        if (n < 8U)
            values[n++] = v;
    }
    return call_values(sc, fn, values, n);
}

/* A function as a value: an arrow function sees the variables where it is made */
static val_t function_value(script_t* sc, const js_t* n)
{
    val_t v = make(V_FUNCTION);
    v.function = n;
    if (sc->scope_count > 0)
    {
        v.closure = arena_alloc(&sc->c->arena, sc->scope_count * sizeof(binding_t));
        if (v.closure != NULL)
        {
            memcpy(v.closure, sc->scope, sc->scope_count * sizeof(binding_t));
            v.closure_count = sc->scope_count;
        }
    }
    return v;
}

static void run_click(script_t* sc, node_t* e);

/* x.classList.add('a'), document.getElementById('a'), f(...) */
static val_t call(script_t* sc, const js_t* n)
{
    const js_t* f = n->a;
    const js_t* arg = n->list;
    if (f != NULL && f->kind == J_MEMBER)
    {
        val_t object = eval(sc, f->a);
        if (object.kind == V_DOCUMENT && is_name(f, "getElementById"))
        {
            val_t id = (arg != NULL) ? eval(sc, arg) : make(V_UNKNOWN);
            if (id.kind != V_STRING)
            {
                report(sc, n, "an element not known at conversion");
                return make(V_UNKNOWN);
            }
            node_t* e = find_id(sc->c->document, id.text, 0);
            val_t v = make(e != NULL ? V_ELEMENT : V_NULL);
            v.element = e;
            return v;
        }
        if ((object.kind == V_DOCUMENT || (object.kind == V_ELEMENT && object.element != NULL)) &&
            (is_name(f, "querySelector") || is_name(f, "querySelectorAll")))
        {
            /* The elements of a selector: at conversion, as the page is */
            val_t sel = (arg != NULL) ? eval(sc, arg) : make(V_UNKNOWN);
            if (sel.kind != V_STRING)
            {
                report(sc, n, "a selector not known at conversion");
                return make(V_UNKNOWN);
            }
            node_t* found[MAX_DOMAIN];
            node_t* under = (object.kind == V_ELEMENT) ? object.element : sc->c->document;
            uint32_t count = css_select(sc->c, sel.text, under, found, MAX_DOMAIN);
            if (is_name(f, "querySelector"))
            {
                val_t v = make(count > 0 ? V_ELEMENT : V_NULL);
                v.element = (count > 0) ? found[0] : NULL;
                return v;
            }
            val_t v = make(V_LIST);
            v.items = (count > 0) ? arena_alloc(&sc->c->arena, count * sizeof(node_t*)) : NULL;
            if (v.items != NULL)
            {
                memcpy(v.items, found, count * sizeof(node_t*));
                v.count = count;
            }
            return v;
        }
        if (object.kind == V_LIST && is_name(f, "forEach"))
        {
            /* Each element in turn - unrolled */
            val_t fn = (arg != NULL) ? eval(sc, arg) : make(V_UNKNOWN);
            if (fn.kind != V_FUNCTION)
            {
                report(sc, n, "forEach() of what is not a function");
                return make(V_UNKNOWN);
            }
            for (uint32_t i = 0; i < object.count; i++)
            {
                val_t args[2] = { make(V_ELEMENT), make(V_NUMBER) };
                args[0].element = object.items[i];
                args[1].num = (int32_t)i * 1000;
                (void)call_values(sc, &fn, args, 2);
            }
            return make(V_UNDEFINED);
        }
        if (object.kind == V_ELEMENT && object.element != NULL && is_name(f, "getAttribute"))
        {
            val_t name = (arg != NULL) ? eval(sc, arg) : make(V_UNKNOWN);
            const char* value = (name.kind == V_STRING) ? node_attr(object.element, name.text) : NULL;
            if (name.kind != V_STRING)
                return make(V_UNKNOWN);
            val_t v = make(value != NULL ? V_STRING : V_NULL);
            v.text = value;
            return v;
        }
        if (object.kind == V_ELEMENT && object.element != NULL && is_name(f, "addEventListener"))
        {
            val_t type = (arg != NULL) ? eval(sc, arg) : make(V_UNKNOWN);
            val_t fn = (arg != NULL && arg->next != NULL) ? eval(sc, arg->next) : make(V_UNKNOWN);
            if (!sc->loading || type.kind != V_STRING || strcmp(type.text, "click") != 0 || fn.kind != V_FUNCTION)
            {
                report(sc, n, sc->loading ? "a listener of what is not a click" : "a listener added by a handler");
                return make(V_UNKNOWN);
            }
            if (sc->listener_count < MAX_LISTENERS)
                sc->listeners[sc->listener_count++] = (listener_t){ object.element, fn };
            return make(V_UNDEFINED);
        }
        if (object.kind == V_ELEMENT && object.element != NULL && is_name(f, "click") && !sc->loading)
        {
            run_click(sc, object.element);   /* What clicking it does, here */
            return make(V_UNDEFINED);
        }
        if (object.kind == V_CLASSES && (is_name(f, "add") || is_name(f, "remove") || is_name(f, "toggle")))
        {
            if (sc->loading)
            {
                report(sc, n, "what the page does when it loads");
                return make(V_UNKNOWN);
            }
            for (; arg != NULL; arg = arg->next)
            {
                val_t cls = eval(sc, arg);
                if (cls.kind != V_STRING)
                {
                    report(sc, n, "a class not known at conversion");
                    continue;
                }
                if (!is_name(f, "toggle"))
                {
                    apply_to(sc, &object, is_name(f, "add") ? MOD_CLASS_ADD : MOD_CLASS_REMOVE, cls.text, NULL);
                    continue;
                }
                /* toggle: its class's variable flipped, then what is is applied */
                if (object.element == NULL)
                {
                    report(sc, n, "classList.toggle() of a variable");
                    continue;
                }
                flush(sc);
                classvar_t* cv = classvar(sc, object.element, cls.text, true);
                (void)change(sc, object.element, MOD_CLASS_ADD, cls.text, NULL);
                (void)change(sc, object.element, MOD_CLASS_REMOVE, cls.text, NULL);
                if (sc->pass == PASS_EMIT && cv != NULL)
                {
                    emit(sc, DMVSI_ACT_TOGGLE, cv->var, 0);
                    emit(sc, DMVSI_ACT_IF_NE, cv->var, 0);
                    apply(sc, object.element, MOD_CLASS_ADD, cls.text, NULL);
                    emit_end(sc);
                    emit(sc, DMVSI_ACT_IF_EQ, cv->var, 0);
                    apply(sc, object.element, MOD_CLASS_REMOVE, cls.text, NULL);
                    emit_end(sc);
                }
            }
            return make(V_UNDEFINED);
        }
        if (object.kind == V_CLASSES && is_name(f, "contains"))
            return make(V_UNKNOWN);     /* As a condition: see condition() */
    }
    if (f != NULL && f->kind == J_IDENT)
    {
        val_t* v = lookup(sc, f->name, f->length);
        if (v != NULL && v->kind == V_FUNCTION)
        {
            if (sc->loading)
            {
                report(sc, n, "what the page does when it loads");
                return make(V_UNKNOWN);
            }
            val_t fn = *v;
            return call_function(sc, &fn, arg);
        }
    }
    report(sc, n, "a call");
    return make(V_UNKNOWN);
}

static val_t eval(script_t* sc, const js_t* n)
{
    if (n == NULL)
        return make(V_UNDEFINED);
    switch (n->kind)
    {
        case J_NUMBER:
        {
            val_t v = make(V_NUMBER);
            v.num = n->num;
            return v;
        }
        case J_STRING:
        {
            val_t v = make(V_STRING);
            v.text = n->name;
            return v;
        }
        case J_NULL:
            return make(V_NULL);
        case J_UNDEFINED:
            return make(V_UNDEFINED);
        case J_TRUE:
        case J_FALSE:
        {
            val_t v = make(V_BOOL);
            v.num = n->kind == J_TRUE;
            return v;
        }
        case J_THIS:
        {
            val_t v = make((sc->self != NULL) ? V_ELEMENT : V_UNKNOWN);
            v.element = sc->self;
            return v;
        }
        case J_IDENT:
        {
            if (is_name(n, "document"))
                return make(V_DOCUMENT);
            val_t* v = lookup(sc, n->name, n->length);
            return (v != NULL) ? *v : make(V_UNKNOWN);
        }
        case J_MEMBER:
        {
            val_t object = eval(sc, n->a);
            if ((object.kind == V_ELEMENT || object.kind == V_RUNTIME) && (is_name(n, "classList") || is_name(n, "style")))
            {
                object.kind = is_name(n, "style") ? V_STYLE : V_CLASSES;
                return object;
            }
            return make(V_UNKNOWN);
        }
        case J_CALL:
            return call(sc, n);
        case J_FUNC:
            return function_value(sc, n);
        case J_ADD:
        {
            val_t a = eval(sc, n->a), b = eval(sc, n->b);
            if (a.kind == V_NUMBER && b.kind == V_NUMBER)
            {
                a.num += b.num;
                return a;
            }
            const char* ta = text_of(sc, &a);
            const char* tb = text_of(sc, &b);
            if ((a.kind != V_STRING && b.kind != V_STRING) || ta == NULL || tb == NULL)
                return make(V_UNKNOWN);
            size_t la = strlen(ta), lb = strlen(tb);
            char* t = arena_alloc(&sc->c->arena, la + lb + 1U);
            if (t == NULL)
                return make(V_UNKNOWN);
            memcpy(t, ta, la);
            memcpy(t + la, tb, lb);
            val_t v = make(V_STRING);
            v.text = t;
            return v;
        }
        case J_ASSIGN:
        {
            const js_t* target = n->a;
            val_t value = eval(sc, n->b);
            if (target != NULL && target->kind == J_MEMBER)
            {
                val_t object = eval(sc, target->a);
                if (object.kind == V_ELEMENT && object.element != NULL && is_name(target, "onclick") && sc->loading &&
                    value.kind == V_FUNCTION)
                {
                    if (sc->listener_count < MAX_LISTENERS)
                        sc->listeners[sc->listener_count++] = (listener_t){ object.element, value };
                    return value;
                }
                if (object.kind == V_STYLE && sc->loading)
                {
                    report(sc, n, "what the page does when it loads");
                    return make(V_UNKNOWN);
                }
                if (object.kind == V_STYLE)
                {
                    const char* text = text_of(sc, &value);
                    if (text == NULL)
                    {
                        report(sc, n, "a style not known at conversion");
                        return make(V_UNKNOWN);
                    }
                    char* name = arena_strndup(&sc->c->arena, target->name, target->length);
                    if (name != NULL)
                        apply_to(sc, &object, MOD_STYLE, name, text);
                    return value;
                }
                report(sc, n, "an assignment");
                return make(V_UNKNOWN);
            }
            if (target != NULL && target->kind == J_IDENT)
            {
                val_t* v = lookup(sc, target->name, target->length);
                if (v != NULL && v->kind == V_RUNTIME && sc->loading)
                {
                    report(sc, n, "what the page does when it loads");
                    return make(V_UNKNOWN);
                }
                if (v != NULL && v->kind == V_RUNTIME)
                {
                    runtime_t* rt = &sc->runtime[v->runtime - 1U];
                    if (value.kind == V_ELEMENT && value.element != NULL)
                    {
                        bool known = false;
                        for (uint32_t i = 0; i < rt->domain_count && !known; i++)
                            known = rt->domain[i] == value.element;
                        if (!known && rt->domain_count < MAX_DOMAIN)
                            rt->domain[rt->domain_count++] = value.element;
                        emit(sc, DMVSI_ACT_SET, rt->var, (int32_t)value.element->index);
                    }
                    else if (value.kind == V_NULL || value.kind == V_UNDEFINED)
                        emit(sc, DMVSI_ACT_SET, rt->var, 0);
                    else
                        report(sc, n, "a variable set to what is not an element");
                    return value;
                }
                if (v != NULL && sc->conditional == 0)
                {
                    *v = value;             /* A local, at conversion */
                    return value;
                }
            }
            report(sc, n, "an assignment");
            return make(V_UNKNOWN);
        }
        default:
            return make(V_UNKNOWN);
    }
}

/* A condition: known at conversion (*known, *truth), or a test of a variable at run time */
typedef struct
{
    bool            known;
    bool            truth;
    dmvsi_var_t     var;            /* Run time: var == value (equal) or != */
    int32_t         value;
    bool            equal;
} cond_t;

static cond_t condition(script_t* sc, const js_t* n)
{
    cond_t c;
    memset(&c, 0, sizeof(c));
    if (n == NULL)
        return c;
    if (n->kind == J_NOT)
    {
        c = condition(sc, n->a);
        c.truth = !c.truth;
        c.equal = !c.equal;
        return c;
    }
    if (n->kind == J_CALL && n->a != NULL && n->a->kind == J_MEMBER && is_name(n->a, "contains"))
    {
        val_t object = eval(sc, n->a->a);
        val_t cls = (n->list != NULL) ? eval(sc, n->list) : make(V_UNKNOWN);
        if (object.kind == V_CLASSES && object.element != NULL && cls.kind == V_STRING)
        {
            classvar_t* cv = classvar(sc, object.element, cls.text, true);
            if (cv != NULL && (cv->var != 0 || sc->pass != PASS_EMIT))
            {
                c.var = cv->var;
                c.value = 0;
                c.equal = false;
                return c;
            }
        }
        report(sc, n, "a condition");
        c.known = true;
        return c;
    }
    if (n->kind == J_EQ || n->kind == J_NE)
    {
        val_t a = eval(sc, n->a), b = eval(sc, n->b);
        if (a.kind != V_RUNTIME)
        {
            val_t t = a;
            a = b;
            b = t;
        }
        if (a.kind == V_RUNTIME && (b.kind == V_ELEMENT || b.kind == V_NULL || b.kind == V_UNDEFINED))
        {
            c.var = sc->runtime[a.runtime - 1U].var;
            c.value = (b.kind == V_ELEMENT && b.element != NULL) ? (int32_t)b.element->index : 0;
            c.equal = n->kind == J_EQ;
            return c;
        }
        if (a.kind == V_STRING && b.kind == V_STRING)
        {
            c.known = true;
            c.truth = (strcmp(a.text, b.text) == 0) == (n->kind == J_EQ);
            return c;
        }
        report(sc, n, "a condition");
        c.known = true;
        return c;
    }
    val_t v = eval(sc, n);
    switch (v.kind)
    {
        case V_RUNTIME:
            c.var = sc->runtime[v.runtime - 1U].var;
            c.value = 0;
            c.equal = false;
            return c;
        case V_ELEMENT:
        case V_FUNCTION:
        case V_DOCUMENT:
        case V_CLASSES:
        case V_STYLE:
            c.known = true;
            c.truth = true;
            return c;
        case V_NULL:
        case V_UNDEFINED:
            c.known = true;
            return c;
        case V_BOOL:
        case V_NUMBER:
            c.known = true;
            c.truth = v.num != 0;
            return c;
        case V_STRING:
            c.known = true;
            c.truth = v.text[0] != '\0';
            return c;
        default:
            report(sc, n, "a condition");
            c.known = true;         /* As false: what it guards is left out */
            return c;
    }
}

static void run_list(script_t* sc, const js_t* s)
{
    for (; s != NULL && !sc->returned; s = s->next)
        run(sc, s);
}

static void run(script_t* sc, const js_t* s)
{
    if (s == NULL || sc->c->arena.failed)
        return;
    switch (s->kind)
    {
        case S_BLOCK:
        {
            uint32_t saved = sc->scope_count;
            run_list(sc, s->list);
            sc->scope_count = saved;
            return;
        }
        case S_EXPR:
            (void)eval(sc, s->a);
            return;
        case S_VAR:
            if (sc->scope_count < MAX_SCOPE)
            {
                sc->scope[sc->scope_count].name = s->name;
                sc->scope[sc->scope_count].length = s->length;
                sc->scope[sc->scope_count].value = eval(sc, s->a);
                sc->scope_count++;
            }
            return;
        case S_IF:
        {
            cond_t c = condition(sc, s->a);
            if (c.known)
            {
                if (c.truth)
                    run(sc, s->b);
                else if (s->c != NULL)
                    run(sc, s->c);
                return;
            }
            if (sc->loading)
            {
                report(sc, s, "what the page does when it loads");
                return;
            }
            sc->conditional++;
            flow(sc, c.equal ? DMVSI_ACT_IF_EQ : DMVSI_ACT_IF_NE, c.var, c.value);
            run(sc, s->b);
            flow(sc, DMVSI_ACT_END, 0, 0);
            if (s->c != NULL)
            {
                flow(sc, c.equal ? DMVSI_ACT_IF_NE : DMVSI_ACT_IF_EQ, c.var, c.value);
                run(sc, s->c);
                flow(sc, DMVSI_ACT_END, 0, 0);
            }
            sc->conditional--;
            sc->returned = false;
            return;
        }
        case S_RETURN:
            if (sc->conditional > 0)
                report(sc, s, "a return in a condition of run time");
            else
            {
                sc->result = eval(sc, s->a);
                sc->returned = true;
            }
            return;
        case S_FUNC:
        case S_EMPTY:
            return;
        default:
            report(sc, s, "a statement");
            return;
    }
}

/* ---- The page's scripts ---- */

typedef struct
{
    js_t*       programs[16];
    uint32_t    count;
} scripts_t;

static void find_scripts(conv_t* c, node_t* n, scripts_t* out, uint32_t depth)
{
    if (depth > 200U)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (node_is(k, "script"))
        {
            const char* type = node_attr(k, "type");
            bool js = type == NULL || type[0] == '\0' || strcmp(type, "module") == 0 || strcmp(type, "text/javascript") == 0;
            if (js && node_attr(k, "src") == NULL && k->first != NULL && k->first->kind == NODE_TEXT && out->count < 16U)
                out->programs[out->count++] = parse(c, k->first->text, k->first->length);
            continue;
        }
        find_scripts(c, k, out, depth + 1U);
    }
}

/* The globals: functions, and the variables of the top level (what a handler assigns runs at run time) */
static bool assigns(const js_t* s, const char* name, size_t n)
{
    for (; s != NULL; s = s->next)
    {
        if (s->kind == J_ASSIGN && s->a != NULL && s->a->kind == J_IDENT && same_name(s->a->name, s->a->length, name, n))
            return true;
        if (assigns(s->a, name, n) || assigns(s->b, name, n) || assigns(s->c, name, n) || assigns(s->list, name, n))
            return true;
    }
    return false;
}

static void declare_globals(script_t* sc, const scripts_t* scripts)
{
    for (uint32_t p = 0; p < scripts->count; p++)
    {
        for (const js_t* s = scripts->programs[p]; s != NULL; s = s->next)
        {
            const js_t* list = (s->kind == S_BLOCK) ? s->list : s;
            for (const js_t* d = list; d != NULL; d = (s->kind == S_BLOCK) ? d->next : NULL)
            {
                if (sc->global_count >= MAX_GLOBALS || (d->kind != S_FUNC && d->kind != S_VAR))
                {
                    if (s->kind != S_BLOCK)
                        break;
                    continue;
                }
                binding_t* g = &sc->globals[sc->global_count++];
                g->name = d->name;
                g->length = d->length;
                if (d->kind == S_FUNC)
                {
                    g->value = make(V_FUNCTION);
                    g->value.function = d;
                }
                else
                {
                    g->value = eval(sc, d->a);
                    bool assigned = false;
                    for (uint32_t q = 0; q < scripts->count && !assigned; q++)
                    {
                        for (const js_t* f = scripts->programs[q]; f != NULL && !assigned; f = f->next)
                            assigned = f->kind == S_FUNC && assigns(f->b, d->name, d->length);
                    }
                    if (assigned && sc->runtime_count < MAX_RUNTIME)
                    {
                        runtime_t* rt = &sc->runtime[sc->runtime_count++];
                        memset(rt, 0, sizeof(*rt));
                        rt->name = d->name;
                        rt->length = d->length;
                        if (g->value.kind == V_ELEMENT && g->value.element != NULL)
                        {
                            rt->initial = (int32_t)g->value.element->index;
                            rt->domain[rt->domain_count++] = g->value.element;
                        }
                        g->value = make(V_RUNTIME);
                        g->value.runtime = sc->runtime_count;
                    }
                }
                if (s->kind != S_BLOCK)
                    break;
            }
        }
    }
}

/* ---- Handlers ---- */

static bool listened(const script_t* sc, const node_t* e)
{
    for (uint32_t i = 0; i < sc->listener_count; i++)
    {
        if (sc->listeners[i].element == e)
            return true;
    }
    return false;
}

/* What clicking e does: its onclick, then its listeners - `this` e */
static void run_click(script_t* sc, node_t* e)
{
    if (sc->click_depth >= 4U)
        return;
    sc->click_depth++;
    node_t* saved_self = sc->self;
    uint32_t saved_scope = sc->scope_count;
    bool saved_returned = sc->returned;
    sc->self = e;
    const char* onclick = node_attr(e, "onclick");
    if (onclick != NULL)
    {
        js_t* program = parse(sc->c, onclick, strlen(onclick));
        sc->returned = false;
        run_list(sc, program);
    }
    for (uint32_t i = 0; i < sc->listener_count; i++)
    {
        if (sc->listeners[i].element != e)
            continue;
        val_t event = make(V_UNKNOWN);
        sc->returned = false;
        (void)call_values(sc, &sc->listeners[i].function, &event, 1);
    }
    sc->self = saved_self;
    sc->scope_count = saved_scope;
    sc->returned = saved_returned;
    sc->click_depth--;
}

static void handlers(script_t* sc, node_t* n, uint32_t depth)
{
    if (depth > 200U)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT || k->pseudo != PSEUDO_NONE)
            continue;
        bool clicks = node_attr(k, "onclick") != NULL || listened(sc, k);
        if (clicks && k->style != NULL && k->style->display != DISPLAY_NONE)
        {
            if (sc->pass == PASS_CHANGES && sc->clickable_count < MAX_MODS)
                sc->clickables[sc->clickable_count++] = k;
            sc->action_count = 0;
            sc->scope_count = 0;
            sc->returned = false;
            sc->conditional = 0;
            sc->pending_count = 0;
            run_click(sc, k);
            flush(sc);
            if (sc->pass == PASS_EMIT && sc->action_count > 0)
            {
                dmvsi_handler_t h = dmvsi_add_handler(sc->c->doc, sc->actions, sc->action_count);
                if (h != 0)
                {
                    if (k->dynamic == NULL)
                        k->dynamic = arena_alloc(&sc->c->arena, sizeof(dynamic_t));
                    if (k->dynamic != NULL)
                        k->dynamic->click = h;
                }
            }
        }
        handlers(sc, k, depth + 1U);
    }
}

/* ---- States: the page laid out with a change ---- */

/* A digest of an element's inside: its boxes and lines relative to it */
static uint32_t inside(const node_t* e, const node_t* n, uint32_t h, uint32_t depth)
{
    if (depth > 200U)
        return h;
    for (const node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT || k->style == NULL || k->style->display == DISPLAY_NONE)
            continue;
        /* Inline elements have no position of their own (their lines are their block's) */
        int32_t v[6] = { k->box.placed ? k->box.ax - e->box.ax : 0, k->box.placed ? k->box.ay - e->box.ay : 0,
                         k->box.w, k->box.h, (int32_t)k->style->color, (int32_t)k->style->background };
        for (int i = 0; i < 6; i++)
            h = (h ^ (uint32_t)v[i]) * 16777619u;
        for (const frag_t* f = k->box.frags; f != NULL; f = f->next)
            h = (h ^ (uint32_t)(f->x + f->y * 7 + (int32_t)f->length)) * 16777619u;
        h = inside(e, k, h, depth + 1U);
    }
    return h;
}

static node_t* element_index(node_t* n, uint32_t index, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->index == index)
            return k;
        node_t* found = element_index(k, index, depth + 1U);
        if (found != NULL)
            return found;
    }
    return NULL;
}

static bool has_class(const node_t* n, const char* name)
{
    for (uint32_t i = 0; i < n->class_count; i++)
    {
        if (strcmp(n->classes[i], name) == 0)
            return true;
    }
    return false;
}

/* The states laid out, kept for painting their looks (c->states) */
#define MAX_STATES      48u

typedef struct
{
    conv_t*     convs[MAX_STATES];
    mod_t       mods[MAX_STATES][2];
    uint32_t    count;
} states_t;

void script_free(conv_t* c)
{
    states_t* st = c->states;
    if (st == NULL)
        return;
    for (uint32_t i = 0; i < st->count; i++)
    {
        arena_free(&st->convs[i]->arena);
        Dmod_Free(st->convs[i]);
    }
    Dmod_Free(st);
    c->states = NULL;
}

/* The page laid out with up to two changes, kept until it is painted */
static conv_t* run_state(script_t* sc, const mod_t* a, const mod_t* b, int* status)
{
    conv_t* c = sc->c;
    states_t* st = c->states;
    if (st == NULL)
    {
        if ((st = Dmod_Malloc(sizeof(*st))) == NULL)
        {
            *status = -ENOMEM;
            return NULL;
        }
        memset(st, 0, sizeof(*st));
        c->states = st;
    }
    if (st->count >= MAX_STATES)
    {
        WARN(c, "script: too many states of the page - the rest not converted\n");
        return NULL;
    }
    conv_t* s = Dmod_Malloc(sizeof(*s));
    if (s == NULL)
    {
        *status = -ENOMEM;
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->options = c->options;
    s->doc = c->doc;
    s->vw = c->vw;
    s->vh = c->vh;
    s->path = c->path;
    st->mods[st->count][0] = *a;
    if (b != NULL)
        st->mods[st->count][1] = *b;
    s->mods = st->mods[st->count];
    s->mod_count = (b != NULL) ? 2U : 1U;
    st->convs[st->count++] = s;
    *status = run_layout(s);
    return (*status == 0) ? s : NULL;
}

static uint32_t mix(uint32_t h, int32_t v)
{
    return (h ^ (uint32_t)v) * 16777619u;
}

/* A digest of how an element looks, but its opacity: its own style and its inside */
static uint32_t look_of(const node_t* e)
{
    const style_t* st = e->style;
    uint32_t h = 2166136261u;
    h = mix(h, (int32_t)st->color);
    h = mix(h, (int32_t)st->background);
    h = mix(h, e->box.w);
    h = mix(h, e->box.h);
    for (int i = 0; i < 4; i++)
    {
        h = mix(h, st->border_w[i]);
        h = mix(h, (int32_t)st->border_color[i]);
        h = mix(h, st->radius[i].px);
    }
    if (st->background_image != NULL)
    {
        for (uint32_t i = 0; i < st->background_image->count; i++)
            h = mix(h, (int32_t)st->background_image->colors[i]);
    }
    for (const char* u = st->background_url; u != NULL && *u != '\0'; u++)
        h = mix(h, *u);
    for (const frag_t* f = e->box.frags; f != NULL; f = f->next)
        h = mix(h, f->x + f->y * 7 + (int32_t)f->length);
    return inside(e, e, h, 0);
}

/* How opaque an element is seen: not at all while it is hidden (visibility) */
static int32_t seen_opacity(const style_t* st)
{
    return st->hidden ? 0 : st->opacity;
}

/* "+a -b": whether the element has all that already */
static bool has_classes(const node_t* e, const char* spec)
{
    char name[128];
    for (const char* q = spec; *q != '\0'; )
    {
        while (*q == ' ')
            q++;
        if (*q != '+' && *q != '-')
            break;
        bool add = *q++ == '+';
        size_t n = 0;
        while (q[n] != '\0' && q[n] != ' ')
            n++;
        size_t k = (n < sizeof(name) - 1U) ? n : sizeof(name) - 1U;
        memcpy(name, q, k);
        name[k] = '\0';
        if (has_class(e, name) != add)
            return false;
        q += n;
    }
    return true;
}

/* An element a change shows that is hidden (visibility) as the page is: shown, its opacity
 * what hides it - so that it can fade in. Its inside hidden as it is then */
static void reveal(node_t* e, const node_t* f, uint32_t depth)
{
    if (e->style != NULL && f->style != NULL && e->kind == NODE_ELEMENT && depth == 0)
        e->style->hidden = false;
    const node_t* g = f->first;
    for (node_t* k = e->first; k != NULL && g != NULL && depth < 200U; k = k->next, g = g->next)
    {
        if (k->kind != g->kind)
            break;
        if (k->kind == NODE_ELEMENT && k->style != NULL && g->style != NULL)
        {
            if (k->style->hidden && !g->style->hidden)
                k->style->hidden = false;
            reveal(k, g, depth + 1U);
        }
    }
}

/* What a change does to its element: laid out with it, against the page as it is - a move and
 * a fade (its variables), or another look (painted as it is then) */
static int lay_out_change(script_t* sc, change_t* ch)
{
    conv_t* c = sc->c;
    node_t* e = ch->element;
    ch->laid_out = true;
    ch->opacity = seen_opacity(e->style);
    const style_t* after = e->style;           /* The style it has then: its transition */
    if ((ch->mod.kind == MOD_CLASS_ADD && has_class(e, ch->mod.name)) ||
        (ch->mod.kind == MOD_CLASS_REMOVE && !has_class(e, ch->mod.name)) ||
        (ch->mod.kind == MOD_CLASSES && has_classes(e, ch->mod.name)))
        ch->same = true;
    else
    {
        int status = 0;
        conv_t* s = run_state(sc, &ch->mod, NULL, &status);
        if (status != 0)
            return status;
        node_t* f = (s != NULL) ? element_index(s->document, e->index, 0) : NULL;
        if (s == NULL)
            ch->same = true;
        else if (f == NULL || !f->box.placed || f->style == NULL || f->style->display == DISPLAY_NONE)
        {
            ch->changed = true;                 /* Hidden then */
            ch->look = NULL;
        }
        else
        {
            int32_t ox, oy, sx, sy;
            view_origin(c, &ox, &oy);
            view_origin(s, &sx, &sy);
            ch->dx = (f->box.ax - sx) - (e->box.ax - ox);
            ch->dy = (f->box.ay - sy) - (e->box.ay - oy);
            ch->opacity = seen_opacity(f->style);
            if (e->style->hidden && !f->style->hidden)
                ch->reveals = f;
            if (look_of(f) != look_of(e) || !e->box.placed)
            {
                ch->changed = true;             /* Another look */
                ch->look = f;
            }
            (void)style_transition(f->style, TRANSITION_POSITION, &ch->move_ms, ch->move_easing);
            (void)style_transition(f->style, TRANSITION_OPACITY, &ch->fade_ms, ch->fade_easing);
            after = NULL;
        }
    }
    if (after != NULL)
    {
        (void)style_transition(after, TRANSITION_POSITION, &ch->move_ms, ch->move_easing);
        (void)style_transition(after, TRANSITION_OPACITY, &ch->fade_ms, ch->fade_easing);
    }
    return 0;
}

/* The looks of an element: one per state of its class (that a script changes) and of being pressed */
static int make_variants(script_t* sc, node_t* e, const change_t* toggled, bool pressable)
{
    conv_t* c = sc->c;
    int status = 0;
    node_t* pressed = NULL;
    node_t* pressed_toggled = NULL;
    mod_t active;
    memset(&active, 0, sizeof(active));
    active.element = e->index;
    active.kind = MOD_ACTIVE;
    active.name = "";
    if (pressable)
    {
        conv_t* s = run_state(sc, &active, NULL, &status);
        node_t* f = (s != NULL) ? element_index(s->document, e->index, 0) : NULL;
        if (f != NULL && f->box.placed && (look_of(f) != look_of(e) || f->style->opacity != e->style->opacity))
            pressed = f;
        if (pressed != NULL && toggled != NULL)
        {
            s = run_state(sc, &toggled->mod, &active, &status);
            pressed_toggled = (s != NULL) ? element_index(s->document, e->index, 0) : NULL;
            if (pressed_toggled != NULL && !pressed_toggled->box.placed)
                pressed_toggled = NULL;
        }
    }
    if (status != 0 || (toggled == NULL && pressed == NULL))
        return status;
    if (e->dynamic == NULL && (e->dynamic = arena_alloc(&c->arena, sizeof(dynamic_t))) == NULL)
        return -ENOMEM;
    dynamic_t* d = e->dynamic;
    classvar_t* cv = (toggled != NULL) ? classvar(sc, e, toggled->mod.name, false) : NULL;
    dmvsi_var_t var = (cv != NULL) ? cv->var : 0;
    int32_t base = (toggled != NULL && has_class(e, toggled->mod.name)) ? 1 : 0;
    variant_t looks[MAX_VARIANTS];
    uint8_t n = 0;
    looks[n++] = (variant_t){ e->box.placed ? e : NULL, var, base, (int8_t)((pressed != NULL) ? 0 : -1) };
    if (toggled != NULL)
        looks[n++] = (variant_t){ toggled->look, var, 1 - base, (int8_t)((pressed != NULL) ? 0 : -1) };
    if (pressed != NULL)
        looks[n++] = (variant_t){ pressed, var, base, 1 };
    if (pressed != NULL && toggled != NULL)
        looks[n++] = (variant_t){ pressed_toggled, var, 1 - base, 1 };
    memcpy(d->variants, looks, n * sizeof(variant_t));
    d->variant_count = n;
    return 0;
}

/* An element's variables: what its changes move and fade */
static int bind_element(script_t* sc, node_t* e)
{
    bool moves_x = false, moves_y = false, fades = false;
    for (uint32_t i = 0; i < sc->change_count; i++)
    {
        const change_t* ch = &sc->changes[i];
        if (ch->element != e || ch->changed)
            continue;
        moves_x = moves_x || ch->dx != 0;
        moves_y = moves_y || ch->dy != 0;
        fades = fades || ch->opacity != seen_opacity(e->style);
    }
    if (!moves_x && !moves_y && !fades)
        return 0;
    if (e->dynamic == NULL && (e->dynamic = arena_alloc(&sc->c->arena, sizeof(dynamic_t))) == NULL)
        return -ENOMEM;
    char name[48];
    int32_t ox, oy;
    view_origin(sc->c, &ox, &oy);
    dmvsi_rect_t r = group_rect(sc->c, e, ox, oy);
    const char* base = (e->id != NULL) ? e->id : e->tag;
    if (moves_x)
    {
        Dmod_SnPrintf(name, sizeof(name), "%s_x", base);
        e->dynamic->bind[DMVSI_BIND_X] = dmvsi_add_var(sc->c->doc, name, r.x);
    }
    if (moves_y)
    {
        Dmod_SnPrintf(name, sizeof(name), "%s_y", base);
        e->dynamic->bind[DMVSI_BIND_Y] = dmvsi_add_var(sc->c->doc, name, r.y);
    }
    if (fades)
    {
        Dmod_SnPrintf(name, sizeof(name), "%s_alpha", base);
        e->dynamic->bind[DMVSI_BIND_OPACITY] = dmvsi_add_var(sc->c->doc, name, seen_opacity(e->style));
    }
    return 0;
}

int script_compile(conv_t* c)
{
    scripts_t scripts;
    memset(&scripts, 0, sizeof(scripts));
    find_scripts(c, c->document, &scripts, 0);
    script_t* sc = Dmod_Malloc(sizeof(*sc));
    if (sc == NULL)
        return -ENOMEM;
    memset(sc, 0, sizeof(*sc));
    sc->c = c;
    declare_globals(sc, &scripts);

    /* The top level, as the page loads: the listeners it adds - what else it does is not converted */
    sc->pass = PASS_EMIT;
    sc->loading = true;
    for (uint32_t p = 0; p < scripts.count; p++)
    {
        for (const js_t* s = scripts.programs[p]; s != NULL; s = s->next)
        {
            if (s->kind == S_EXPR && s->a != NULL && s->a->kind == J_ASSIGN && s->a->a != NULL &&
                s->a->a->kind == J_MEMBER && s->a->a->a != NULL && is_name(s->a->a->a, "tailwind"))
                continue;               /* tailwind.config = ...: read with the style sheets */
            if (s->kind == S_EXPR || s->kind == S_IF)
            {
                sc->scope_count = 0;
                run(sc, s);
            }
            else if (s->kind == S_UNKNOWN)
                report(sc, s, "what the page does when it loads");
        }
    }
    sc->loading = false;
    sc->action_count = 0;
    sc->scope_count = 0;

    int status = 0;
    sc->pass = PASS_DOMAINS;
    handlers(sc, c->document, 0);
    sc->pass = PASS_CHANGES;
    handlers(sc, c->document, 0);
    for (uint32_t i = 0; i < sc->change_count && status == 0; i++)
        status = lay_out_change(sc, &sc->changes[i]);
    for (uint32_t i = 0; i < sc->change_count && status == 0; i++)
    {
        node_t* e = sc->changes[i].element;
        if (e->dynamic == NULL || (e->dynamic->bind[0] == 0 && e->dynamic->bind[1] == 0 && e->dynamic->bind[2] == 0))
            status = bind_element(sc, e);
    }
    /* What fades in from hidden: shown, at its opacity's variable */
    for (uint32_t i = 0; i < sc->change_count && status == 0; i++)
    {
        change_t* ch = &sc->changes[i];
        node_t* e = ch->element;
        if (ch->reveals != NULL && !ch->changed && e->dynamic != NULL && e->dynamic->bind[DMVSI_BIND_OPACITY] != 0)
            reveal(e, ch->reveals, 0);
    }

    /* The looks of classes changed at once: each element's variable, a value per look */
    for (uint32_t i = 0; i < sc->change_count && status == 0; i++)
    {
        change_t* ch = &sc->changes[i];
        if (ch->mod.kind != MOD_CLASSES || !ch->changed || ch->look == NULL)
            continue;
        lookvar_t* lv = NULL;
        for (uint32_t k = 0; k < sc->lookvar_count && lv == NULL; k++)
            lv = (sc->lookvars[k].element == ch->element) ? &sc->lookvars[k] : NULL;
        if (lv == NULL && sc->lookvar_count < MAX_LOOKVARS)
        {
            lv = &sc->lookvars[sc->lookvar_count++];
            memset(lv, 0, sizeof(*lv));
            lv->element = ch->element;
        }
        if (lv == NULL)
            continue;
        uint32_t digest = look_of(ch->look);
        for (uint32_t k = 0; k < lv->count && ch->look_index == 0; k++)
            ch->look_index = (lv->digests[k] == digest) ? k + 1U : 0U;
        if (ch->look_index == 0 && lv->count < MAX_VARIANTS - 1U)
        {
            lv->looks[lv->count] = ch->look;
            lv->digests[lv->count++] = digest;
            ch->look_index = lv->count;
        }
        else if (ch->look_index == 0)
            WARN(c, "script: too many looks of #%s - not converted\n", (ch->element->id != NULL) ? ch->element->id : ch->element->tag);
    }
    for (uint32_t i = 0; i < sc->lookvar_count && status == 0; i++)
    {
        lookvar_t* lv = &sc->lookvars[i];
        node_t* e = lv->element;
        char name[48];
        Dmod_SnPrintf(name, sizeof(name), "%s_look", (e->id != NULL) ? e->id : e->tag);
        if ((lv->var = dmvsi_add_var(c->doc, name, 0)) == 0 ||
            (e->dynamic == NULL && (e->dynamic = arena_alloc(&c->arena, sizeof(dynamic_t))) == NULL))
        {
            status = -ENOMEM;
            break;
        }
        variant_t looks[MAX_VARIANTS];
        uint8_t n = 0;
        looks[n++] = (variant_t){ e->box.placed ? e : NULL, lv->var, 0, -1 };
        for (uint32_t k = 0; k < lv->count; k++)
            looks[n++] = (variant_t){ lv->looks[k], lv->var, (int32_t)(k + 1U), -1 };
        memcpy(e->dynamic->variants, looks, n * sizeof(variant_t));
        e->dynamic->variant_count = n;
    }

    /* Another look of a class: the class's variable, asked by the looks */
    sc->pass = PASS_CHANGES;
    for (uint32_t i = 0; i < sc->change_count; i++)
    {
        change_t* ch = &sc->changes[i];
        if (ch->mod.kind == MOD_CLASSES)
            continue;
        if (ch->changed && ch->mod.kind != MOD_STYLE)
            (void)classvar(sc, ch->element, ch->mod.name, true);
        else if (ch->changed)
            WARN(c, "script: a style that changes how #%s looks - not converted\n", (ch->element->id != NULL) ? ch->element->id : ch->element->tag);
    }

    /* The variables of the script: elements as their indices; the classes asked about */
    for (uint32_t i = 0; i < sc->runtime_count && status == 0; i++)
    {
        runtime_t* rt = &sc->runtime[i];
        char name[48];
        size_t n = (rt->length < sizeof(name) - 1U) ? rt->length : sizeof(name) - 1U;
        memcpy(name, rt->name, n);
        name[n] = '\0';
        if ((rt->var = dmvsi_add_var(c->doc, name, rt->initial)) == 0)
            status = -ENOMEM;
    }
    for (uint32_t i = 0; i < sc->classvar_count && status == 0; i++)
    {
        classvar_t* cv = &sc->classvars[i];
        char name[48];
        Dmod_SnPrintf(name, sizeof(name), "%s_%s", (cv->element->id != NULL) ? cv->element->id : cv->element->tag, cv->name);
        if ((cv->var = dmvsi_add_var(c->doc, name, has_class(cv->element, cv->name) ? 1 : 0)) == 0)
            status = -ENOMEM;
    }
    /* The looks: per element, its first class that changes how it looks, and being pressed */
    for (uint32_t i = 0; i < sc->change_count && status == 0; i++)
    {
        change_t* ch = &sc->changes[i];
        bool first = ch->changed && ch->mod.kind != MOD_STYLE && ch->mod.kind != MOD_CLASSES &&
                     (ch->element->dynamic == NULL || ch->element->dynamic->variant_count == 0);
        for (uint32_t k = 0; k < i && first; k++)
            first = !(sc->changes[k].element == ch->element && sc->changes[k].changed && sc->changes[k].mod.kind != MOD_STYLE);
        if (first)
        {
            bool pressable = c->has_active && (node_attr(ch->element, "onclick") != NULL || listened(sc, ch->element));
            status = make_variants(sc, ch->element, ch, pressable);
        }
    }
    for (uint32_t i = 0; i < sc->clickable_count && status == 0; i++)
    {
        node_t* e = sc->clickables[i];
        if (c->has_active && (e->dynamic == NULL || e->dynamic->variant_count == 0))
            status = make_variants(sc, e, NULL, true);
    }
    if (status == 0)
    {
        sc->pass = PASS_EMIT;
        handlers(sc, c->document, 0);
    }
    Dmod_Free(sc);
    return c->arena.failed ? -ENOMEM : status;
}
