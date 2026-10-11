#include "private.h"
#include "dmvs_js.h"
#include <errno.h>
#include <string.h>

/*
 * Scripts: a page's JavaScript compiled into the view's code by dmvs_js -
 * this is its host, the DOM. What the scripts do with elements:
 *
 *   document.getElementById / querySelector(All)   the elements, at conversion
 *   el.addEventListener('click', f), el.onclick, onclick="..."   its click
 *   el.classList.add / remove / toggle / replace / contains      its looks
 *   el.style.opacity = '0.3'                       a style changes
 *   el.innerText = ..., parseFloat(el.innerText)   a text variable (and its number)
 *   let current = null; ... current = el           elements in a variable
 *
 * A change of an element (a class, a style) is laid out: the page with it,
 * how the element moved and faded is what its variables become - x, y,
 * opacity of the group it is painted in - set at once, or animated by the
 * element's CSS transition; another look of it is painted as a variant.
 * That is known only when every change is known: the code calls a handler
 * of the change, made at the end. The classes an element changes together
 * (up to where the code goes another way: the compiler's flush) are one
 * change - the look they make together.
 *
 * What is not understood is reported and left out.
 */

#define MAX_HANDLES         512u
#define MAX_PLACES          256u
#define MAX_HELD            64u
#define MAX_CLICKS          64u
#define MAX_CLICK_CALLS     8u
#define MAX_MODS            128u
#define MAX_CLASSVARS       32u
#define MAX_ACTIONS         256u
#define MAX_DOMAIN          32u
#define MAX_PROGRAMS        32u
#define TEXT_VAR_SIZE       64u
#define MAX_WIDE_CHARS      96u

/* ---- The host's objects ---- */

#define H_DOCUMENT          1u
#define H_ELEMENT           2u
#define H_CLASSES           3u              /* x.classList */
#define H_STYLE             4u              /* x.style */
#define H_IGNORED           5u              /* tailwind (its config is read with the style sheets) */

typedef struct
{
    uint8_t         kind;
    node_t*         element;                /* ELEMENT; CLASSES, STYLE of one ... */
    dmvsi_var_t     holder;                 /* ... or of the element a variable holds */
} handle_t;

/* A change of an element in the code: a handler made when the changes are laid out */
typedef struct
{
    node_t*         element;
    uint8_t         kind;                   /* MOD_* */
    const char*     name;
    const char*     value;
    dmvsi_handler_t handler;
} place_t;

/* A change of the element a variable holds: an IF for each element it may hold */
typedef struct
{
    dmvsi_var_t     holder;
    uint8_t         kind;
    const char*     name;
    const char*     value;
    dmvsi_handler_t handler;
    dmvsi_action_t* actions;                /* Its IFs, CALLs of the elements' changes (expand_held()) */
    uint32_t        count;
} held_t;

/* What clicking an element does: its onclick, then its listeners - a handler made at the end */
typedef struct
{
    node_t*         element;
    dmvsi_handler_t handler;
    dmvsi_handler_t onclick;
    dmvsi_handler_t calls[MAX_CLICK_CALLS];
    uint32_t        count;
} click_t;

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

/* A class of an element a script asks about (contains, toggle), or that changes its look: its variable */
typedef struct
{
    node_t*         element;
    const char*     name;
    dmvsi_var_t     var;
} classvar_t;

/* The classes changed of an element, until the code goes another way: one change of them all */
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

/* An image whose src a script sets to one of known ones (dmvs_js_choices()): a variant of each, its variable which */
#define MAX_SRCVARS     8u

typedef struct
{
    node_t*         element;
    dmvsi_var_t     var;
    const char*     picks[MAX_VARIANTS];
    uint32_t        count;
} srcvar_t;

typedef struct
{
    conv_t*             c;
    dmvs_js_compiler_t  js;
    handle_t            handles[MAX_HANDLES];
    uint32_t            handle_count;
    place_t             places[MAX_PLACES];
    uint32_t            place_count;
    held_t              held[MAX_HELD];
    uint32_t            held_count;
    click_t             clicks[MAX_CLICKS];
    uint32_t            click_count;
    change_t            changes[MAX_MODS];
    uint32_t            change_count;
    classvar_t          classvars[MAX_CLASSVARS];
    uint32_t            classvar_count;
    pending_t           pending[MAX_PENDING];
    uint32_t            pending_count;
    lookvar_t           lookvars[MAX_LOOKVARS];
    uint32_t            lookvar_count;
    dmvsi_action_t      actions[MAX_ACTIONS];   /* A handler being made at the end */
    uint32_t            action_count;
    uint32_t            ascii[4];               /* The characters of the scripts (text variables may show them) */
    uint32_t            wide[MAX_WIDE_CHARS];
    uint32_t            wide_count;
    const char*         chars;
    uint32_t            made;                   /* document.createElement() so far: the k-th is the build's */
    srcvar_t            srcvars[MAX_SRCVARS];
    uint32_t            srcvar_count;
} script_t;

static void report(script_t* sc, const char* what)
{
    dmvs_js_report(sc->js, what);
}

/* An element the scripts built as the page loaded (build.c): what they set of it is as it was then */
static bool is_built(const node_t* e)
{
    return e != NULL && e->kind == NODE_ELEMENT && node_attr(e, "data-dmvs-built") != NULL;
}

/* Holding what the scripts built: emptying it, appending to it, is what the build did */
static bool holds_built(const node_t* e)
{
    for (const node_t* k = (e != NULL) ? e->first : NULL; k != NULL; k = k->next)
    {
        if (is_built(k))
            return true;
    }
    return false;
}

static const char* label_of(const node_t* e)
{
    return (e->id != NULL) ? e->id : e->tag;
}

static dynamic_t* dynamic_of(script_t* sc, node_t* e)
{
    if (e->dynamic == NULL)
        e->dynamic = arena_alloc(&sc->c->arena, sizeof(dynamic_t));
    return e->dynamic;
}

static uint32_t handle(script_t* sc, uint8_t kind, node_t* e, dmvsi_var_t holder)
{
    for (uint32_t i = 0; i < sc->handle_count; i++)
    {
        const handle_t* h = &sc->handles[i];
        if (h->kind == kind && h->element == e && h->holder == holder)
            return i + 1U;
    }
    if (sc->handle_count >= MAX_HANDLES)
        return 0;
    sc->handles[sc->handle_count] = (handle_t){ kind, e, holder };
    return ++sc->handle_count;
}

static const handle_t* handle_at(const script_t* sc, uint32_t h)
{
    return (h >= 1 && h <= sc->handle_count) ? &sc->handles[h - 1U] : NULL;
}

static dmvs_js_value_t object_value(uint32_t h)
{
    dmvs_js_value_t v;
    memset(&v, 0, sizeof(v));
    v.kind = (h != 0) ? DMVS_JS_V_OBJECT : DMVS_JS_V_NULL;
    v.object = h;
    return v;
}

static dmvs_js_value_t element_value(script_t* sc, node_t* e)
{
    return object_value((e != NULL) ? handle(sc, H_ELEMENT, e, 0) : 0);
}

/* What a value of the host is: one element (`*element`), or a variable holding them (`*holder`) */
static uint8_t object_of(script_t* sc, const dmvs_js_value_t* v, node_t** element, dmvsi_var_t* holder)
{
    *element = NULL;
    *holder = 0;
    if (v->kind == DMVS_JS_V_RUNTIME)
    {
        *holder = v->var;
        return H_ELEMENT;
    }
    const handle_t* h = (v->kind == DMVS_JS_V_OBJECT) ? handle_at(sc, v->object) : NULL;
    if (h == NULL)
        return 0;
    *element = h->element;
    *holder = h->holder;
    return h->kind;
}

static const char* static_text(script_t* sc, const dmvs_js_value_t* v)
{
    dmvsi_var_t var;
    const char* text = NULL;
    if (v->kind == DMVS_JS_V_RUNTIME || dmvs_js_text_operand(sc->js, v, &var, &text) != 0 || var != 0)
        return NULL;
    return text;
}

/* ---- Emitting ---- */

static void emit_into(script_t* sc, const dmvsi_action_t* a)
{
    dmvs_js_emit(sc->js, a);
}

static void emit_op(script_t* sc, uint8_t kind, dmvsi_var_t var, dmvsi_var_t operand, int32_t value, const char* text)
{
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = kind;
    a.var = var;
    a.operand = operand;
    a.value = value;
    a.text = text;
    emit_into(sc, &a);
}

static void emit_call(script_t* sc, dmvsi_handler_t h)
{
    dmvsi_action_t a;
    memset(&a, 0, sizeof(a));
    a.kind = DMVSI_ACT_CALL;
    a.handler = h;
    if (h != 0)
        emit_into(sc, &a);
}

/* A change in the code here: a CALL of its handler (made at the end) */
static void place(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    if (sc->place_count >= MAX_PLACES)
    {
        report(sc, "too many changes of elements - not converted");
        return;
    }
    place_t* p = &sc->places[sc->place_count];
    p->handler = dmvsi_new_handler(sc->c->doc);
    if (p->handler == 0)
        return;
    p->element = e;
    p->kind = kind;
    p->name = name;
    p->value = value;
    sc->place_count++;
    emit_call(sc, p->handler);
}

static void place_held(script_t* sc, dmvsi_var_t holder, uint8_t kind, const char* name, const char* value)
{
    if (sc->held_count >= MAX_HELD)
    {
        report(sc, "too many changes of elements in variables - not converted");
        return;
    }
    held_t* h = &sc->held[sc->held_count];
    h->handler = dmvsi_new_handler(sc->c->doc);
    if (h->handler == 0)
        return;
    h->holder = holder;
    h->kind = kind;
    h->name = name;
    h->value = value;
    sc->held_count++;
    emit_call(sc, h->handler);
}

/* The classes changed of each element so far, as their changes (the compiler's flush) */
static void flush_pending(script_t* sc)
{
    uint32_t count = sc->pending_count;
    sc->pending_count = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        pending_t* p = &sc->pending[i];
        if (p->count == 1)
        {
            place(sc, p->element, p->add[0] ? MOD_CLASS_ADD : MOD_CLASS_REMOVE, p->names[0], NULL);
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
            place(sc, p->element, MOD_CLASSES, name, NULL);
    }
}

static void host_flush(void* ctx, dmvs_js_compiler_t js)
{
    (void)js;
    flush_pending(ctx);
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
            flush_pending(sc);
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

static bool has_class(const node_t* n, const char* name);

/* A class's variable (1: the element has it): made when it is first asked about */
static classvar_t* classvar(script_t* sc, node_t* e, const char* name, bool make_it)
{
    for (uint32_t i = 0; i < sc->classvar_count; i++)
    {
        if (sc->classvars[i].element == e && strcmp(sc->classvars[i].name, name) == 0)
            return &sc->classvars[i];
    }
    if (!make_it || sc->classvar_count >= MAX_CLASSVARS)
        return NULL;
    char label[48];
    Dmod_SnPrintf(label, sizeof(label), "%s_%s", label_of(e), name);
    dmvsi_var_t var = dmvsi_add_var(sc->c->doc, label, has_class(e, name) ? 1 : 0);
    if (var == 0)
        return NULL;
    classvar_t* cv = &sc->classvars[sc->classvar_count++];
    cv->element = e;
    cv->name = name;
    cv->var = var;
    return cv;
}

/* ---- Texts ---- */

/* The text an element shows: its text nodes' */
static size_t text_content(const node_t* n, char* out, size_t size, size_t at, uint32_t depth)
{
    for (const node_t* k = n->first; k != NULL && depth < 64U; k = k->next)
    {
        if (k->kind == NODE_TEXT)
        {
            for (size_t i = 0; i < k->length && at + 1U < size; i++)
                out[at++] = k->text[i];
        }
        else if (k->kind == NODE_ELEMENT)
            at = text_content(k, out, size, at, depth + 1U);
    }
    out[(at < size) ? at : size - 1U] = '\0';
    return at;
}

/* Its text variable (and the number it is), made when a script first uses its text */
static dynamic_t* text_of(script_t* sc, node_t* e)
{
    dynamic_t* d = dynamic_of(sc, e);
    if (d == NULL || d->text != 0)
        return d;
    char text[TEXT_VAR_SIZE];
    char label[48];
    double number = 0.0;
    size_t n = text_content(e, text, sizeof(text), 0, 0);
    size_t a = 0;
    while (a < n && (text[a] == ' ' || text[a] == '\n' || text[a] == '\t' || text[a] == '\r'))
        a++;
    while (n > a && (text[n - 1U] == ' ' || text[n - 1U] == '\n' || text[n - 1U] == '\t' || text[n - 1U] == '\r'))
        text[--n] = '\0';
    Dmod_SnPrintf(label, sizeof(label), "%s_text", label_of(e));
    d->text = dmvsi_add_text_var(sc->c->doc, label, TEXT_VAR_SIZE, text + a);
    Dmod_SnPrintf(label, sizeof(label), "%s_number", label_of(e));
    bool known = dmvs_js_parse_number(text + a, n - a, &number);
    d->number = dmvsi_add_var(sc->c->doc, label, known ? (int32_t)(number * 1000.0 + ((number < 0) ? -0.5 : 0.5)) : 0);
    return d;
}

static void set_text(script_t* sc, node_t* e, const dmvs_js_value_t* v)
{
    if (v->kind == DMVS_JS_V_UNDEFINED)
        return;                             /* What was not converted (reported): the text stays */
    dynamic_t* d = text_of(sc, e);
    if (d == NULL || d->text == 0)
        return;
    dmvsi_var_t var;
    const char* text = NULL;
    if (dmvs_js_text_operand(sc->js, v, &var, &text) != 0)
    {
        report(sc, "a text the view cannot show - not converted");
        return;
    }
    if (var != d->text)
        emit_op(sc, DMVSI_ACT_SET, d->text, var, 0, (var == 0) ? text : NULL);
    /* Its number: parseFloat(el.innerText) */
    double n = 0.0;
    if (v->kind == DMVS_JS_V_RUNTIME && v->number_var != 0)
        emit_op(sc, DMVSI_ACT_SET, d->number, v->number_var, 0, NULL);
    else if (text != NULL && dmvs_js_parse_number(text, strlen(text), &n))
        emit_op(sc, DMVSI_ACT_SET, d->number, 0, (int32_t)(n * 1000.0 + ((n < 0) ? -0.5 : 0.5)), NULL);
    else if (v->kind == DMVS_JS_V_NUMBER)
        emit_op(sc, DMVSI_ACT_SET, d->number, 0, (int32_t)(v->number * 1000.0 + ((v->number < 0) ? -0.5 : 0.5)), NULL);
}

/* The characters a script may show: every one of its source (what it writes is made of them) and digits */
static void add_chars(script_t* sc, const char* s, size_t n)
{
    for (size_t i = 0; i < n; )
    {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80)
        {
            if (b >= 0x20)
                sc->ascii[b >> 5] |= 1u << (b & 31u);
            i++;
            continue;
        }
        size_t len = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
        uint32_t cp = 0;
        memcpy(&cp, s + i, (i + len <= n) ? len : 1);
        bool known = false;
        for (uint32_t k = 0; k < sc->wide_count && !known; k++)
            known = sc->wide[k] == cp;
        if (!known && sc->wide_count < MAX_WIDE_CHARS && len > 1)
            sc->wide[sc->wide_count++] = cp;
        i += len;
    }
}

static const char* chars_of(script_t* sc)
{
    if (sc->chars != NULL)
        return sc->chars;
    add_chars(sc, "0123456789-.,:% ", 16);
    char* out = arena_alloc(&sc->c->arena, 96U + 4U * sc->wide_count + 1U);
    if (out == NULL)
        return "";
    size_t n = 0;
    for (uint32_t b = 0x20; b < 0x7F; b++)
    {
        if ((sc->ascii[b >> 5] & (1u << (b & 31u))) != 0)
            out[n++] = (char)b;
    }
    for (uint32_t k = 0; k < sc->wide_count; k++)
    {
        const char* w = (const char*)&sc->wide[k];
        uint8_t b = (uint8_t)w[0];
        size_t len = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : 2;
        memcpy(out + n, w, len);
        n += len;
    }
    out[n] = '\0';
    sc->chars = out;
    return out;
}

/* ---- Clicks ---- */

static click_t* click_of(script_t* sc, node_t* e)
{
    for (uint32_t i = 0; i < sc->click_count; i++)
    {
        if (sc->clicks[i].element == e)
            return &sc->clicks[i];
    }
    if (sc->click_count >= MAX_CLICKS)
        return NULL;
    click_t* k = &sc->clicks[sc->click_count];
    memset(k, 0, sizeof(*k));
    k->element = e;
    k->handler = dmvsi_new_handler(sc->c->doc);
    if (k->handler == 0)
        return NULL;
    sc->click_count++;
    return k;
}

static bool listened(const script_t* sc, const node_t* e)
{
    for (uint32_t i = 0; i < sc->click_count; i++)
    {
        if (sc->clicks[i].element == e && (sc->clicks[i].count > 0 || sc->clicks[i].onclick != 0))
            return true;
    }
    return false;
}

static int add_listener(script_t* sc, node_t* e, const dmvs_js_value_t* fn)
{
    click_t* k = click_of(sc, e);
    if (k == NULL || k->count >= MAX_CLICK_CALLS)
    {
        report(sc, "too many listeners - not converted");
        return 0;
    }
    dmvsi_handler_t h = dmvs_js_function_handler(sc->js, fn, handle(sc, H_ELEMENT, e, 0));
    if (h == 0)
        return -ENOTSUP;
    k->calls[k->count++] = h;
    return 0;
}

/* ---- The host ---- */

static bool host_global(void* ctx, dmvs_js_compiler_t js, const char* name, dmvs_js_value_t* value)
{
    script_t* sc = ctx;
    (void)js;
    if (strcmp(name, "document") == 0)
        *value = object_value(handle(sc, H_DOCUMENT, NULL, 0));
    else if (strcmp(name, "tailwind") == 0)
        *value = object_value(handle(sc, H_IGNORED, NULL, 0));
    else
        return false;
    return true;
}

static int host_get(void* ctx, dmvs_js_compiler_t js, const dmvs_js_value_t* object, const char* name, dmvs_js_value_t* value)
{
    script_t* sc = ctx;
    node_t* e;
    dmvsi_var_t holder;
    uint8_t kind = object_of(sc, object, &e, &holder);
    (void)js;
    memset(value, 0, sizeof(*value));
    if (kind == H_IGNORED)
    {
        *value = *object;
        return 0;
    }
    if (kind != H_ELEMENT)
        return -ENOTSUP;
    if (strcmp(name, "classList") == 0 || strcmp(name, "style") == 0)
    {
        *value = object_value(handle(sc, (name[0] == 'c') ? H_CLASSES : H_STYLE, e, holder));
        return 0;
    }
    if (e == NULL)
        return -ENOTSUP;                    /* The element a variable holds: only its classes, its style */
    if (strcmp(name, "innerText") == 0 || strcmp(name, "textContent") == 0)
    {
        dynamic_t* d = text_of(sc, e);
        if (d == NULL || d->text == 0)
            return -ENOMEM;
        value->kind = DMVS_JS_V_RUNTIME;
        value->type = DMVS_JS_T_TEXT;
        value->var = d->text;
        value->number_var = d->number;
        return 0;
    }
    if (strcmp(name, "id") == 0)
    {
        value->kind = DMVS_JS_V_STRING;
        value->text = (e->id != NULL) ? e->id : "";
        value->length = strlen(value->text);
        return 0;
    }
    return -ENOTSUP;
}

/* A class list's change from what the element has: "+a -b" (by name, as flush_pending() writes it) */
static const char* class_diff(script_t* sc, const node_t* e, const char* list)
{
    const char* names[2 * MAX_PENDING_OPS];
    bool add[2 * MAX_PENDING_OPS];
    uint32_t n = 0;
    char word[128];
    /* Added: what the list has, the element not */
    for (const char* q = list; *q != '\0' && n < MAX_PENDING_OPS; )
    {
        while (*q == ' ' || *q == '\t' || *q == '\n')
            q++;
        size_t k = 0;
        while (q[k] != '\0' && q[k] != ' ' && q[k] != '\t' && q[k] != '\n')
            k++;
        if (k == 0)
            break;
        size_t m = (k < sizeof(word) - 1U) ? k : sizeof(word) - 1U;
        memcpy(word, q, m);
        word[m] = '\0';
        q += k;
        if (!has_class(e, word))
        {
            names[n] = arena_strndup(&sc->c->arena, word, m);
            add[n++] = true;
        }
    }
    /* Removed: what the element has, the list not */
    for (uint32_t i = 0; i < e->class_count && n < 2U * MAX_PENDING_OPS; i++)
    {
        const char* cls = e->classes[i];
        size_t m = strlen(cls);
        bool kept = false;
        for (const char* q = list; *q != '\0' && !kept; )
        {
            while (*q == ' ' || *q == '\t' || *q == '\n')
                q++;
            size_t k = 0;
            while (q[k] != '\0' && q[k] != ' ' && q[k] != '\t' && q[k] != '\n')
                k++;
            kept = k == m && memcmp(q, cls, m) == 0;
            q += k;
        }
        if (!kept)
        {
            names[n] = cls;
            add[n++] = false;
        }
    }
    char spec[512];
    size_t len = 0;
    bool used[2 * MAX_PENDING_OPS] = { false };
    spec[0] = '\0';
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t best = n;
        for (uint32_t j = 0; j < n; j++)
            if (!used[j] && names[j] != NULL && (best == n || strcmp(names[j], names[best]) < 0))
                best = j;
        if (best == n)
            break;
        used[best] = true;
        len += (size_t)Dmod_SnPrintf(spec + len, sizeof(spec) - len, "%s%c%s", (len > 0) ? " " : "", add[best] ? '+' : '-', names[best]);
        if (len >= sizeof(spec))
            len = sizeof(spec) - 1U;
    }
    return arena_strndup(&sc->c->arena, spec, len);
}

/* el.className = one of known lists: the change to each, on which it is */
static int set_class_name(script_t* sc, node_t* e, const dmvs_js_value_t* value)
{
    const dmvs_js_value_t* picks = NULL;
    dmvsi_var_t index = 0;
    uint32_t n = dmvs_js_choices(sc->js, value, &picks, &index);
    const char* text = static_text(sc, value);
    if (text != NULL)
    {
        const char* spec = class_diff(sc, e, text);
        if (spec != NULL)
            place(sc, e, MOD_CLASSES, spec, NULL);
        return 0;
    }
    if (n == 0 || n > MAX_VARIANTS)
        return is_built(e) ? 0 : -ENOTSUP;      /* (what the build made it: as it is) */
    for (uint32_t i = 0; i < n; i++)
    {
        if (picks[i].kind != DMVS_JS_V_STRING)
            return -ENOTSUP;
    }
    for (uint32_t i = 0; i < n; i++)
    {
        const char* spec = class_diff(sc, e, picks[i].text);
        if (spec == NULL)
            return -ENOMEM;
        emit_op(sc, DMVSI_ACT_IF_EQ, index, 0, (int32_t)i, NULL);
        place(sc, e, MOD_CLASSES, spec, NULL);
        emit_op(sc, DMVSI_ACT_END, 0, 0, 0, NULL);
    }
    return 0;
}

/* img.src = one of known images: a variant of each (made at the end), its variable set to which */
static int set_src(script_t* sc, node_t* e, const dmvs_js_value_t* value)
{
    const dmvs_js_value_t* picks = NULL;
    dmvsi_var_t index = 0;
    uint32_t n = dmvs_js_choices(sc->js, value, &picks, &index);
    if (!node_is(e, "img") || n < 2 || n > MAX_VARIANTS)
        return -ENOTSUP;
    srcvar_t* sv = NULL;
    for (uint32_t i = 0; i < sc->srcvar_count && sv == NULL; i++)
        sv = (sc->srcvars[i].element == e) ? &sc->srcvars[i] : NULL;
    if (sv == NULL)
    {
        if (sc->srcvar_count >= MAX_SRCVARS)
            return -ENOTSUP;
        sv = &sc->srcvars[sc->srcvar_count];
        memset(sv, 0, sizeof(*sv));
        const char* now = node_attr(e, "src");
        int32_t initial = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            if (picks[i].kind != DMVS_JS_V_STRING)
                return -ENOTSUP;
            sv->picks[i] = arena_strndup(&sc->c->arena, picks[i].text, picks[i].length);
            if (now != NULL && strcmp(now, picks[i].text) == 0)
                initial = (int32_t)i;
        }
        char label[48];
        Dmod_SnPrintf(label, sizeof(label), "%s_src", label_of(e));
        if ((sv->var = dmvsi_add_var(sc->c->doc, label, initial)) == 0)
            return -ENOMEM;
        sv->element = e;
        sv->count = n;
        sc->srcvar_count++;
    }
    else if (sv->count != n)
        return -ENOTSUP;                        /* Of other images elsewhere */
    emit_op(sc, DMVSI_ACT_SET, sv->var, index, 0, NULL);
    return 0;
}

/* The images' variants: the element as it is, its src another */
static int make_src_variants(script_t* sc)
{
    for (uint32_t i = 0; i < sc->srcvar_count; i++)
    {
        srcvar_t* sv = &sc->srcvars[i];
        node_t* e = sv->element;
        dynamic_t* d = dynamic_of(sc, e);
        if (d == NULL)
            return -ENOMEM;
        if (d->variant_count > 0 || !e->box.placed)
        {
            WARN(sc->c, "script: the images of #%s as well as its looks - not converted\n", label_of(e));
            continue;
        }
        for (uint32_t k = 0; k < sv->count; k++)
        {
            node_t* look = arena_alloc(&sc->c->arena, sizeof(node_t));
            attr_t* src = arena_alloc(&sc->c->arena, sizeof(attr_t));
            if (look == NULL || src == NULL)
                return -ENOMEM;
            *look = *e;
            look->dynamic = NULL;
            src->name = (char*)"src";
            src->value = (char*)sv->picks[k];
            src->next = e->attrs;                /* (before the element's own: node_attr() finds it first) */
            look->attrs = src;
            d->variants[d->variant_count++] = (variant_t){ look, sv->var, (int32_t)k, -1 };
        }
    }
    return 0;
}

static int host_set(void* ctx, dmvs_js_compiler_t js, const dmvs_js_value_t* object, const char* name, const dmvs_js_value_t* value)
{
    script_t* sc = ctx;
    node_t* e;
    dmvsi_var_t holder;
    uint8_t kind = object_of(sc, object, &e, &holder);
    (void)js;
    if (kind == H_IGNORED)
        return 0;
    if (kind == H_STYLE)
    {
        const char* text = static_text(sc, value);
        if (text == NULL)
        {
            report(sc, "a style known only when the view runs - not converted");
            return 0;
        }
        const char* n = arena_strndup(&sc->c->arena, name, strlen(name));
        const char* t = arena_strndup(&sc->c->arena, text, strlen(text));
        if (n == NULL || t == NULL)
            return -ENOMEM;
        if (e != NULL)
            place(sc, e, MOD_STYLE, n, t);
        else
            place_held(sc, holder, MOD_STYLE, n, t);
        return 0;
    }
    if (kind != H_ELEMENT || e == NULL)
        return -ENOTSUP;
    if (strcmp(name, "className") == 0)
        return set_class_name(sc, e, value);
    if (strcmp(name, "src") == 0)
        return set_src(sc, e, value);
    if (is_built(e) && (strcmp(name, "innerHTML") == 0 || strcmp(name, "id") == 0))
        return 0;                           /* As the build made it */
    if (holds_built(e) && strcmp(name, "innerHTML") == 0)
        return 0;                           /* Emptied to be built anew: as the build left it */
    if (strcmp(name, "innerText") == 0 || strcmp(name, "textContent") == 0)
    {
        set_text(sc, e, value);
        return 0;
    }
    if (strcmp(name, "innerHTML") == 0)
    {
        /* Text (its character references decoded by the compiler) in what holds no elements: its text */
        const char* text = static_text(sc, value);
        bool elements = false;
        for (const node_t* k = e->first; k != NULL && !elements; k = k->next)
            elements = k->kind == NODE_ELEMENT;
        if (elements || (text != NULL && strchr(text, '<') != NULL))
            return -ENOTSUP;                /* Elements made or replaced */
        set_text(sc, e, value);
        return 0;
    }
    if (strcmp(name, "onclick") == 0)
        return add_listener(sc, e, value);
    return -ENOTSUP;
}

/* document / an element: querySelector(All) - the elements, as the page is */
static int select_elements(script_t* sc, node_t* under, const dmvs_js_value_t* selector, bool all, dmvs_js_value_t* result)
{
    const char* text = static_text(sc, selector);
    if (text == NULL)
    {
        report(sc, "a selector known only when the view runs - not converted");
        return 0;
    }
    node_t* found[MAX_DOMAIN];
    uint32_t count = css_select(sc->c, text, under, found, MAX_DOMAIN);
    if (!all)
    {
        *result = element_value(sc, (count > 0) ? found[0] : NULL);
        return 0;
    }
    dmvs_js_value_t values[MAX_DOMAIN];
    for (uint32_t i = 0; i < count; i++)
        values[i] = element_value(sc, found[i]);
    return dmvs_js_array(sc->js, values, count, result);
}

static int classes_call(script_t* sc, node_t* e, dmvsi_var_t holder, const char* method, const dmvs_js_value_t* args,
                        uint32_t count, dmvs_js_value_t* result)
{
    bool add = strcmp(method, "add") == 0, remove = strcmp(method, "remove") == 0;
    bool toggle = strcmp(method, "toggle") == 0, replace = strcmp(method, "replace") == 0;
    bool contains = strcmp(method, "contains") == 0;
    const char* names[MAX_PENDING_OPS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < MAX_PENDING_OPS; i++)
    {
        const char* text = static_text(sc, &args[i]);
        if (text == NULL)
        {
            report(sc, "a class known only when the view runs - not converted");
            return 0;
        }
        names[n] = arena_strndup(&sc->c->arena, text, strlen(text));
        if (names[n++] == NULL)
            return -ENOMEM;
    }
    if (e == NULL)
    {
        /* The element a variable holds: each class, a change of whichever it is */
        if (!(add || remove))
        {
            report(sc, "classList.toggle / contains / replace of an element in a variable - not converted");
            return 0;
        }
        for (uint32_t i = 0; i < n; i++)
            place_held(sc, holder, add ? MOD_CLASS_ADD : MOD_CLASS_REMOVE, names[i], NULL);
        return 0;
    }
    if (add || remove)
    {
        for (uint32_t i = 0; i < n; i++)
            class_op(sc, e, names[i], add);
        return 0;
    }
    if (replace && n == 2)
    {
        class_op(sc, e, names[0], false);
        class_op(sc, e, names[1], true);
        return 0;
    }
    if ((toggle || contains) && n >= 1)
    {
        classvar_t* cv = classvar(sc, e, names[0], true);
        if (cv == NULL)
            return -ENOMEM;
        if (toggle)
        {
            /* Its variable flipped, then what it is applied */
            emit_op(sc, DMVSI_ACT_TOGGLE, cv->var, 0, 0, NULL);
            emit_op(sc, DMVSI_ACT_IF_NE, cv->var, 0, 0, NULL);
            place(sc, e, MOD_CLASS_ADD, names[0], NULL);
            emit_op(sc, DMVSI_ACT_ELSE, 0, 0, 0, NULL);
            place(sc, e, MOD_CLASS_REMOVE, names[0], NULL);
            emit_op(sc, DMVSI_ACT_END, 0, 0, 0, NULL);
        }
        memset(result, 0, sizeof(*result));
        result->kind = DMVS_JS_V_RUNTIME;
        result->type = DMVS_JS_T_BOOL;
        result->var = cv->var;
        return 0;
    }
    return -ENOTSUP;
}

static int host_call(void* ctx, dmvs_js_compiler_t js, const dmvs_js_value_t* object, const char* method,
                     const dmvs_js_value_t* args, uint32_t count, dmvs_js_value_t* result)
{
    script_t* sc = ctx;
    node_t* e;
    dmvsi_var_t holder;
    uint8_t kind = object_of(sc, object, &e, &holder);
    (void)js;
    memset(result, 0, sizeof(*result));
    if (kind == H_IGNORED)
        return 0;
    if (kind == H_CLASSES)
        return classes_call(sc, e, holder, method, args, count, result);
    if (kind != H_DOCUMENT && (kind != H_ELEMENT || e == NULL))
        return -ENOTSUP;
    node_t* under = (kind == H_DOCUMENT) ? sc->c->document : e;
    if (kind == H_DOCUMENT && strcmp(method, "getElementById") == 0 && count == 1)
    {
        const char* id = static_text(sc, &args[0]);
        if (id == NULL)
        {
            report(sc, "an element known only when the view runs - not converted");
            return 0;
        }
        *result = element_value(sc, find_id(sc->c->document, id, 0));
        return 0;
    }
    if ((strcmp(method, "querySelector") == 0 || strcmp(method, "querySelectorAll") == 0) && count == 1)
        return select_elements(sc, under, &args[0], method[13] == 'A', result);
    if (kind == H_DOCUMENT && strcmp(method, "createElement") == 0)
    {
        /* The k-th: the element the build made (when it was put in the page) */
        node_t* made = built_element(sc->c, ++sc->made);
        if (made == NULL)
        {
            report(sc, "an element made that is not in the page as it loads - not converted");
            return 0;
        }
        *result = element_value(sc, made);
        return 0;
    }
    if (strcmp(method, "appendChild") == 0 && count == 1)
    {
        node_t* child;
        dmvsi_var_t held;
        if (object_of(sc, &args[0], &child, &held) == H_ELEMENT && is_built(child))
        {
            *result = args[0];
            return 0;                       /* Where the build put it */
        }
        return -ENOTSUP;
    }
    if (kind != H_ELEMENT)
        return -ENOTSUP;
    if (strcmp(method, "getAttribute") == 0 && count == 1)
    {
        const char* name = static_text(sc, &args[0]);
        const char* value = (name != NULL) ? node_attr(e, name) : NULL;
        result->kind = (value != NULL) ? DMVS_JS_V_STRING : DMVS_JS_V_NULL;
        result->text = value;
        result->length = (value != NULL) ? strlen(value) : 0;
        return (name != NULL) ? 0 : -ENOTSUP;
    }
    if (strcmp(method, "addEventListener") == 0 && count >= 2)
    {
        const char* type = static_text(sc, &args[0]);
        if (type == NULL || strcmp(type, "click") != 0)
        {
            report(sc, "a listener of what is not a click - not converted");
            return 0;
        }
        return add_listener(sc, e, &args[1]);
    }
    if (strcmp(method, "click") == 0)
    {
        click_t* k = click_of(sc, e);
        if (k != NULL)
            emit_call(sc, k->handler);      /* What clicking it does, here */
        return 0;
    }
    return -ENOTSUP;
}

static void host_report(void* ctx, uint32_t line, uint32_t column, const char* message)
{
    script_t* sc = ctx;
    WARN(sc->c, "script %u:%u: %s\n", (unsigned)line, (unsigned)column, message);
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
        {
            h = (h ^ (uint32_t)(f->x + f->y * 7 + (int32_t)f->length)) * 16777619u;
            for (size_t i = 0; f->text != NULL && i < f->length; i++)
                h = (h ^ (uint8_t)f->text[i]) * 16777619u;      /* Another text, another icon (::before) */
        }
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
    s->dom = c->dom;                        /* What the scripts built */
    s->dom_count = c->dom_count;
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
    {
        h = mix(h, f->x + f->y * 7 + (int32_t)f->length);
        for (size_t i = 0; f->text != NULL && i < f->length; i++)
            h = mix(h, (uint8_t)f->text[i]);
    }
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

/*
 * A look of an element (laid out in a state of the page) seen as the element
 * is: hidden only where the element is - a screen hidden as the page is
 * (visibility), shown by a change, is revealed in the page, not in its
 * states (a row of its list would be painted unseen in its other look)
 */
static void unhide(node_t* n, uint32_t depth)
{
    if (n->style != NULL)
        n->style->hidden = false;
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
        if (k->kind == NODE_ELEMENT)
            unhide(k, depth + 1U);
}

static void seen_like(node_t* look, const node_t* e, uint32_t depth)
{
    if (look == NULL || e == NULL || depth > 200U)
        return;
    if (look->style != NULL && e->style != NULL && !e->style->hidden)
        look->style->hidden = false;
    const node_t* g = e->first;
    for (node_t* k = look->first; k != NULL; k = k->next)
    {
        if (g != NULL && g->kind == k->kind)
        {
            seen_like(k, g, depth + 1U);
            g = g->next;
        }
        else if (k->kind == NODE_ELEMENT && look->style != NULL && !look->style->hidden)
            unhide(k, depth + 1U);                  /* (none to follow: seen as its parent is) */
    }
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
    for (uint8_t k = 1; k < n; k++)
        seen_like(looks[k].node, e, 0);
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


/* ---- The changes' handlers: made when every change is laid out ---- */

static change_t* change(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    for (uint32_t i = 0; i < sc->change_count; i++)
    {
        change_t* ch = &sc->changes[i];
        if (ch->element == e && ch->mod.kind == kind && strcmp(ch->mod.name, name) == 0 &&
            ((value == NULL && ch->mod.value == NULL) || (value != NULL && ch->mod.value != NULL && strcmp(ch->mod.value, value) == 0)))
            return ch;
    }
    if (sc->change_count >= MAX_MODS)
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

static void put(script_t* sc, uint8_t kind, dmvsi_var_t var, int32_t value)
{
    if (var == 0 || sc->action_count >= MAX_ACTIONS)
        return;
    dmvsi_action_t* a = &sc->actions[sc->action_count++];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->var = var;
    a->value = value;
}

/* A variable set, or animated (its transition) */
static void put_to(script_t* sc, dmvsi_var_t var, int32_t value, uint16_t ms, const int16_t* easing)
{
    put(sc, (ms > 0) ? DMVSI_ACT_ANIMATE : DMVSI_ACT_SET, var, value);
    if (ms > 0 && sc->action_count > 0 && sc->actions[sc->action_count - 1U].var == var)
    {
        sc->actions[sc->action_count - 1U].duration = ms;
        memcpy(sc->actions[sc->action_count - 1U].easing, easing, 4 * sizeof(int16_t));
    }
}

/* The actions of a change of one element: its variables to where the change puts it */
static void apply(script_t* sc, node_t* e, uint8_t kind, const char* name, const char* value)
{
    change_t* ch = change(sc, e, kind, name, value);
    if (ch == NULL)
        return;
    if (kind == MOD_CLASSES)
    {
        for (uint32_t i = 0; i < sc->lookvar_count; i++)
        {
            if (sc->lookvars[i].element == e)
                put(sc, DMVSI_ACT_SET, sc->lookvars[i].var, (int32_t)ch->look_index);
        }
    }
    else if (kind != MOD_STYLE)
    {
        classvar_t* cv = classvar(sc, e, name, false);
        if (cv != NULL)
            put(sc, DMVSI_ACT_SET, cv->var, (kind == MOD_CLASS_ADD) ? 1 : 0);
    }
    if (e->dynamic == NULL || ch->changed)
        return;
    int32_t ox, oy;
    view_origin(sc->c, &ox, &oy);
    dmvsi_rect_t r = group_rect(sc->c, e, ox, oy);
    if (e->dynamic->bind[DMVSI_BIND_X] != 0)
        put_to(sc, e->dynamic->bind[DMVSI_BIND_X], r.x + ch->dx, ch->move_ms, ch->move_easing);
    if (e->dynamic->bind[DMVSI_BIND_Y] != 0)
        put_to(sc, e->dynamic->bind[DMVSI_BIND_Y], r.y + ch->dy, ch->move_ms, ch->move_easing);
    if (e->dynamic->bind[DMVSI_BIND_OPACITY] != 0)
        put_to(sc, e->dynamic->bind[DMVSI_BIND_OPACITY], ch->opacity, ch->fade_ms, ch->fade_easing);
}

/* The changes of what variables hold: a change of each element they may hold */
static int expand_held(script_t* sc)
{
    uint32_t count = sc->held_count;
    for (uint32_t i = 0; i < count; i++)
    {
        held_t* h = &sc->held[i];
        uint32_t objects[MAX_DOMAIN];
        uint32_t n = dmvs_js_object_domain(sc->js, h->holder, objects, MAX_DOMAIN);
        if (n > MAX_DOMAIN)
        {
            WARN(sc->c, "script: a variable that may hold too many elements - not converted\n");
            n = MAX_DOMAIN;
        }
        sc->action_count = 0;
        for (uint32_t k = 0; k < n; k++)
        {
            const handle_t* o = handle_at(sc, objects[k]);
            if (o == NULL || o->kind != H_ELEMENT || o->element == NULL || sc->place_count >= MAX_PLACES ||
                sc->action_count + 3U > MAX_ACTIONS)
                continue;
            place_t* p = &sc->places[sc->place_count];
            if ((p->handler = dmvsi_new_handler(sc->c->doc)) == 0)
                return -ENOMEM;
            p->element = o->element;
            p->kind = h->kind;
            p->name = h->name;
            p->value = h->value;
            sc->place_count++;
            put(sc, DMVSI_ACT_IF_EQ, h->holder, (int32_t)objects[k]);
            memset(&sc->actions[sc->action_count], 0, sizeof(dmvsi_action_t));
            sc->actions[sc->action_count].kind = DMVSI_ACT_CALL;
            sc->actions[sc->action_count++].handler = p->handler;
            memset(&sc->actions[sc->action_count], 0, sizeof(dmvsi_action_t));
            sc->actions[sc->action_count++].kind = DMVSI_ACT_END;
        }
        h->actions = arena_alloc(&sc->c->arena, (sc->action_count + 1U) * sizeof(dmvsi_action_t));
        if (h->actions == NULL)
            return -ENOMEM;
        memcpy(h->actions, sc->actions, sc->action_count * sizeof(dmvsi_action_t));
        h->count = sc->action_count;
    }
    return 0;
}

/* ---- The page's scripts ---- */

typedef struct
{
    node_t*         element;            /* An onclick="...": its element; NULL: a <script> */
    dmvs_js_ast_t   ast;
} program_t;

typedef struct
{
    program_t       list[MAX_PROGRAMS];
    uint32_t        count;
} programs_t;

static void parse_into(script_t* sc, programs_t* out, node_t* element, const char* text, size_t length)
{
    dmvs_js_error_t error;
    if (out->count >= MAX_PROGRAMS)
    {
        WARN(sc->c, "script: too many scripts - the rest not converted\n");
        return;
    }
    add_chars(sc, text, length);
    dmvs_js_ast_t ast = dmvs_js_parse(text, length, &error);
    if (ast == NULL)
    {
        WARN(sc->c, "script %u:%u: %s - not converted\n", (unsigned)error.line, (unsigned)error.column, error.message);
        return;
    }
    out->list[out->count].element = element;
    out->list[out->count++].ast = ast;
}

/* The page's <script>s (inline JavaScript) and onclick="..." code, in document order */
static void find_scripts(script_t* sc, node_t* n, programs_t* out, uint32_t depth)
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
            if (js && node_attr(k, "src") == NULL && k->first != NULL && k->first->kind == NODE_TEXT)
                parse_into(sc, out, NULL, k->first->text, k->first->length);
            continue;
        }
        const char* onclick = node_attr(k, "onclick");
        if (onclick != NULL && k->pseudo == PSEUDO_NONE)
            parse_into(sc, out, k, onclick, strlen(onclick));
        find_scripts(sc, k, out, depth + 1U);
    }
}

/* What a script changes: laid out, the elements' variables and looks (as before scripts were compiled) */
static int lay_out_changes(script_t* sc)
{
    conv_t* c = sc->c;
    int status = 0;
    for (uint32_t i = 0; i < sc->place_count; i++)
    {
        const place_t* p = &sc->places[i];
        if (change(sc, p->element, p->kind, p->name, p->value) == NULL)
            WARN(c, "script: too many changes of elements - not converted\n");
    }
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
            WARN(c, "script: too many looks of #%s - not converted\n", label_of(ch->element));
    }
    for (uint32_t i = 0; i < sc->lookvar_count && status == 0; i++)
    {
        lookvar_t* lv = &sc->lookvars[i];
        node_t* e = lv->element;
        char name[48];
        Dmod_SnPrintf(name, sizeof(name), "%s_look", label_of(e));
        if ((lv->var = dmvsi_add_var(c->doc, name, 0)) == 0 || dynamic_of(sc, e) == NULL)
        {
            status = -ENOMEM;
            break;
        }
        variant_t looks[MAX_VARIANTS];
        uint8_t n = 0;
        looks[n++] = (variant_t){ e->box.placed ? e : NULL, lv->var, 0, -1 };
        for (uint32_t k = 0; k < lv->count; k++)
            looks[n++] = (variant_t){ lv->looks[k], lv->var, (int32_t)(k + 1U), -1 };
        for (uint8_t k = 1; k < n; k++)
            seen_like(looks[k].node, e, 0);
        memcpy(e->dynamic->variants, looks, n * sizeof(variant_t));
        e->dynamic->variant_count = n;
    }

    /* Another look of a class: the class's variable, asked by the looks */
    for (uint32_t i = 0; i < sc->change_count; i++)
    {
        change_t* ch = &sc->changes[i];
        if (ch->mod.kind == MOD_CLASSES)
            continue;
        if (ch->changed && ch->mod.kind != MOD_STYLE)
            (void)classvar(sc, ch->element, ch->mod.name, true);
        else if (ch->changed)
            WARN(c, "script: a style that changes how #%s looks - not converted\n", label_of(ch->element));
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
            status = make_variants(sc, ch->element, ch, c->has_active && listened(sc, ch->element));
    }
    for (uint32_t i = 0; i < sc->click_count && status == 0; i++)
    {
        node_t* e = sc->clicks[i].element;
        if (c->has_active && listened(sc, e) && (e->dynamic == NULL || e->dynamic->variant_count == 0))
            status = make_variants(sc, e, NULL, true);
    }
    return status;
}

/*
 * A call of a handler made, its actions in its place when they can be (no
 * RETURN, room): dmview's calls are few (8 deep) - the click's, a change's
 * are not one more
 */
static void put_call(script_t* sc, dmvsi_handler_t h)
{
    const dmvsi_action_t* a = NULL;
    uint32_t n = dmvsi_handler_actions(sc->c->doc, h, &a);
    bool inline_it = sc->action_count + n <= MAX_ACTIONS;
    for (uint32_t i = 0; i < n && inline_it; i++)
        inline_it = a[i].kind != DMVSI_ACT_RETURN;
    if (inline_it)
    {
        memcpy(&sc->actions[sc->action_count], a, n * sizeof(dmvsi_action_t));
        sc->action_count += n;
        return;
    }
    if (sc->action_count >= MAX_ACTIONS)
        return;
    memset(&sc->actions[sc->action_count], 0, sizeof(dmvsi_action_t));
    sc->actions[sc->action_count].kind = DMVSI_ACT_CALL;
    sc->actions[sc->action_count++].handler = h;
}

/* The handlers made at the end: the changes', the clicks' */
static int make_handlers(script_t* sc)
{
    for (uint32_t i = 0; i < sc->place_count; i++)
    {
        const place_t* p = &sc->places[i];
        sc->action_count = 0;
        apply(sc, p->element, p->kind, p->name, p->value);
        if (dmvsi_set_handler(sc->c->doc, p->handler, sc->actions, sc->action_count) != 0)
            return -EINVAL;
    }
    /* What variables hold: an IF for each element, its change in it */
    for (uint32_t i = 0; i < sc->held_count; i++)
    {
        held_t* h = &sc->held[i];
        sc->action_count = 0;
        for (uint32_t k = 0; k < h->count; k++)
        {
            if (h->actions[k].kind == DMVSI_ACT_CALL)
                put_call(sc, h->actions[k].handler);
            else if (sc->action_count < MAX_ACTIONS)
                sc->actions[sc->action_count++] = h->actions[k];
        }
        if (dmvsi_set_handler(sc->c->doc, h->handler, sc->actions, sc->action_count) != 0)
            return -EINVAL;
    }
    for (uint32_t i = 0; i < sc->click_count; i++)
    {
        click_t* k = &sc->clicks[i];
        uint32_t n = (k->onclick != 0) ? 1U : 0U;
        sc->action_count = 0;
        if (k->onclick != 0)
            put_call(sc, k->onclick);
        for (uint32_t j = 0; j < k->count; j++, n++)
            put_call(sc, k->calls[j]);
        if (dmvsi_set_handler(sc->c->doc, k->handler, sc->actions, sc->action_count) != 0)
            return -EINVAL;
        node_t* e = k->element;
        if (n > 0 && e->style != NULL && e->style->display != DISPLAY_NONE && dynamic_of(sc, e) != NULL)
            e->dynamic->click = k->handler;
    }
    return 0;
}

/* The elements' texts the scripts set: the characters they may show */
static void texts(script_t* sc, node_t* n, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->dynamic != NULL && k->dynamic->text != 0)
            k->dynamic->chars = chars_of(sc);
        texts(sc, k, depth + 1U);
    }
}

int script_compile(conv_t* c)
{
    script_t* sc = Dmod_Malloc(sizeof(*sc));
    programs_t* programs = Dmod_Malloc(sizeof(*programs));
    if (sc == NULL || programs == NULL)
    {
        Dmod_Free(sc);
        Dmod_Free(programs);
        return -ENOMEM;
    }
    memset(sc, 0, sizeof(*sc));
    memset(programs, 0, sizeof(*programs));
    sc->c = c;
    find_scripts(sc, c->document, programs, 0);
    int status = 0;
    if (programs->count == 0)
        goto done;

    dmvs_js_host_t host;
    memset(&host, 0, sizeof(host));
    host.ctx = sc;
    host.global = host_global;
    host.get = host_get;
    host.set = host_set;
    host.call = host_call;
    host.report = host_report;
    host.flush = host_flush;
    if ((sc->js = dmvs_js_compiler_new(c->doc, &host)) == NULL)
    {
        status = -ENOMEM;
        goto done;
    }
    /* The onclick code first seen (what it assigns is a variable), the scripts, then the onclick code */
    for (uint32_t i = 0; i < programs->count; i++)
    {
        if (programs->list[i].element != NULL)
            (void)dmvs_js_scan(sc->js, programs->list[i].ast);
    }
    for (uint32_t i = 0; i < programs->count && status == 0; i++)
    {
        if (programs->list[i].element == NULL)
            status = dmvs_js_compile(sc->js, programs->list[i].ast);
        else
            (void)dmvs_js_scan(sc->js, programs->list[i].ast);         /* The compiler's to free, too */
    }
    for (uint32_t i = 0; i < programs->count && status == 0; i++)
    {
        node_t* e = programs->list[i].element;
        if (e == NULL)
            continue;
        click_t* k = click_of(sc, e);
        if (k != NULL)
            k->onclick = dmvs_js_compile_handler(sc->js, programs->list[i].ast, handle(sc, H_ELEMENT, e, 0));
    }
    if (status == 0)
        status = dmvs_js_finish(sc->js);
    if (status == 0)
        status = expand_held(sc);
    if (status == 0)
        status = lay_out_changes(sc);
    if (status == 0)
        status = make_src_variants(sc);
    if (status == 0)
        status = make_handlers(sc);
    texts(sc, c->document, 0);
    dmvs_js_compiler_free(sc->js);

done:
    Dmod_Free(programs);
    Dmod_Free(sc);
    return (c->arena.failed && status == 0) ? -ENOMEM : status;
}
