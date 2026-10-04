#include "private.h"
#include <string.h>

/*
 * HTML: tags, attributes, text and comments into a tree. What a still page
 * needs of the HTML parsing rules: void elements (img, br, meta, ...), raw
 * text (script, style, title, textarea), the end tags a page may leave out
 * (a <p> before a block, <li>, <option>, table cells), character
 * references. Names are lowercase; attribute values and text decoded.
 */

#define MAX_DEPTH       256u

/*
 * The tables are text, not arrays of pointers: a module's data is not
 * relocated when it is loaded (only its GOT is), so a pointer in it would
 * point where the module was linked, not where it is.
 */

/* Lists of names: "|a|b|c|" */
static const char g_void[] = "|area|base|br|col|embed|hr|img|input|link|meta|source|track|wbr|";
static const char g_raw[] = "|script|style|title|textarea|";

/* Elements a <p> ends before */
static const char g_closes_p[] =
    "|address|article|aside|blockquote|div|dl|fieldset|footer|form|h1|h2|h3|h4|h5|h6|header|hr|main|nav|ol|p|pre|"
    "section|table|ul|figure|";

static const struct
{
    char        name[8];
    uint32_t    codepoint;
} g_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "middot", 0xB7 }, { "bull", 0x2022 },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "euro", 0x20AC }, { "trade", 0x2122 }, { "larr", 0x2190 },
    { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "sup2", 0xB2 }, { "frac12", 0xBD }, { "micro", 0xB5 },
    { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwj", 0x200D }, { "zwnj", 0x200C },
};

static bool in_list(const char* list, const char* name)
{
    size_t n = strlen(name);
    for (const char* s = list; *s != '\0'; s++)
    {
        if (*s == '|' && strncmp(s + 1, name, n) == 0 && s[1 + n] == '|')
            return true;
    }
    return false;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static size_t put_utf8(char* out, uint32_t c)
{
    if (c < 0x80u)
    {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800u)
    {
        out[0] = (char)(0xC0u | (c >> 6));
        out[1] = (char)(0x80u | (c & 0x3Fu));
        return 2;
    }
    if (c < 0x10000u)
    {
        out[0] = (char)(0xE0u | (c >> 12));
        out[1] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (c & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (c >> 18));
    out[1] = (char)(0x80u | ((c >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((c >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (c & 0x3Fu));
    return 4;
}

/* `s` with its character references decoded (never longer) */
static char* decode(conv_t* c, const char* s, size_t length, size_t* out_length)
{
    char* out = arena_alloc(&c->arena, length + 1U);
    size_t n = 0;
    if (out == NULL)
        return NULL;
    for (size_t i = 0; i < length; )
    {
        if (s[i] != '&')
        {
            out[n++] = s[i++];
            continue;
        }
        size_t j = i + 1U;
        uint32_t cp = 0;
        bool ok = false;
        if (j < length && s[j] == '#')
        {
            bool hex = j + 1U < length && (s[j + 1] == 'x' || s[j + 1] == 'X');
            j += hex ? 2U : 1U;
            size_t digits = 0;
            for (; j < length && digits < 8U; j++, digits++)
            {
                char d = s[j];
                uint32_t v = (d >= '0' && d <= '9') ? (uint32_t)(d - '0') : (hex && d >= 'a' && d <= 'f') ? (uint32_t)(d - 'a' + 10)
                           : (hex && d >= 'A' && d <= 'F') ? (uint32_t)(d - 'A' + 10) : 99U;
                if (v == 99U)
                    break;
                cp = cp * (hex ? 16U : 10U) + v;
            }
            ok = digits > 0 && cp > 0 && cp <= 0x10FFFFu;
        }
        else
        {
            size_t k = j;
            while (k < length && k - j < 10U && (is_alpha(s[k]) || (s[k] >= '0' && s[k] <= '9')))
                k++;
            for (size_t e = 0; e < sizeof(g_entities) / sizeof(g_entities[0]) && !ok; e++)
            {
                if (strlen(g_entities[e].name) == k - j && strncmp(g_entities[e].name, s + j, k - j) == 0)
                {
                    cp = g_entities[e].codepoint;
                    ok = true;
                }
            }
            j = k;
        }
        if (!ok)
        {
            out[n++] = s[i++];
            continue;
        }
        if (j < length && s[j] == ';')
            j++;
        n += put_utf8(out + n, cp);
        i = j;
    }
    out[n] = '\0';
    *out_length = n;
    return out;
}

/* ---- The tree ---- */

typedef struct
{
    conv_t*     c;
    node_t*     open[MAX_DEPTH];
    uint32_t    depth;
} parser_t;

static node_t* current(parser_t* p)
{
    return p->open[p->depth - 1U];
}

static void append(node_t* parent, node_t* n)
{
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last != NULL)
        parent->last->next = n;
    else
        parent->first = n;
    parent->last = n;
}

static void add_text(parser_t* p, const char* s, size_t length)
{
    if (length == 0)
        return;
    node_t* n = arena_alloc(&p->c->arena, sizeof(*n));
    if (n == NULL)
        return;
    n->kind = NODE_TEXT;
    n->text = decode(p->c, s, length, &n->length);
    append(current(p), n);
}

/* Close up to and with the innermost open `tag`, if one is open (not past `stop`, when given) */
static bool close_tag(parser_t* p, const char* tag, const char* stop)
{
    for (uint32_t i = p->depth; i > 1U; i--)
    {
        node_t* n = p->open[i - 1U];
        if (strcmp(n->tag, tag) == 0)
        {
            p->depth = i - 1U;
            return true;
        }
        if (stop != NULL && in_list(stop, n->tag))
            return false;
    }
    return false;
}

/* The end tags a page may leave out, before a start tag */
static void close_implied(parser_t* p, const char* tag)
{
    static const char list_stop[] = "|ul|ol|";
    static const char table_stop[] = "|table|";
    node_t* cur = current(p);
    if (cur->tag != NULL && strcmp(cur->tag, "p") == 0 && in_list(g_closes_p, tag))
        p->depth--;
    else if (strcmp(tag, "li") == 0)
        (void)close_tag(p, "li", list_stop);
    else if (strcmp(tag, "option") == 0 && cur->tag != NULL && strcmp(cur->tag, "option") == 0)
        p->depth--;
    else if (strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0)
    {
        if (cur->tag != NULL && (strcmp(cur->tag, "dt") == 0 || strcmp(cur->tag, "dd") == 0))
            p->depth--;
    }
    else if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0)
    {
        if (!close_tag(p, "td", table_stop))
            (void)close_tag(p, "th", table_stop);
    }
    else if (strcmp(tag, "tr") == 0)
        (void)close_tag(p, "tr", table_stop);
}

/* class="a b c" -> its classes */
static void split_classes(conv_t* c, node_t* n, const char* value)
{
    uint32_t count = 0;
    for (const char* s = value; *s != '\0'; )
    {
        while (is_space(*s))
            s++;
        if (*s == '\0')
            break;
        count++;
        while (*s != '\0' && !is_space(*s))
            s++;
    }
    if (count == 0 || (n->classes = arena_alloc(&c->arena, count * sizeof(char*))) == NULL)
        return;
    for (const char* s = value; *s != '\0'; )
    {
        while (is_space(*s))
            s++;
        const char* start = s;
        while (*s != '\0' && !is_space(*s))
            s++;
        if (s > start)
            n->classes[n->class_count++] = arena_strndup(&c->arena, start, (size_t)(s - start));
    }
}

/* A start tag at `s` (after '<'); returns where it ends */
static const char* start_tag(parser_t* p, const char* s, const char* end)
{
    conv_t* c = p->c;
    const char* name = s;
    while (s < end && !is_space(*s) && *s != '>' && *s != '/')
        s++;
    node_t* n = arena_alloc(&c->arena, sizeof(*n));
    if (n == NULL)
        return end;
    n->kind = NODE_ELEMENT;
    n->tag = arena_strndup(&c->arena, name, (size_t)(s - name));
    if (n->tag == NULL)
        return end;
    for (char* t = n->tag; *t != '\0'; t++)
        *t = lower(*t);

    attr_t* last = NULL;
    bool self_closing = false;
    while (s < end)
    {
        while (s < end && is_space(*s))
            s++;
        if (s >= end || *s == '>')
            break;
        if (*s == '/')
        {
            self_closing = true;
            s++;
            continue;
        }
        const char* an = s;
        while (s < end && !is_space(*s) && *s != '=' && *s != '>' && *s != '/')
            s++;
        attr_t* a = arena_alloc(&c->arena, sizeof(*a));
        if (a == NULL)
            return end;
        a->name = arena_strndup(&c->arena, an, (size_t)(s - an));
        for (char* t = a->name; t != NULL && *t != '\0'; t++)
            *t = lower(*t);
        while (s < end && is_space(*s))
            s++;
        const char* value = "";
        size_t value_length = 0;
        if (s < end && *s == '=')
        {
            s++;
            while (s < end && is_space(*s))
                s++;
            if (s < end && (*s == '"' || *s == '\''))
            {
                char q = *s++;
                value = s;
                while (s < end && *s != q)
                    s++;
                value_length = (size_t)(s - value);
                if (s < end)
                    s++;
            }
            else
            {
                value = s;
                while (s < end && !is_space(*s) && *s != '>')
                    s++;
                value_length = (size_t)(s - value);
            }
        }
        size_t decoded;
        a->value = decode(c, value, value_length, &decoded);
        if (last == NULL)
            n->attrs = a;
        else
            last->next = a;
        last = a;
        if (a->name != NULL && a->value != NULL)
        {
            if (strcmp(a->name, "id") == 0)
                n->id = a->value;
            else if (strcmp(a->name, "class") == 0)
                split_classes(c, n, a->value);
        }
    }
    if (s < end)
        s++;                            /* '>' */

    close_implied(p, n->tag);
    append(current(p), n);
    if (in_list(g_void, n->tag) || self_closing)
        return s;
    if (in_list(g_raw, n->tag))
    {
        /* Raw text up to its end tag */
        size_t tag_length = strlen(n->tag);
        const char* text = s;
        while (s < end)
        {
            if (s[0] == '<' && s[1] == '/' && (size_t)(end - s) > tag_length + 2U)
            {
                bool same = true;
                for (size_t k = 0; k < tag_length && same; k++)
                    same = lower(s[2 + k]) == n->tag[k];
                if (same)
                    break;
            }
            s++;
        }
        node_t* t = arena_alloc(&c->arena, sizeof(*t));
        if (t != NULL)
        {
            t->kind = NODE_TEXT;
            if (strcmp(n->tag, "textarea") == 0 || strcmp(n->tag, "title") == 0)
                t->text = decode(c, text, (size_t)(s - text), &t->length);
            else
            {
                t->text = arena_strndup(&c->arena, text, (size_t)(s - text));
                t->length = (size_t)(s - text);
            }
            append(n, t);
        }
        while (s < end && *s != '>')
            s++;
        return (s < end) ? s + 1 : s;
    }
    if (p->depth < MAX_DEPTH)
        p->open[p->depth++] = n;
    return s;
}

node_t* html_parse(conv_t* c, const char* text, size_t length)
{
    parser_t* p = arena_alloc(&c->arena, sizeof(*p));
    node_t* doc = arena_alloc(&c->arena, sizeof(*doc));
    if (p == NULL || doc == NULL)
        return NULL;
    doc->kind = NODE_DOCUMENT;
    p->c = c;
    p->open[0] = doc;
    p->depth = 1;

    const char* s = text;
    const char* end = text + length;
    if (length >= 3 && (uint8_t)s[0] == 0xEFu && (uint8_t)s[1] == 0xBBu && (uint8_t)s[2] == 0xBFu)
        s += 3;                         /* UTF-8 BOM */
    while (s < end)
    {
        const char* lt = s;
        while (lt < end && *lt != '<')
            lt++;
        add_text(p, s, (size_t)(lt - s));
        if (lt >= end)
            break;
        s = lt + 1;
        if (end - s >= 3 && s[0] == '!' && s[1] == '-' && s[2] == '-')
        {
            const char* close = s + 3;
            while (close + 2 < end && !(close[0] == '-' && close[1] == '-' && close[2] == '>'))
                close++;
            s = (close + 2 < end) ? close + 3 : end;
        }
        else if (s < end && (*s == '!' || *s == '?'))
        {
            while (s < end && *s != '>')
                s++;
            s = (s < end) ? s + 1 : s;
        }
        else if (s < end && *s == '/')
        {
            const char* name = ++s;
            while (s < end && !is_space(*s) && *s != '>')
                s++;
            char tag[32];
            size_t n = (size_t)(s - name);
            if (n < sizeof(tag))
            {
                for (size_t k = 0; k < n; k++)
                    tag[k] = lower(name[k]);
                tag[n] = '\0';
                (void)close_tag(p, tag, NULL);
            }
            while (s < end && *s != '>')
                s++;
            s = (s < end) ? s + 1 : s;
        }
        else if (s < end && is_alpha(*s))
            s = start_tag(p, s, end);
        else
            add_text(p, lt, 1);         /* A '<' that starts no tag */
    }
    return c->arena.failed ? NULL : doc;
}

const char* node_attr(const node_t* n, const char* name)
{
    for (const attr_t* a = n->attrs; a != NULL; a = a->next)
    {
        if (a->name != NULL && strcmp(a->name, name) == 0)
            return a->value;
    }
    return NULL;
}

bool node_is(const node_t* n, const char* tag)
{
    return n != NULL && n->kind == NODE_ELEMENT && strcmp(n->tag, tag) == 0;
}
