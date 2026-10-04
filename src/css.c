#include "private.h"
#include <string.h>

/*
 * CSS: style sheets into rules. Selectors - types, ids, classes,
 * attributes, the combinators, :first-child and friends, :not(), :is(),
 * :where(), ::before / ::after; a pseudo-class of interaction (:hover,
 * :active, :focus, ...) never matches a still page. At-rules: @media
 * (evaluated against the viewport), @supports and @layer (their rules
 * taken), @import, @font-face; the others are skipped.
 *
 * Rules are kept by their last compound's id, first class or type, so an
 * element is matched only against the rules that can match it - a style
 * sheet like Font Awesome's has thousands of them.
 */

#define MAX_PARTS       16u
#define MAX_SELECTORS   64u
#define MAX_DECLS       128u
#define MAX_IMPORT_DEPTH 4u

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (uint8_t)c >= 0x80u || c == '\\';
}

static bool ieq(const char* a, size_t n, const char* b)
{
    if (strlen(b) != n)
        return false;
    for (size_t i = 0; i < n; i++)
    {
        if (lower(a[i]) != b[i])
            return false;
    }
    return true;
}

/* Past whitespace and comments */
static const char* skip(const char* s, const char* end)
{
    for (;;)
    {
        while (s < end && is_space(*s))
            s++;
        if (s + 1 < end && s[0] == '/' && s[1] == '*')
        {
            s += 2;
            while (s + 1 < end && !(s[0] == '*' && s[1] == '/'))
                s++;
            s = (s + 1 < end) ? s + 2 : end;
            continue;
        }
        if (end - s >= 4 && strncmp(s, "<!--", 4) == 0)
        {
            s += 4;
            continue;
        }
        if (end - s >= 3 && strncmp(s, "-->", 3) == 0)
        {
            s += 3;
            continue;
        }
        return s;
    }
}

/* Past a string starting at s (its quote) */
static const char* skip_string(const char* s, const char* end)
{
    char q = *s++;
    while (s < end && *s != q)
    {
        if (*s == '\\' && s + 1 < end)
            s++;
        s++;
    }
    return (s < end) ? s + 1 : end;
}

/* The first of `stops` at the top level (not in a string, a comment, brackets), or end */
static const char* find_top(const char* s, const char* end, const char* stops)
{
    int depth = 0;
    while (s < end)
    {
        char ch = *s;
        if (ch == '"' || ch == '\'')
        {
            s = skip_string(s, end);
            continue;
        }
        if (ch == '/' && s + 1 < end && s[1] == '*')
        {
            s = skip(s, end);
            continue;
        }
        if (ch == '\\' && s + 1 < end)
        {
            s += 2;
            continue;
        }
        if (depth == 0 && strchr(stops, ch) != NULL)
            return s;
        if (ch == '(' || ch == '[' || ch == '{')
            depth++;
        else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0)
            depth--;
        s++;
    }
    return end;
}

uint32_t css_unescape(const char** p, const char* end)
{
    const char* s = *p;
    uint32_t c = 0;
    s++;                                /* '\\' */
    uint32_t digits = 0;
    while (s < end && digits < 6U)
    {
        char d = *s;
        uint32_t v = (d >= '0' && d <= '9') ? (uint32_t)(d - '0') : (d >= 'a' && d <= 'f') ? (uint32_t)(d - 'a' + 10)
                   : (d >= 'A' && d <= 'F') ? (uint32_t)(d - 'A' + 10) : 99U;
        if (v == 99U)
            break;
        c = c * 16U + v;
        s++;
        digits++;
    }
    if (digits > 0)
    {
        if (s < end && is_space(*s))
            s++;
        *p = s;
        return (c == 0 || c > 0x10FFFFu) ? 0xFFFDu : c;
    }
    if (s < end)
    {
        const char* q = s;
        c = dmvsi_utf8_next(&q, end);
        *p = q;
        return c;
    }
    *p = s;
    return 0xFFFDu;
}

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

/* An identifier at *p (escapes decoded), or NULL */
static char* identifier(conv_t* c, const char** p, const char* end, bool lowercase)
{
    const char* s = *p;
    char buffer[256];
    size_t n = 0;
    while (s < end && is_name_char(*s) && n + 4U < sizeof(buffer))
    {
        if (*s == '\\')
        {
            n += put_utf8(buffer + n, css_unescape(&s, end));
            continue;
        }
        buffer[n++] = lowercase ? lower(*s) : *s;
        s++;
    }
    *p = s;
    return (n == 0) ? NULL : arena_strndup(&c->arena, buffer, n);
}

/* ---- Selectors ---- */

static bool parse_compound(conv_t* c, const char** p, const char* end, compound_t* out, uint32_t* spec, uint8_t* pseudo_element);

/* The selectors in :not(), :is(), :where() - compounds only */
static uint8_t parse_compound_list(conv_t* c, const char* s, const char* end, compound_t** out, uint32_t* spec)
{
    compound_t list[8];
    uint8_t n = 0;
    uint32_t best = 0;
    while (s < end && n < 8U)
    {
        s = skip(s, end);
        uint32_t sp = 0;
        uint8_t pe = PSEUDO_NONE;
        memset(&list[n], 0, sizeof(list[n]));
        if (!parse_compound(c, &s, end, &list[n], &sp, &pe))
            return 0;
        best = (sp > best) ? sp : best;
        n++;
        s = skip(s, end);
        if (s < end && *s == ',')
            s++;
        else if (s < end)
            return 0;                   /* A complex selector: not supported here */
    }
    *out = arena_alloc(&c->arena, n * sizeof(compound_t));
    if (*out == NULL)
        return 0;
    memcpy(*out, list, n * sizeof(compound_t));
    *spec = best;
    return n;
}

static bool parse_attribute(conv_t* c, const char** p, const char* end, attr_sel_t* a)
{
    const char* s = skip(*p + 1, end);  /* '[' */
    memset(a, 0, sizeof(*a));
    if ((a->name = identifier(c, &s, end, true)) == NULL)
        return false;
    s = skip(s, end);
    a->op = ATTR_EXISTS;
    if (s < end && *s != ']')
    {
        static const char ops[] = "=~|^$*";
        const char* o = strchr(ops, *s);
        if (o == NULL || *o == '\0')
            return false;
        a->op = (*s == '=') ? ATTR_EQUALS : (*s == '~') ? ATTR_INCLUDES : (*s == '|') ? ATTR_DASH
              : (*s == '^') ? ATTR_PREFIX : (*s == '$') ? ATTR_SUFFIX : ATTR_CONTAINS;
        s += (*s == '=') ? 1 : 2;
        s = skip(s, end);
        if (s < end && (*s == '"' || *s == '\''))
        {
            const char* q = skip_string(s, end);
            a->value = arena_strndup(&c->arena, s + 1, (size_t)(q - s - 2));
            s = q;
        }
        else
            a->value = identifier(c, &s, end, false);
        s = skip(s, end);
        if (s < end && (*s == 'i' || *s == 'I'))
        {
            a->icase = true;
            s = skip(s + 1, end);
        }
    }
    if (s >= end || *s != ']')
        return false;
    *p = s + 1;
    return true;
}

static bool parse_pseudo(conv_t* c, const char** p, const char* end, compound_t* out, uint32_t* spec, uint8_t* pseudo_element)
{
    const char* s = *p + 1;
    bool element = false;
    if (s < end && *s == ':')
    {
        element = true;
        s++;
    }
    const char* name = s;
    while (s < end && is_name_char(*s))
        s++;
    size_t n = (size_t)(s - name);
    const char* args = NULL;
    const char* args_end = NULL;
    if (s < end && *s == '(')
    {
        args = s + 1;
        args_end = find_top(args, end, ")");
        s = (args_end < end) ? args_end + 1 : end;
    }
    *p = s;

    if (ieq(name, n, "before") || ieq(name, n, "after"))
    {
        *pseudo_element = ieq(name, n, "before") ? PSEUDO_BEFORE : PSEUDO_AFTER;
        *spec += 1U;
        return true;
    }
    if (element)
        return false;                   /* ::placeholder, ::selection, ...: never drawn */
    *spec += 1U << 8;
    if (ieq(name, n, "first-child"))
        out->pseudo |= PC_FIRST_CHILD;
    else if (ieq(name, n, "last-child"))
        out->pseudo |= PC_LAST_CHILD;
    else if (ieq(name, n, "only-child"))
        out->pseudo |= PC_ONLY_CHILD;
    else if (ieq(name, n, "root"))
        out->pseudo |= PC_ROOT;
    else if (ieq(name, n, "empty"))
        out->pseudo |= PC_EMPTY;
    else if (args != NULL && (ieq(name, n, "not") || ieq(name, n, "is") || ieq(name, n, "where") || ieq(name, n, "matches")))
    {
        uint32_t inner = 0;
        compound_t* list = NULL;
        uint8_t count = parse_compound_list(c, args, args_end, &list, &inner);
        if (count == 0)
            return false;
        *spec -= 1U << 8;
        if (ieq(name, n, "not"))
        {
            out->nots = list;
            out->not_count = count;
        }
        else
        {
            out->ises = list;
            out->is_count = count;
        }
        if (!ieq(name, n, "where"))
            *spec += inner;
    }
    else
        out->pseudo |= PC_NEVER;        /* :hover, :focus, :host, ... */
    return true;
}

static bool parse_compound(conv_t* c, const char** p, const char* end, compound_t* out, uint32_t* spec, uint8_t* pseudo_element)
{
    const char* s = *p;
    char* classes[16];
    attr_sel_t attrs[8];
    uint8_t nc = 0, na = 0;
    bool any = false;

    if (s < end && *s == '*')
    {
        s++;
        any = true;
    }
    else if (s < end && is_name_char(*s))
    {
        out->tag = identifier(c, &s, end, true);
        *spec += 1U;
        any = true;
    }
    while (s < end)
    {
        if (*s == '#')
        {
            s++;
            out->id = identifier(c, &s, end, false);
            if (out->id == NULL)
                return false;
            *spec += 1U << 16;
        }
        else if (*s == '.')
        {
            s++;
            char* name = identifier(c, &s, end, false);
            if (name == NULL || nc >= 16U)
                return false;
            classes[nc++] = name;
            *spec += 1U << 8;
        }
        else if (*s == '[')
        {
            if (na >= 8U || !parse_attribute(c, &s, end, &attrs[na]))
                return false;
            na++;
            *spec += 1U << 8;
        }
        else if (*s == ':')
        {
            if (!parse_pseudo(c, &s, end, out, spec, pseudo_element))
                return false;
        }
        else
            break;
        any = true;
    }
    if (!any)
        return false;
    if (nc > 0)
    {
        out->classes = arena_alloc(&c->arena, nc * sizeof(char*));
        if (out->classes == NULL)
            return false;
        memcpy(out->classes, classes, nc * sizeof(char*));
        out->class_count = nc;
    }
    if (na > 0)
    {
        out->attrs = arena_alloc(&c->arena, na * sizeof(attr_sel_t));
        if (out->attrs == NULL)
            return false;
        memcpy(out->attrs, attrs, na * sizeof(attr_sel_t));
        out->attr_count = na;
    }
    *p = s;
    return true;
}

/* One selector of a list into `rule` (its parts, specificity, pseudo-element) */
static bool parse_selector(conv_t* c, const char* s, const char* end, rule_t* rule)
{
    compound_t parts[MAX_PARTS];
    uint8_t n = 0;
    uint8_t comb = COMB_NONE;
    uint32_t spec = 0;
    uint8_t pseudo_element = PSEUDO_NONE;

    s = skip(s, end);
    while (s < end)
    {
        if (n >= MAX_PARTS || pseudo_element != PSEUDO_NONE)
            return false;               /* Nothing may follow a pseudo-element */
        memset(&parts[n], 0, sizeof(parts[n]));
        parts[n].comb = comb;
        if (!parse_compound(c, &s, end, &parts[n], &spec, &pseudo_element))
            return false;
        n++;
        const char* before = s;
        s = skip(s, end);
        if (s >= end)
            break;
        if (*s == '>' || *s == '+' || *s == '~')
        {
            comb = (*s == '>') ? COMB_CHILD : (*s == '+') ? COMB_NEXT : COMB_SIBLING;
            s = skip(s + 1, end);
        }
        else if (s > before)
            comb = COMB_DESCENDANT;
        else
            return false;
    }
    if (n == 0)
        return false;
    rule->parts = arena_alloc(&c->arena, n * sizeof(compound_t));
    if (rule->parts == NULL)
        return false;
    memcpy(rule->parts, parts, n * sizeof(compound_t));
    rule->count = n;
    rule->specificity = spec;
    rule->pseudo_element = pseudo_element;
    return true;
}

/* ---- Matching ---- */

static bool attr_matches(const attr_sel_t* a, const node_t* n)
{
    const char* v = node_attr(n, a->name);
    if (v == NULL)
        return false;
    if (a->op == ATTR_EXISTS)
        return true;
    const char* want = (a->value != NULL) ? a->value : "";
    size_t vl = strlen(v), wl = strlen(want);
    switch (a->op)
    {
        case ATTR_EQUALS:
            if (a->icase)
            {
                if (vl != wl)
                    return false;
                for (size_t i = 0; i < vl; i++)
                {
                    if (lower(v[i]) != lower(want[i]))
                        return false;
                }
                return true;
            }
            return strcmp(v, want) == 0;
        case ATTR_PREFIX:
            return wl > 0 && strncmp(v, want, wl) == 0;
        case ATTR_SUFFIX:
            return wl > 0 && vl >= wl && strcmp(v + vl - wl, want) == 0;
        case ATTR_DASH:
            return strcmp(v, want) == 0 || (strncmp(v, want, wl) == 0 && v[wl] == '-');
        case ATTR_CONTAINS:
        case ATTR_INCLUDES:
            for (size_t i = 0; wl > 0 && i + wl <= vl; i++)
            {
                if (strncmp(v + i, want, wl) != 0)
                    continue;
                if (a->op == ATTR_CONTAINS)
                    return true;
                if ((i == 0 || is_space(v[i - 1])) && (v[i + wl] == '\0' || is_space(v[i + wl])))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static const node_t* prev_element(const node_t* n)
{
    for (n = n->prev; n != NULL; n = n->prev)
    {
        if (n->kind == NODE_ELEMENT && n->pseudo == PSEUDO_NONE)
            return n;
    }
    return NULL;
}

static const node_t* next_element(const node_t* n)
{
    for (n = n->next; n != NULL; n = n->next)
    {
        if (n->kind == NODE_ELEMENT && n->pseudo == PSEUDO_NONE)
            return n;
    }
    return NULL;
}

static bool compound_matches(const compound_t* k, const node_t* n)
{
    if (n == NULL || n->kind != NODE_ELEMENT || n->pseudo != PSEUDO_NONE)
        return false;
    if ((k->pseudo & PC_NEVER) != 0)
        return false;
    if (k->tag != NULL && strcmp(k->tag, n->tag) != 0)
        return false;
    if (k->id != NULL && (n->id == NULL || strcmp(k->id, n->id) != 0))
        return false;
    for (uint8_t i = 0; i < k->class_count; i++)
    {
        bool found = false;
        for (uint32_t j = 0; j < n->class_count && !found; j++)
            found = strcmp(n->classes[j], k->classes[i]) == 0;
        if (!found)
            return false;
    }
    for (uint8_t i = 0; i < k->attr_count; i++)
    {
        if (!attr_matches(&k->attrs[i], n))
            return false;
    }
    if ((k->pseudo & (PC_FIRST_CHILD | PC_ONLY_CHILD)) != 0 && prev_element(n) != NULL)
        return false;
    if ((k->pseudo & (PC_LAST_CHILD | PC_ONLY_CHILD)) != 0 && next_element(n) != NULL)
        return false;
    if ((k->pseudo & PC_ROOT) != 0 && (n->parent == NULL || n->parent->kind != NODE_DOCUMENT))
        return false;
    if ((k->pseudo & PC_EMPTY) != 0 && n->first != NULL)
        return false;
    for (uint8_t i = 0; i < k->not_count; i++)
    {
        if (compound_matches(&k->nots[i], n))
            return false;
    }
    if (k->is_count > 0)
    {
        bool any = false;
        for (uint8_t i = 0; i < k->is_count && !any; i++)
            any = compound_matches(&k->ises[i], n);
        if (!any)
            return false;
    }
    return true;
}

/* Parts 0 ... i, with part i on n */
static bool matches_from(const rule_t* r, int i, const node_t* n)
{
    if (!compound_matches(&r->parts[i], n))
        return false;
    if (i == 0)
        return true;
    switch (r->parts[i].comb)
    {
        case COMB_CHILD:
            return matches_from(r, i - 1, n->parent);
        case COMB_DESCENDANT:
            for (const node_t* a = n->parent; a != NULL && a->kind == NODE_ELEMENT; a = a->parent)
            {
                if (matches_from(r, i - 1, a))
                    return true;
            }
            return false;
        case COMB_NEXT:
            return matches_from(r, i - 1, prev_element(n));
        case COMB_SIBLING:
            for (const node_t* s = prev_element(n); s != NULL; s = prev_element(s))
            {
                if (matches_from(r, i - 1, s))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

bool css_matches(const rule_t* rule, const node_t* n)
{
    return rule->count > 0 && matches_from(rule, (int)rule->count - 1, n);
}

/* ---- Rules ---- */

uint16_t css_parse_decls(conv_t* c, const char* text, size_t length, decl_t** decls);

static uint32_t hash(const char* prefix, const char* s)
{
    uint32_t h = 2166136261u;
    for (; *prefix != '\0'; prefix++)
        h = (h ^ (uint8_t)*prefix) * 16777619u;
    for (; *s != '\0'; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h % RULE_BUCKETS;
}

void css_add_rule(conv_t* c, rule_t* rule)
{
    rule_ref_t* ref = arena_alloc(&c->arena, sizeof(*ref));
    if (ref == NULL)
        return;
    ref->rule = rule;
    const compound_t* last = &rule->parts[rule->count - 1U];
    rule_ref_t** list = &c->universal;
    if (last->id != NULL)
        list = &c->buckets[hash("#", last->id)];
    else if (last->class_count > 0)
        list = &c->buckets[hash(".", last->classes[0])];
    else if (last->tag != NULL)
        list = &c->buckets[hash("", last->tag)];
    ref->next = *list;
    *list = ref;
    c->rule_count++;
}

const rule_ref_t* css_bucket(const conv_t* c, const char* prefix, const char* name)
{
    return c->buckets[hash(prefix, name)];
}

/* "name: value; ..." -> declarations (into the arena); returns how many */
static uint16_t parse_decls(conv_t* c, const char* s, const char* end, decl_t** out)
{
    decl_t decls[MAX_DECLS];
    uint16_t n = 0;
    while (s < end && n < MAX_DECLS)
    {
        s = skip(s, end);
        const char* stop = find_top(s, end, ";");
        const char* colon = find_top(s, stop, ":");
        if (colon < stop)
        {
            const char* name_end = colon;
            while (name_end > s && is_space(name_end[-1]))
                name_end--;
            const char* v = skip(colon + 1, stop);
            const char* v_end = stop;
            while (v_end > v && is_space(v_end[-1]))
                v_end--;
            bool important = false;
            /* "... !important" */
            const char* bang = v_end;
            while (bang > v && bang[-1] != '!')
                bang--;
            if (bang > v)
            {
                const char* w = skip(bang, v_end);
                if (ieq(w, (size_t)(v_end - w), "important"))
                {
                    important = true;
                    v_end = bang - 1;
                    while (v_end > v && is_space(v_end[-1]))
                        v_end--;
                }
            }
            bool custom = name_end - s > 2 && s[0] == '-' && s[1] == '-';
            char* name = arena_strndup(&c->arena, s, (size_t)(name_end - s));
            if (name != NULL && !custom)
            {
                for (char* t = name; *t != '\0'; t++)
                    *t = lower(*t);
            }
            decls[n].name = name;
            decls[n].value = arena_strndup(&c->arena, v, (size_t)(v_end - v));
            decls[n].important = important;
            if (name != NULL && decls[n].value != NULL && name[0] != '\0')
                n++;
        }
        s = (stop < end) ? stop + 1 : end;
    }
    *out = (n > 0) ? arena_alloc(&c->arena, n * sizeof(decl_t)) : NULL;
    if (*out != NULL)
        memcpy(*out, decls, n * sizeof(decl_t));
    return (*out != NULL) ? n : 0;
}

/* ---- @font-face ---- */

static uint16_t parse_weight(const char* v, const char** next)
{
    const char* s = v;
    while (is_space(*s))
        s++;
    uint32_t w = 0;
    if (strncmp(s, "bold", 4) == 0)
    {
        w = 700;
        s += 4;
    }
    else if (strncmp(s, "normal", 6) == 0)
    {
        w = 400;
        s += 6;
    }
    else
    {
        for (; *s >= '0' && *s <= '9'; s++)
            w = w * 10U + (uint32_t)(*s - '0');
    }
    if (next != NULL)
        *next = s;
    return (w >= 1U && w <= 1000U) ? (uint16_t)w : 400U;
}

static bool font_file(const char* url, size_t n)
{
    /* The extension, before a query or a fragment */
    for (size_t k = 0; k + 4U <= n; k++)
    {
        bool end = k + 4U == n || url[k + 4] == '?' || url[k + 4] == '#';
        if (end && (ieq(url + k, 4, ".ttf") || ieq(url + k, 4, ".otf") || ieq(url + k, 4, ".ttc")))
            return true;
    }
    return false;
}

/* The first url() of src that is a TrueType / OpenType font */
static char* pick_src(conv_t* c, const char* v, const char* base)
{
    const char* end = v + strlen(v);
    const char* s = v;
    while (s < end)
    {
        const char* item_end = find_top(s, end, ",");
        const char* u = s;
        const char* url = NULL;
        size_t url_length = 0;
        bool truetype = false;
        while (u < item_end)
        {
            u = skip(u, item_end);
            if (item_end - u > 4 && ieq(u, 4, "url("))
            {
                const char* a = skip(u + 4, item_end);
                const char* close = find_top(a, item_end, ")");
                const char* b = close;
                while (b > a && is_space(b[-1]))
                    b--;
                if (b > a && (*a == '"' || *a == '\''))
                {
                    a++;
                    b--;
                }
                url = a;
                url_length = (size_t)(b - a);
                u = (close < item_end) ? close + 1 : item_end;
            }
            else if (item_end - u > 7 && ieq(u, 7, "format("))
            {
                const char* close = find_top(u, item_end, ")");
                for (const char* f = u; f + 8 <= close; f++)
                {
                    if (ieq(f, 8, "truetype") || ieq(f, 8, "opentype"))
                        truetype = true;
                }
                u = (close < item_end) ? close + 1 : item_end;
            }
            else
                u++;
        }
        if (url != NULL && (truetype || font_file(url, url_length)))
            return resolve_resource(c, base, url, url_length);
        s = (item_end < end) ? item_end + 1 : end;
    }
    return NULL;
}

static void font_face(conv_t* c, const char* s, const char* end, const char* base)
{
    decl_t* decls = NULL;
    uint16_t n = parse_decls(c, s, end, &decls);
    font_face_t* f = arena_alloc(&c->arena, sizeof(*f));
    if (f == NULL)
        return;
    f->weight_min = 400;
    f->weight_max = 400;
    for (uint16_t i = 0; i < n; i++)
    {
        const char* v = decls[i].value;
        if (strcmp(decls[i].name, "font-family") == 0)
        {
            size_t length = strlen(v);
            if (length >= 2U && (v[0] == '"' || v[0] == '\''))
            {
                v++;
                length -= 2U;
            }
            f->family = arena_strndup(&c->arena, v, length);
            for (char* t = f->family; t != NULL && *t != '\0'; t++)
                *t = lower(*t);
        }
        else if (strcmp(decls[i].name, "font-weight") == 0)
        {
            const char* rest = NULL;
            f->weight_min = parse_weight(v, &rest);
            f->weight_max = (rest != NULL && *skip(rest, rest + strlen(rest)) != '\0') ? parse_weight(rest, NULL) : f->weight_min;
        }
        else if (strcmp(decls[i].name, "font-style") == 0)
            f->italic = strncmp(v, "italic", 6) == 0 || strncmp(v, "oblique", 7) == 0;
        else if (strcmp(decls[i].name, "src") == 0)
            f->path = pick_src(c, v, base);
    }
    if (f->family == NULL || f->path == NULL)
        return;                         /* Nothing it can be drawn with (woff2 only, a remote font) */
    f->next = c->faces;
    c->faces = f;
}

/* ---- @media ---- */

static int32_t media_px(const char* s, const char* end)
{
    int32_t v = 0, frac = 0, scale = 1;
    s = skip(s, end);
    for (; s < end && *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
    if (s < end && *s == '.')
    {
        for (s++; s < end && *s >= '0' && *s <= '9' && scale < 1000; s++, scale *= 10)
            frac = frac * 10 + (*s - '0');
    }
    int32_t px = v * U + frac * U / scale;
    if (end - s >= 2 && (ieq(s, 2, "em") || (end - s >= 3 && ieq(s, 3, "rem"))))
        px *= 16;
    return px;
}

static bool media_feature(conv_t* c, const char* s, const char* end)
{
    const char* colon = find_top(s, end, ":");
    const char* name = skip(s, end);
    const char* name_end = colon;
    while (name_end > name && is_space(name_end[-1]))
        name_end--;
    size_t n = (size_t)(name_end - name);
    const char* v = (colon < end) ? skip(colon + 1, end) : end;
    if (ieq(name, n, "min-width"))
        return c->vw >= media_px(v, end);
    if (ieq(name, n, "max-width"))
        return c->vw <= media_px(v, end);
    if (ieq(name, n, "min-height"))
        return c->vh >= media_px(v, end);
    if (ieq(name, n, "max-height"))
        return c->vh <= media_px(v, end);
    if (ieq(name, n, "orientation"))
        return ieq(v, 8, "portrait") ? c->vh >= c->vw : c->vw > c->vh;
    if (ieq(name, n, "prefers-color-scheme"))
        return ieq(v, 5, "light");
    if (ieq(name, n, "prefers-reduced-motion"))
        return ieq(v, 13, "no-preference");
    if (ieq(name, n, "hover") || ieq(name, n, "any-hover"))
        return ieq(v, 4, "none");
    if (ieq(name, n, "pointer") || ieq(name, n, "any-pointer"))
        return ieq(v, 6, "coarse");
    return false;
}

/* A media query list: any of its queries */
static bool media_matches(conv_t* c, const char* s, const char* end)
{
    s = skip(s, end);
    if (s >= end)
        return true;
    while (s < end)
    {
        const char* q_end = find_top(s, end, ",");
        bool ok = true, negate = false;
        const char* t = s;
        while (t < q_end)
        {
            t = skip(t, q_end);
            if (t >= q_end)
                break;
            if (*t == '(')
            {
                const char* close = find_top(t + 1, q_end, ")");
                ok = ok && media_feature(c, t + 1, close);
                t = (close < q_end) ? close + 1 : q_end;
                continue;
            }
            const char* w = t;
            while (t < q_end && is_name_char(*t))
                t++;
            size_t n = (size_t)(t - w);
            if (n == 0)
            {
                t++;
                continue;
            }
            if (ieq(w, n, "not"))
                negate = true;
            else if (ieq(w, n, "only") || ieq(w, n, "and") || ieq(w, n, "all") || ieq(w, n, "screen"))
                continue;
            else
                ok = false;             /* print, speech, ... */
        }
        if (ok != negate)
            return true;
        s = (q_end < end) ? q_end + 1 : end;
    }
    return false;
}

/* ---- Style sheets ---- */

static void parse_sheet(conv_t* c, const char* s, const char* end, const char* base, uint32_t layer, uint32_t depth);

/* @import url(...) [media] */
static void import(conv_t* c, const char* s, const char* end, const char* base, uint32_t layer, uint32_t depth)
{
    s = skip(s, end);
    const char* url = NULL;
    size_t n = 0;
    if (end - s > 4 && ieq(s, 4, "url("))
    {
        const char* a = skip(s + 4, end);
        const char* close = find_top(a, end, ")");
        const char* b = close;
        while (b > a && is_space(b[-1]))
            b--;
        if (b > a && (*a == '"' || *a == '\''))
        {
            a++;
            b--;
        }
        url = a;
        n = (size_t)(b - a);
        s = (close < end) ? close + 1 : end;
    }
    else if (s < end && (*s == '"' || *s == '\''))
    {
        const char* q = skip_string(s, end);
        url = s + 1;
        n = (size_t)(q - s - 2);
        s = q;
    }
    if (url == NULL || depth >= MAX_IMPORT_DEPTH || !media_matches(c, s, end))
        return;
    char* absolute = resource_url(c, base, url, n);
    char* path = (absolute != NULL) ? resource_path(c, absolute) : NULL;
    size_t size = 0;
    char* text = (path != NULL) ? read_resource(c, path, &size) : NULL;
    if (text == NULL)
    {
        WARN(c, "cannot load the style sheet %s\n", (absolute != NULL) ? absolute : "?");
        return;
    }
    parse_sheet(c, text, text + size, absolute, layer, depth + 1U);
}

/* A rule: its selectors, each a rule of the same declarations */
static void style_rule(conv_t* c, const char* prelude, const char* prelude_end, const char* body, const char* body_end,
                       const char* base, uint32_t layer)
{
    decl_t* decls = NULL;
    uint16_t n = parse_decls(c, body, body_end, &decls);
    if (n == 0)
        return;
    uint32_t order = ++c->order;
    const char* s = prelude;
    while (s < prelude_end)
    {
        const char* sel_end = find_top(s, prelude_end, ",");
        rule_t* r = arena_alloc(&c->arena, sizeof(*r));
        if (r == NULL)
            return;
        if (parse_selector(c, s, sel_end, r))
        {
            r->decls = decls;
            r->decl_count = n;
            r->order = order;
            r->layer = (uint8_t)layer;
            r->base = base;
            css_add_rule(c, r);
        }
        s = (sel_end < prelude_end) ? sel_end + 1 : prelude_end;
    }
}

static void parse_sheet(conv_t* c, const char* s, const char* end, const char* base, uint32_t layer, uint32_t depth)
{
    while (s < end && !c->arena.failed)
    {
        s = skip(s, end);
        if (s >= end)
            break;
        if (*s == '}')
        {
            s++;                        /* A stray one */
            continue;
        }
        if (*s == '@')
        {
            const char* name = ++s;
            while (s < end && is_name_char(*s))
                s++;
            size_t n = (size_t)(s - name);
            const char* stop = find_top(s, end, ";{");
            if (stop >= end || *stop == ';')
            {
                if (ieq(name, n, "import"))
                    import(c, s, stop, base, layer, depth);
                s = (stop < end) ? stop + 1 : end;
                continue;
            }
            const char* body = stop + 1;
            const char* close = find_top(body, end, "}");
            if (ieq(name, n, "media"))
            {
                if (media_matches(c, s, stop))
                    parse_sheet(c, body, close, base, layer, depth);
            }
            else if (ieq(name, n, "supports") || ieq(name, n, "layer") || ieq(name, n, "container"))
                parse_sheet(c, body, close, base, layer, depth);
            else if (ieq(name, n, "font-face"))
                font_face(c, body, close, base);
            s = (close < end) ? close + 1 : end;
            continue;
        }
        const char* open = find_top(s, end, "{");
        if (open >= end)
            break;
        const char* close = find_top(open + 1, end, "}");
        style_rule(c, s, open, open + 1, close, base, layer);
        s = (close < end) ? close + 1 : end;
    }
}

uint16_t css_parse_decls(conv_t* c, const char* text, size_t length, decl_t** decls)
{
    return parse_decls(c, text, text + length, decls);
}

void css_parse(conv_t* c, const char* text, size_t length, const char* base, uint32_t layer)
{
    parse_sheet(c, text, text + length, base, layer, 0);
}
