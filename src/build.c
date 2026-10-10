#include "private.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/*
 * What the scripts build: the elements they make as the page loads
 * (createElement, className, innerHTML, appendChild - a list of a
 * playlist's songs) are what the page is when it is shown. dmvs_js
 * evaluates the loading as a browser runs it (every variable known,
 * functions run at each call, listeners and timers not); what it does to
 * the DOM is recorded here - operations on the elements of the page as it
 * is parsed (dom_t), applied each time it is laid out (a state of it too).
 *
 * An element made is marked data-dmvs-built="k" (the k-th
 * document.createElement()): the scripts' compilation finds it there.
 */

#define MAX_MADE            64u
#define MAX_CHILDREN        32u
#define MAX_PARENTS         16u
#define MARKUP_SIZE         16384u

#define B_DOCUMENT          1u
#define B_IGNORED           2u              /* What building does not care about (style, tailwind, ...) */
#define B_EXISTING          0x100000u       /* + an element's index */
#define B_MADE              0x200000u       /* + k (1 ...) */
#define B_MADE_CLASSES      0x300000u       /* + k: its classList */

typedef struct
{
    char*       tag;
    char*       classes;                    /* Space-separated */
    char*       id;
    char*       text;                       /* textContent (escaped when written) */
    char*       html;                       /* innerHTML (as it is) */
    char*       attrs;                      /* ' name="value"' ... */
    uint32_t    children[MAX_CHILDREN];     /* Made elements appended to it (k) */
    uint32_t    child_count;
    bool        appended;
} made_t;

/* An element of the page the scripts change: emptied, and what is appended to it */
typedef struct
{
    uint32_t    index;
    bool        clear;
    char*       html;                       /* innerHTML set to markup */
    uint32_t    children[MAX_CHILDREN];
    uint32_t    child_count;
} parent_t;

typedef struct
{
    conv_t*             c;
    made_t              made[MAX_MADE];
    uint32_t            made_count;
    parent_t            parents[MAX_PARENTS];
    uint32_t            parent_count;
} builder_t;

static dmvs_js_value_t object(uint32_t h)
{
    dmvs_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = (h != 0) ? DMVS_JS_V_OBJECT : DMVS_JS_V_NULL;
    v.object = h;
    return v;
}

static dmvs_js_value_t string(const char* s)
{
    dmvs_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = DMVS_JS_V_STRING;
    v.text = s;
    v.length = strlen(s);
    return v;
}

static node_t* by_index(node_t* n, uint32_t index, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->index == index)
            return k;
        node_t* found = by_index(k, index, depth + 1U);
        if (found != NULL)
            return found;
    }
    return NULL;
}

static node_t* existing(builder_t* b, uint32_t h)
{
    return (h > B_EXISTING && h < B_MADE) ? by_index(b->c->document, h - B_EXISTING, 0) : NULL;
}

static made_t* made_of(builder_t* b, uint32_t h)
{
    return (h > B_MADE && h <= B_MADE + b->made_count) ? &b->made[h - B_MADE - 1U] : NULL;
}

static dmvs_js_value_t element(node_t* e)
{
    return object((e != NULL) ? B_EXISTING + e->index : 0);
}

static char* copy(builder_t* b, const char* s)
{
    return arena_strndup(&b->c->arena, s, strlen(s));
}

static const char* text_of(const dmvs_js_value_t* v, char* buffer, size_t size)
{
    if (v->kind == DMVS_JS_V_STRING)
        return v->text;
    if (v->kind == DMVS_JS_V_NUMBER)
    {
        Dmod_SnPrintf(buffer, size, "%d", (int)v->number);
        return buffer;
    }
    return NULL;
}

static parent_t* parent_of(builder_t* b, node_t* e)
{
    for (uint32_t i = 0; i < b->parent_count; i++)
    {
        if (b->parents[i].index == e->index)
            return &b->parents[i];
    }
    if (b->parent_count >= MAX_PARENTS)
        return NULL;
    parent_t* p = &b->parents[b->parent_count++];
    memset(p, 0, sizeof(*p));
    p->index = e->index;
    return p;
}

/* The text an element shows: its text nodes' */
static size_t content(const node_t* n, char* out, size_t size, size_t at, uint32_t depth)
{
    for (const node_t* k = n->first; k != NULL && depth < 64U; k = k->next)
    {
        if (k->kind == NODE_TEXT)
            for (size_t i = 0; i < k->length && at + 1U < size; i++)
                out[at++] = k->text[i];
        else if (k->kind == NODE_ELEMENT)
            at = content(k, out, size, at, depth + 1U);
    }
    out[(at < size) ? at : size - 1U] = '\0';
    return at;
}

/* ---- The host: what building cares about ---- */

static bool b_global(void* ctx, dmvs_js_compiler_t c, const char* name, dmvs_js_value_t* value)
{
    (void)ctx;
    (void)c;
    if (strcmp(name, "document") == 0)
        *value = object(B_DOCUMENT);
    else if (strcmp(name, "tailwind") == 0 || strcmp(name, "window") == 0)
        *value = object(B_IGNORED);
    else
        return false;
    return true;
}

static int b_get(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* obj, const char* name, dmvs_js_value_t* value)
{
    builder_t* b = ctx;
    (void)c;
    uint32_t h = (obj->kind == DMVS_JS_V_OBJECT) ? obj->object : 0;
    node_t* e = existing(b, h);
    made_t* m = made_of(b, h);
    if (h == B_DOCUMENT && strcmp(name, "body") == 0)
    {
        node_t* body = NULL;
        node_t* found[1];
        if (css_select(b->c, "body", b->c->document, found, 1) == 1)
            body = found[0];
        *value = element(body);
        return 0;
    }
    if (m != NULL && strcmp(name, "classList") == 0)
    {
        *value = object(B_MADE_CLASSES + (h - B_MADE));
        return 0;
    }
    if (e != NULL && (strcmp(name, "innerText") == 0 || strcmp(name, "textContent") == 0))
    {
        char text[256];
        content(e, text, sizeof(text), 0, 0);
        *value = string(copy(b, text));
        return 0;
    }
    if (e != NULL && strcmp(name, "id") == 0)
    {
        *value = string((e->id != NULL) ? e->id : "");
        return 0;
    }
    *value = object(B_IGNORED);                 /* style, classList of the page's, ...: not what is built */
    return 0;
}

static int b_set(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* obj, const char* name, const dmvs_js_value_t* value)
{
    builder_t* b = ctx;
    (void)c;
    char number[24];
    uint32_t h = (obj->kind == DMVS_JS_V_OBJECT) ? obj->object : 0;
    const char* text = text_of(value, number, sizeof(number));
    made_t* m = made_of(b, h);
    node_t* e = existing(b, h);
    if (text == NULL)
        return 0;
    if (m != NULL)
    {
        if (strcmp(name, "className") == 0)
            m->classes = copy(b, text);
        else if (strcmp(name, "id") == 0)
            m->id = copy(b, text);
        else if (strcmp(name, "innerHTML") == 0)
            m->html = copy(b, text);
        else if (strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0)
            m->text = copy(b, text);
        return 0;
    }
    if (e != NULL && strcmp(name, "innerHTML") == 0)
    {
        /* Emptied (a list rendered anew), or its markup */
        parent_t* p = parent_of(b, e);
        if (p != NULL)
        {
            p->clear = true;
            p->child_count = 0;
            p->html = (text[0] != '\0') ? copy(b, text) : NULL;
        }
    }
    return 0;
}

static int b_call(void* ctx, dmvs_js_compiler_t c, const dmvs_js_value_t* obj, const char* method,
                  const dmvs_js_value_t* args, uint32_t count, dmvs_js_value_t* result)
{
    builder_t* b = ctx;
    char number[24];
    uint32_t h = (obj->kind == DMVS_JS_V_OBJECT) ? obj->object : 0;
    const char* a0 = (count > 0) ? text_of(&args[0], number, sizeof(number)) : NULL;
    memset(result, 0, sizeof(*result));
    if (h == B_DOCUMENT && strcmp(method, "getElementById") == 0 && a0 != NULL)
    {
        *result = element(find_id(b->c->document, a0, 0));
        return 0;
    }
    if ((h == B_DOCUMENT || existing(b, h) != NULL) && a0 != NULL &&
        (strcmp(method, "querySelector") == 0 || strcmp(method, "querySelectorAll") == 0))
    {
        node_t* found[32];
        node_t* under = (h == B_DOCUMENT) ? b->c->document : existing(b, h);
        uint32_t n = css_select(b->c, a0, under, found, 32);
        if (method[13] != 'A')
        {
            *result = element((n > 0) ? found[0] : NULL);
            return 0;
        }
        dmvs_js_value_t list[32];
        for (uint32_t i = 0; i < n; i++)
            list[i] = element(found[i]);
        return dmvs_js_array(c, list, n, result);
    }
    if (h == B_DOCUMENT && strcmp(method, "createElement") == 0 && a0 != NULL)
    {
        if (b->made_count >= MAX_MADE)
            return 0;
        made_t* m = &b->made[b->made_count++];
        memset(m, 0, sizeof(*m));
        m->tag = copy(b, a0);
        *result = object(B_MADE + b->made_count);
        return 0;
    }
    if (strcmp(method, "appendChild") == 0 && count == 1 && args[0].kind == DMVS_JS_V_OBJECT)
    {
        made_t* child = made_of(b, args[0].object);
        made_t* m = made_of(b, h);
        node_t* e = existing(b, h);
        uint32_t k = args[0].object - B_MADE;
        if (child == NULL || child->appended)
            return 0;
        if (m != NULL && m->child_count < MAX_CHILDREN)
            m->children[m->child_count++] = k;
        else if (e != NULL)
        {
            parent_t* p = parent_of(b, e);
            if (p == NULL || p->child_count >= MAX_CHILDREN)
                return 0;
            p->children[p->child_count++] = k;
        }
        else
            return 0;
        child->appended = true;
        *result = args[0];
        return 0;
    }
    if (h > B_MADE_CLASSES && h <= B_MADE_CLASSES + b->made_count && a0 != NULL && strcmp(method, "add") == 0)
    {
        made_t* m = &b->made[h - B_MADE_CLASSES - 1U];
        size_t n = (m->classes != NULL) ? strlen(m->classes) : 0;
        char* joined = arena_alloc(&b->c->arena, n + strlen(a0) + 2U);
        if (joined != NULL)
        {
            Dmod_SnPrintf(joined, n + strlen(a0) + 2U, "%s%s%s", (n > 0) ? m->classes : "", (n > 0) ? " " : "", a0);
            m->classes = joined;
        }
        return 0;
    }
    if (strcmp(method, "setAttribute") == 0 && count == 2 && made_of(b, h) != NULL && a0 != NULL)
    {
        made_t* m = made_of(b, h);
        char v[24];
        const char* value = text_of(&args[1], v, sizeof(v));
        if (value != NULL && strchr(value, '"') == NULL)
        {
            size_t n = (m->attrs != NULL) ? strlen(m->attrs) : 0;
            size_t size = n + strlen(a0) + strlen(value) + 5U;
            char* attrs = arena_alloc(&b->c->arena, size);
            if (attrs != NULL)
            {
                Dmod_SnPrintf(attrs, size, "%s %s=\"%s\"", (n > 0) ? m->attrs : "", a0, value);
                m->attrs = attrs;
            }
        }
        return 0;
    }
    return 0;                                   /* Not what is built */
}

static void b_report(void* ctx, uint32_t line, uint32_t column, const char* message)
{
    (void)ctx;
    (void)line;
    (void)column;
    (void)message;                              /* The compilation reports it */
}

/* ---- The markup of what was made ---- */

typedef struct
{
    char*   out;
    size_t  n;
    bool    full;
} markup_t;

static void put(markup_t* m, const char* s, bool escape)
{
    for (; *s != '\0'; s++)
    {
        const char* e = NULL;
        if (escape)
            e = (*s == '<') ? "&lt;" : (*s == '>') ? "&gt;" : (*s == '&') ? "&amp;" : (*s == '"') ? "&quot;" : NULL;
        size_t len = (e != NULL) ? strlen(e) : 1U;
        if (m->n + len + 1U >= MARKUP_SIZE)
        {
            m->full = true;
            return;
        }
        memcpy(m->out + m->n, (e != NULL) ? e : s, len);
        m->n += len;
    }
    m->out[m->n] = '\0';
}

static void write_made(builder_t* b, markup_t* m, uint32_t k, uint32_t depth)
{
    const made_t* e = &b->made[k - 1U];
    char mark[32];
    if (depth > 16U)
        return;
    put(m, "<", false);
    put(m, e->tag, true);
    Dmod_SnPrintf(mark, sizeof(mark), " data-dmvs-built=\"%u\"", (unsigned)k);
    put(m, mark, false);
    if (e->classes != NULL)
    {
        put(m, " class=\"", false);
        put(m, e->classes, true);
        put(m, "\"", false);
    }
    if (e->id != NULL)
    {
        put(m, " id=\"", false);
        put(m, e->id, true);
        put(m, "\"", false);
    }
    if (e->attrs != NULL)
        put(m, e->attrs, false);
    put(m, ">", false);
    if (e->html != NULL)
        put(m, e->html, false);
    else if (e->text != NULL)
        put(m, e->text, true);
    for (uint32_t i = 0; i < e->child_count; i++)
        write_made(b, m, e->children[i], depth + 1U);
    put(m, "</", false);
    put(m, e->tag, true);
    put(m, ">", false);
}

/* ---- Building ---- */

static void find_scripts(conv_t* c, node_t* n, dmvs_js_compiler_t js, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (node_is(k, "script"))
        {
            const char* type = node_attr(k, "type");
            bool is_js = type == NULL || type[0] == '\0' || strcmp(type, "module") == 0 || strcmp(type, "text/javascript") == 0;
            if (is_js && node_attr(k, "src") == NULL && k->first != NULL && k->first->kind == NODE_TEXT)
            {
                dmvs_js_error_t error;
                dmvs_js_ast_t ast = dmvs_js_parse(k->first->text, k->first->length, &error);
                if (ast != NULL)
                    (void)dmvs_js_compile(js, ast);
            }
            continue;
        }
        find_scripts(c, k, js, depth + 1U);
    }
}

int script_build(conv_t* c, conv_t* into)
{
    builder_t* b = Dmod_Malloc(sizeof(*b));
    if (b == NULL)
        return -ENOMEM;
    memset(b, 0, sizeof(*b));
    b->c = c;
    dmvs_js_host_t host;
    memset(&host, 0, sizeof(host));
    host.ctx = b;
    host.global = b_global;
    host.get = b_get;
    host.set = b_set;
    host.call = b_call;
    host.report = b_report;
    dmvs_js_compiler_t js = dmvs_js_evaluator_new(&host);
    if (js == NULL)
    {
        Dmod_Free(b);
        return -ENOMEM;
    }
    find_scripts(c, c->document, js, 0);

    /* What the page's elements are given: operations of `into` */
    int status = 0;
    dom_t* ops = (b->parent_count > 0) ? arena_alloc(&into->arena, b->parent_count * sizeof(dom_t)) : NULL;
    char* buffer = (b->parent_count > 0) ? Dmod_Malloc(MARKUP_SIZE) : NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < b->parent_count && ops != NULL && buffer != NULL; i++)
    {
        const parent_t* p = &b->parents[i];
        markup_t m = { buffer, 0, false };
        buffer[0] = '\0';
        if (p->html != NULL)
            put(&m, p->html, false);
        for (uint32_t k = 0; k < p->child_count; k++)
            write_made(b, &m, p->children[k], 0);
        if (m.full)
            WARN(c, "script: what a script builds in #%u is too large - not converted\n", (unsigned)p->index);
        if ((!p->clear && m.n == 0) || m.full)
            continue;
        ops[count].parent = p->index;
        ops[count].clear = p->clear;
        ops[count].markup = arena_strndup(&into->arena, buffer, m.n);
        if (ops[count].markup == NULL)
            status = -ENOMEM;
        count++;
    }
    if (b->parent_count > 0 && (ops == NULL || buffer == NULL))
        status = -ENOMEM;
    into->dom = ops;
    into->dom_count = count;
    Dmod_Free(buffer);
    dmvs_js_compiler_free(js);
    Dmod_Free(b);
    return status;
}

/* The highest index of an element under n */
static uint32_t highest(const node_t* n, uint32_t max, uint32_t depth)
{
    for (const node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->index > max)
            max = k->index;
        max = highest(k, max, depth + 1U);
    }
    return max;
}

static void renumber(node_t* n, node_t* parent, uint32_t* next, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        k->parent = parent;
        if (k->kind == NODE_ELEMENT)
        {
            k->index = ++*next;
            renumber(k, k, next, depth + 1U);
        }
    }
}

void apply_dom(conv_t* c)
{
    uint32_t next = highest(c->document, 0, 0);
    for (uint32_t i = 0; i < c->dom_count; i++)
    {
        const dom_t* op = &c->dom[i];
        node_t* parent = by_index(c->document, op->parent, 0);
        if (parent == NULL)
            continue;
        if (op->clear)
        {
            parent->first = NULL;
            parent->last = NULL;
        }
        node_t* frag = html_parse(c, op->markup, strlen(op->markup));
        if (frag == NULL)
            continue;
        renumber(frag, parent, &next, 0);           /* The page's numbers, then these: the same in every state */
        for (node_t* k = frag->first; k != NULL; )
        {
            node_t* following = k->next;
            k->next = NULL;
            k->prev = parent->last;
            if (parent->last != NULL)
                parent->last->next = k;
            else
                parent->first = k;
            parent->last = k;
            k->parent = parent;
            k = following;
        }
    }
}

node_t* built_element(conv_t* c, uint32_t k)
{
    char mark[16];
    Dmod_SnPrintf(mark, sizeof(mark), "%u", (unsigned)k);
    /* By its mark: data-dmvs-built="k" */
    node_t* found[1];
    char selector[48];
    Dmod_SnPrintf(selector, sizeof(selector), "[data-dmvs-built=\"%s\"]", mark);
    return (css_select(c, selector, c->document, found, 1) == 1) ? found[0] : NULL;
}
