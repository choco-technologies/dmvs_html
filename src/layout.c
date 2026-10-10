#include "private.h"
#include <string.h>

/*
 * Layout: the boxes of the elements, as CSS lays them out - what a UI page
 * needs of it:
 *
 *  - block flow: children one under another, margins between siblings
 *    collapsed, auto margins centering;
 *  - inline formatting: text broken into lines at spaces, inline elements,
 *    inline blocks; every line as high as its inline boxes' line heights
 *    around a common baseline, aligned by text-align;
 *  - flex: one line in a row or a column - flex-basis, grow, shrink, auto
 *    margins, justify-content, align-items / -self (stretch, baseline, ...);
 *  - grid: tracks of lengths, fr and auto, items placed row by row (with
 *    spans), rows as high as their items;
 *  - positioned: relative offsets, absolute and fixed boxes against their
 *    containing block (insets, shrink-to-fit widths, static positions),
 *    translate().
 *
 * Text is measured with the fonts the view will draw it in (dmvsi_font()),
 * so it fits the room made for it.
 *
 * The normal flow is laid out first, every box relative to the box that
 * placed it; then, top-down, the absolute positions - and the absolutely
 * positioned boxes, once their containing block is placed.
 */

#define MAX_ITEMS       4096u
#define MAX_FLEX        256u
#define MAX_DEPTH       200u

static int32_t max32(int32_t a, int32_t b) { return (a > b) ? a : b; }
static int32_t min32(int32_t a, int32_t b) { return (a < b) ? a : b; }

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool is_flex(const style_t* st) { return st->display == DISPLAY_FLEX || st->display == DISPLAY_INLINE_FLEX; }
static bool is_grid(const style_t* st) { return st->display == DISPLAY_GRID || st->display == DISPLAY_INLINE_GRID; }

static bool out_of_flow(const style_t* st)
{
    return st->position == POSITION_ABSOLUTE || st->position == POSITION_FIXED;
}

static bool block_level(const node_t* n)
{
    if (n->kind != NODE_ELEMENT || n->style == NULL)
        return false;
    uint8_t d = n->style->display;
    return d == DISPLAY_BLOCK || d == DISPLAY_FLEX || d == DISPLAY_GRID;
}

static bool displayed(const node_t* n)
{
    return n->kind == NODE_TEXT || (n->kind == NODE_ELEMENT && n->style != NULL && n->style->display != DISPLAY_NONE);
}

/* ---- Fonts ---- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* The face of `family` (lowercase) nearest to the weight, as CSS matches fonts */
static const font_face_t* match_face(conv_t* c, const char* family, uint16_t weight, bool italic)
{
    const font_face_t* best = NULL;
    int32_t best_score = INT32_MAX;
    for (const font_face_t* f = c->faces; f != NULL; f = f->next)
    {
        if (strcmp(f->family, family) != 0)
            continue;
        int32_t score;
        if (weight >= f->weight_min && weight <= f->weight_max)
            score = 0;
        else
        {
            int32_t below = (weight > f->weight_max) ? weight - f->weight_max : 0;
            int32_t above = (weight < f->weight_min) ? f->weight_min - weight : 0;
            /* Below 400 lighter first, above 500 heavier first, 400 ... 500: up to 500, then lighter */
            if (weight < 400)
                score = (below > 0) ? below : 1000 + above;
            else if (weight > 500)
                score = (above > 0) ? above : 1000 + below;
            else
                score = (above > 0 && f->weight_min <= 500) ? above : (below > 0) ? 500 + below : 1000 + above;
        }
        if (f->italic != italic)
            score += 5000;
        if (score < best_score)
        {
            best = f;
            best_score = score;
        }
    }
    return best;
}

dmvsi_font_t style_font(conv_t* c, style_t* st)
{
    if (st->font_chosen)
        return st->font;
    st->font_chosen = true;
    const char* path = NULL;
    for (const char* s = (st->font_family != NULL) ? st->font_family : ""; *s != '\0' && path == NULL; )
    {
        while (is_space(*s) || *s == ',')
            s++;
        char family[64];
        size_t n = 0;
        char q = (*s == '"' || *s == '\'') ? *s++ : '\0';
        while (*s != '\0' && (q != '\0' ? *s != q : *s != ','))
        {
            if (n + 1U < sizeof(family))
                family[n++] = lower(*s);
            s++;
        }
        if (q != '\0' && *s == q)
            s++;
        while (n > 0 && is_space(family[n - 1U]))
            n--;
        family[n] = '\0';
        const font_face_t* f = (n > 0) ? match_face(c, family, st->font_weight, st->italic) : NULL;
        if (f != NULL)
            path = f->path;
        while (*s != '\0' && *s != ',')
            s++;
    }
    int32_t size = (st->font_size + U / 2) / U;
    int32_t tracking = (st->letter_spacing * 100 + ((st->letter_spacing >= 0) ? U / 2 : -U / 2)) / U;
    int status = 0;
    st->font = dmvsi_font(c->doc, path, (uint16_t)max32(size, 1), tracking, &status);
    if (st->font == NULL && path != NULL)
    {
        if (c->font_warned == NULL || strcmp(c->font_warned, path) != 0)
            WARN(c, "cannot read the font %s (%d) - the built-in font instead\n", path, status);
        c->font_warned = path;
        st->font = dmvsi_font(c->doc, NULL, (uint16_t)max32(size, 1), tracking, NULL);
    }
    return st->font;
}

typedef struct
{
    int32_t     ascent, descent;        /* 1/64 px */
} metrics_t;

static metrics_t metrics(conv_t* c, style_t* st)
{
    metrics_t m = { 0, 0 };
    dmvsi_font_info_t info;
    dmvsi_font_t f = style_font(c, st);
    if (f != NULL && dmvsi_font_info(f, &info) == 0)
    {
        m.ascent = info.ascent * U;
        m.descent = info.descent * U;
    }
    return m;
}

static int32_t line_height(conv_t* c, style_t* st)
{
    switch (st->line_height_kind)
    {
        case LH_NUMBER:
            return (int32_t)((int64_t)st->font_size * st->line_height / 1000);
        case LH_LENGTH:
            return st->line_height;
        default:
        {
            metrics_t m = metrics(c, st);
            return m.ascent + m.descent;
        }
    }
}

int32_t text_baseline(conv_t* c, style_t* st)
{
    metrics_t m = metrics(c, st);
    return (line_height(c, st) - (m.ascent + m.descent)) / 2 + m.ascent;
}

/* ---- Text ---- */

static uint32_t to_upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32U;
    if ((c >= 0xE0u && c <= 0xFEu && c != 0xF7u))
        return c - 32U;
    if ((c >= 0x100u && c <= 0x137u) || (c >= 0x14Au && c <= 0x177u))
        return c & ~1u;
    if ((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu))
        return ((c & 1u) == 0) ? c - 1U : c;
    return c;
}

static uint32_t to_lower(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32U;
    if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)
        return c + 32U;
    if ((c >= 0x100u && c <= 0x137u) || (c >= 0x14Au && c <= 0x177u))
        return c | 1u;
    if ((c >= 0x139u && c <= 0x148u) || (c >= 0x179u && c <= 0x17Eu))
        return ((c & 1u) != 0) ? c + 1U : c;
    return c;
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

/* The text of a node as it is drawn: text-transform applied (into the arena) */
static char* transformed(conv_t* c, const char* s, size_t length, uint8_t mode, bool* word_start)
{
    char* out = arena_alloc(&c->arena, length * 2U + 1U);
    size_t n = 0;
    if (out == NULL)
        return NULL;
    const char* end = s + length;
    while (s < end)
    {
        uint32_t cp = dmvsi_utf8_next(&s, end);
        if (mode == CASE_UPPER || (mode == CASE_CAPITALIZE && *word_start))
            cp = to_upper(cp);
        else if (mode == CASE_LOWER)
            cp = to_lower(cp);
        *word_start = cp == ' ' || cp == '\n' || cp == '\t';
        n += put_utf8(out + n, cp);
    }
    out[n] = '\0';
    return out;
}

/* ---- Inline formatting ---- */

#define IT_WORD         0u
#define IT_SPACE        1u
#define IT_OPEN         2u              /* An inline element starts: its left margin, border, padding */
#define IT_CLOSE        3u              /* ... ends: its right ones */
#define IT_ATOMIC       4u              /* An inline block, an image */
#define IT_BREAK        5u              /* <br>, a newline that is kept */
#define IT_ABSOLUTE     6u              /* An absolutely positioned box: only its static position */

typedef struct
{
    uint8_t     kind;
    node_t*     node;                   /* Text: the text node; else the element */
    style_t*    style;                  /* Text: of its parent */
    const char* text;
    size_t      length;
    int32_t     w;
    bool        opportunity;            /* A line may break after it (a space) */
} item_t;

typedef struct
{
    conv_t*     c;
    item_t*     items;
    uint32_t    count;
    bool        space_before;           /* The last item was a collapsible space */
} items_t;

static void layout_box(conv_t* c, node_t* n, int32_t w, int32_t forced_h, int32_t cb_w, int32_t cb_h);
static int32_t intrinsic(conv_t* c, node_t* n, bool max_content);
static void resolve_edges(node_t* n, int32_t cb_w);

static item_t* add_item(items_t* it, uint8_t kind, node_t* node, style_t* st)
{
    if (it->count >= MAX_ITEMS)
        return NULL;
    item_t* i = &it->items[it->count++];
    memset(i, 0, sizeof(*i));
    i->kind = kind;
    i->node = node;
    i->style = st;
    return i;
}

static int32_t text_width(conv_t* c, style_t* st, const char* s, size_t n)
{
    dmvsi_font_t f = style_font(c, st);
    return (f != NULL) ? dmvsi_text_width(f, s, n) * U : 0;
}

/* The words and spaces of a text node */
static void text_items(items_t* it, node_t* t)
{
    conv_t* c = it->c;
    style_t* st = t->style;
    bool word_start = !it->space_before;
    char* text = transformed(c, t->text, t->length, st->text_transform, &word_start);
    if (text == NULL)
        return;
    bool keep_spaces = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
    bool keep_lines = keep_spaces || st->white_space == WS_PRE_LINE;
    bool wraps = st->white_space != WS_NOWRAP && st->white_space != WS_PRE;
    const char* s = text;
    while (*s != '\0')
    {
        if (*s == '\n' && keep_lines)
        {
            (void)add_item(it, IT_BREAK, t, st);
            it->space_before = !keep_spaces;
            s++;
            continue;
        }
        if (is_space(*s))
        {
            const char* start = s;
            if (keep_spaces)
            {
                while (*s == ' ' || *s == '\t')
                    s++;
            }
            else
            {
                while (is_space(*s) && !(*s == '\n' && keep_lines))
                    s++;
                if (it->space_before)
                    continue;           /* Collapsed into the one before */
                start = " ";
            }
            item_t* i = add_item(it, IT_SPACE, t, st);
            if (i != NULL)
            {
                i->text = start;
                i->length = keep_spaces ? (size_t)(s - start) : 1U;
                i->w = text_width(c, st, i->text, i->length);
                i->opportunity = wraps;
            }
            it->space_before = !keep_spaces;
            continue;
        }
        const char* start = s;
        while (*s != '\0' && !is_space(*s))
            s++;
        item_t* i = add_item(it, IT_WORD, t, st);
        if (i != NULL)
        {
            i->text = start;
            i->length = (size_t)(s - start);
            i->w = text_width(c, st, start, i->length);
        }
        it->space_before = false;
    }
}

/* An atomic inline: its width (laid out shrink-to-fit in `avail`) */
static void atomic_item(items_t* it, node_t* n, int32_t avail, int32_t cb_h, bool measure, bool max_content)
{
    item_t* i = add_item(it, IT_ATOMIC, n, n->style);
    if (i == NULL)
        return;
    resolve_edges(n, avail);
    int32_t margins = n->box.m[1] + n->box.m[3];
    it->space_before = false;           /* A space after it is kept */
    if (measure)
    {
        i->w = intrinsic(it->c, n, max_content) + margins;
        return;
    }
    int32_t w;
    if (!len_is_auto(n->style->width))
    {
        w = len_resolve(n->style->width, avail);
        if (!n->style->border_box)
            w += n->box.p[1] + n->box.p[3] + n->box.b[1] + n->box.b[3];
    }
    else
        w = min32(max32(intrinsic(it->c, n, false), avail - margins), intrinsic(it->c, n, true));
    layout_box(it->c, n, max32(w, 0), -1, avail, cb_h);
    i->w = n->box.w + margins;
    it->space_before = false;
}

static void inline_items(items_t* it, node_t* parent, int32_t avail, int32_t cb_h, bool measure, bool max_content, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return;
    for (node_t* n = parent->first; n != NULL; n = n->next)
    {
        if (!displayed(n))
            continue;
        if (n->kind == NODE_TEXT)
        {
            text_items(it, n);
            continue;
        }
        style_t* st = n->style;
        if (out_of_flow(st))
        {
            if (!measure)
                (void)add_item(it, IT_ABSOLUTE, n, st);
            continue;
        }
        if (node_is(n, "br"))
        {
            (void)add_item(it, IT_BREAK, n, st);
            it->space_before = true;
            continue;
        }
        if (st->display == DISPLAY_INLINE && !node_is(n, "img"))
        {
            resolve_edges(n, avail);
            item_t* open = add_item(it, IT_OPEN, n, st);
            if (open != NULL)
                open->w = n->box.m[3] + n->box.b[3] + n->box.p[3];
            inline_items(it, n, avail, cb_h, measure, max_content, depth + 1U);
            item_t* close = add_item(it, IT_CLOSE, n, st);
            if (close != NULL)
                close->w = n->box.p[1] + n->box.b[1] + n->box.m[1];
            continue;
        }
        atomic_item(it, n, avail, cb_h, measure, max_content);
    }
}

/* Inline boxes open at an item: the stack of inline elements around it */
#define MAX_NESTED      16u

typedef struct
{
    node_t*     open[MAX_NESTED];
    int32_t     shift[MAX_NESTED];      /* Of their baselines, down */
    int32_t     start_x[MAX_NESTED];
    uint32_t    depth;
} nesting_t;

/* An inline box's baseline shift from its parent's (vertical-align) */
static int32_t valign_shift(conv_t* c, style_t* st, style_t* parent)
{
    switch (st->valign)
    {
        case VALIGN_LENGTH:
            return -st->valign_by;
        case VALIGN_MIDDLE:
        {
            metrics_t m = metrics(c, st), pm = metrics(c, parent);
            return (m.ascent - m.descent) / 2 - pm.ascent / 4;   /* Its middle on the parent's half x-height */
        }
        default:
            return 0;
    }
}

/* The part of a line above (top < 0) and below the baseline of an inline box */
static void inline_extent(conv_t* c, style_t* st, int32_t shift, int32_t* top, int32_t* bottom)
{
    metrics_t m = metrics(c, st);
    int32_t lh = line_height(c, st);
    int32_t half = (lh - (m.ascent + m.descent)) / 2;
    int32_t t = shift - m.ascent - half;
    *top = min32(*top, t);
    *bottom = max32(*bottom, t + lh);
}

static frag_t* add_frag(conv_t* c, node_t* block, uint8_t kind)
{
    frag_t* f = arena_alloc(&c->arena, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->kind = kind;
    if (block->box.last_frag != NULL)
        block->box.last_frag->next = f;
    else
        block->box.frags = f;
    block->box.last_frag = f;
    return f;
}

static bool decorated(const style_t* st)
{
    return (st->background >> 24) != 0 || st->background_image != NULL || st->background_url != NULL ||
           st->border_w[0] > 0 || st->border_w[1] > 0 || st->border_w[2] > 0 || st->border_w[3] > 0 || st->shadow_count > 0;
}

/* One line of items [from, to) at y: placed, returns its height */
static int32_t place_line(conv_t* c, node_t* block, item_t* items, uint32_t from, uint32_t to, int32_t x0, int32_t y,
                          int32_t avail, nesting_t* nest, bool* first_baseline_set)
{
    style_t* bst = block->style;
    /* Spaces at the start and the end of a line do not count */
    while (from < to && items[from].kind == IT_SPACE && items[from].opportunity)
        from++;
    uint32_t end = to;
    while (end > from && items[end - 1U].kind == IT_SPACE && items[end - 1U].opportunity)
        end--;
    int32_t width = 0;
    for (uint32_t i = from; i < end; i++)
        width += items[i].w;

    /* Heights around the baseline: the block's strut, the inline boxes, the atomics */
    int32_t top = 0, bottom = 0;
    inline_extent(c, bst, 0, &top, &bottom);
    nesting_t n = *nest;
    for (uint32_t k = 0; k < n.depth; k++)
        inline_extent(c, n.open[k]->style, n.shift[k], &top, &bottom);
    for (uint32_t i = from; i < end; i++)
    {
        item_t* it = &items[i];
        int32_t shift = (n.depth > 0) ? n.shift[n.depth - 1U] : 0;
        if (it->kind == IT_OPEN && n.depth < MAX_NESTED)
        {
            style_t* parent = (n.depth > 0) ? n.open[n.depth - 1U]->style : bst;
            shift += valign_shift(c, it->style, parent);
            n.open[n.depth] = it->node;
            n.shift[n.depth] = shift;
            n.depth++;
            inline_extent(c, it->style, shift, &top, &bottom);
        }
        else if (it->kind == IT_CLOSE && n.depth > 0)
            n.depth--;
        else if (it->kind == IT_ATOMIC)
        {
            node_t* a = it->node;
            int32_t mt = a->box.m[0], mb = a->box.m[2];
            int32_t base = (a->box.baseline != AUTO_SIZE && a->style->overflow_y == OVERFLOW_VISIBLE) ? mt + a->box.baseline
                         : mt + a->box.h + mb;
            style_t* parent = (n.depth > 0) ? n.open[n.depth - 1U]->style : bst;
            int32_t s = shift + valign_shift(c, a->style, parent);
            if (a->style->valign == VALIGN_MIDDLE)
            {
                metrics_t pm = metrics(c, parent);
                base = mt + a->box.h / 2 + pm.ascent / 4;
                s = shift;
            }
            top = min32(top, s - base);
            bottom = max32(bottom, s - base + mt + a->box.h + mb);
        }
    }
    int32_t baseline = y - top;
    if (!*first_baseline_set)
    {
        block->box.baseline = baseline;
        *first_baseline_set = true;
    }

    int32_t x = x0;
    if (bst->text_align == TEXT_CENTER)
        x += (avail - width) / 2;
    else if (bst->text_align == TEXT_RIGHT)
        x += avail - width;

    /* The items: text runs merged per text node, atomics placed */
    frag_t* run = NULL;
    for (uint32_t k = 0; k < nest->depth; k++)
        nest->start_x[k] = x;
    for (uint32_t i = from; i < to; i++)
    {
        item_t* it = &items[i];
        int32_t shift = (nest->depth > 0) ? nest->shift[nest->depth - 1U] : 0;
        bool trailing = i >= end;
        switch (it->kind)
        {
            case IT_WORD:
            case IT_SPACE:
                if (trailing)
                    break;
                if (run != NULL && run->node == it->node && run->y == baseline + shift)
                {
                    /* Appended: the run's text grows */
                    char* t = arena_alloc(&c->arena, run->length + it->length + 1U);
                    if (t != NULL)
                    {
                        memcpy(t, run->text, run->length);
                        memcpy(t + run->length, it->text, it->length);
                        run->length += it->length;
                        t[run->length] = '\0';
                        run->text = t;
                        run->w += it->w;
                    }
                }
                else if (it->kind == IT_WORD || run != NULL)
                {
                    run = add_frag(c, block, FRAG_TEXT);
                    if (run != NULL)
                    {
                        run->node = it->node;
                        run->style = it->style;
                        run->font = style_font(c, it->style);
                        run->x = x;
                        run->y = baseline + shift;
                        run->w = it->w;
                        run->text = arena_strndup(&c->arena, it->text, it->length);
                        run->length = it->length;
                    }
                }
                x += it->w;
                break;
            case IT_OPEN:
                if (nest->depth < MAX_NESTED)
                {
                    style_t* parent = (nest->depth > 0) ? nest->open[nest->depth - 1U]->style : bst;
                    nest->open[nest->depth] = it->node;
                    nest->shift[nest->depth] = shift + valign_shift(c, it->style, parent);
                    nest->start_x[nest->depth] = x + it->node->box.m[3];
                    nest->depth++;
                }
                x += it->w;
                run = NULL;
                break;
            case IT_CLOSE:
                if (nest->depth > 0)
                {
                    nest->depth--;
                    node_t* e = nest->open[nest->depth];
                    if (decorated(e->style))
                    {
                        metrics_t m = metrics(c, e->style);
                        frag_t* f = add_frag(c, block, FRAG_BOX);
                        if (f != NULL)
                        {
                            f->node = e;
                            f->style = e->style;
                            f->x = nest->start_x[nest->depth];
                            f->w = x + it->w - e->box.m[1] - f->x;
                            f->y = baseline + nest->shift[nest->depth] - m.ascent - e->box.p[0] - e->box.b[0];
                            f->h = m.ascent + m.descent + e->box.p[0] + e->box.p[2] + e->box.b[0] + e->box.b[2];
                        }
                    }
                }
                x += it->w;
                run = NULL;
                break;
            case IT_ATOMIC:
            {
                node_t* a = it->node;
                int32_t mt = a->box.m[0];
                int32_t base = (a->box.baseline != AUTO_SIZE && a->style->overflow_y == OVERFLOW_VISIBLE) ? mt + a->box.baseline
                             : mt + a->box.h + a->box.m[2];
                int32_t s = shift + valign_shift(c, a->style, (nest->depth > 0) ? nest->open[nest->depth - 1U]->style : bst);
                if (a->style->valign == VALIGN_MIDDLE)
                {
                    metrics_t pm = metrics(c, (nest->depth > 0) ? nest->open[nest->depth - 1U]->style : bst);
                    base = mt + a->box.h / 2 + pm.ascent / 4;
                    s = shift;
                }
                a->box.x = x + a->box.m[3];
                a->box.y = baseline + s - base + mt;
                a->box.container = block;
                x += it->w;
                run = NULL;
                break;
            }
            case IT_ABSOLUTE:
                it->node->box.x = x;
                it->node->box.y = y;
                it->node->box.container = block;
                break;
            default:
                break;
        }
    }
    /* Inline boxes that go on: their part of this line */
    for (uint32_t k = 0; k < nest->depth; k++)
    {
        node_t* e = nest->open[k];
        if (!decorated(e->style))
            continue;
        metrics_t m = metrics(c, e->style);
        frag_t* f = add_frag(c, block, FRAG_BOX);
        if (f != NULL)
        {
            f->node = e;
            f->style = e->style;
            f->x = nest->start_x[k];
            f->w = x - f->x;
            f->y = baseline + nest->shift[k] - m.ascent - e->box.p[0] - e->box.b[0];
            f->h = m.ascent + m.descent + e->box.p[0] + e->box.p[2] + e->box.b[0] + e->box.b[2];
        }
    }
    return bottom - top;
}

/* The inline children of `block` as lines at (x0, y0), `avail` wide; returns their height */
static int32_t layout_inline(conv_t* c, node_t* block, node_t* first, node_t* stop, int32_t x0, int32_t y0, int32_t avail,
                             int32_t cb_h, bool* first_baseline_set)
{
    item_t* storage = arena_alloc(&c->arena, MAX_ITEMS * sizeof(item_t));
    items_t it = { c, storage, 0, true };
    if (storage == NULL)
        return 0;
    /* The children from `first` up to `stop`, as if they were all of a parent */
    node_t holder;
    memset(&holder, 0, sizeof(holder));
    holder.first = first;
    for (node_t* n = first; n != NULL && n != stop; n = n->next)
    {
        node_t* next = n->next;
        node_t* saved = n->next;
        if (next == stop)
            n->next = NULL;
        holder.last = n;
        if (next == stop)
        {
            inline_items(&it, &holder, avail, cb_h, false, false, 0);
            n->next = saved;
            break;
        }
        if (next == NULL)
            inline_items(&it, &holder, avail, cb_h, false, false, 0);
    }

    /* Nothing but collapsible spaces: no lines */
    bool content = false;
    for (uint32_t i = 0; i < it.count && !content; i++)
        content = it.items[i].kind != IT_SPACE && it.items[i].kind != IT_ABSOLUTE;
    if (!content)
    {
        for (uint32_t i = 0; i < it.count; i++)
        {
            if (it.items[i].kind == IT_ABSOLUTE)
            {
                it.items[i].node->box.x = x0;
                it.items[i].node->box.y = y0;
                it.items[i].node->box.container = block;
            }
        }
        return 0;
    }

    nesting_t nest;
    memset(&nest, 0, sizeof(nest));
    int32_t y = y0;
    uint32_t start = 0;
    while (start < it.count)
    {
        /* A line: as many items as fit, broken after the last space that does */
        int32_t w = 0;
        uint32_t i = start, last_break = UINT32_MAX;
        bool any = false;
        for (; i < it.count; i++)
        {
            item_t* x = &it.items[i];
            if (x->kind == IT_BREAK)
                break;
            if (x->kind == IT_SPACE && !any)
            {
                if (x->opportunity)
                    continue;           /* At the start of a line: dropped */
            }
            if ((x->kind == IT_WORD || x->kind == IT_ATOMIC) && any && w + x->w > avail && last_break != UINT32_MAX)
            {
                i = last_break + 1U;
                break;
            }
            w += x->w;
            if (x->kind == IT_WORD || x->kind == IT_ATOMIC)
                any = true;
            if (x->opportunity && any)
                last_break = i;
            /* An atomic inline is a break opportunity before and after, in normal wrapping */
            if (x->kind == IT_ATOMIC && block->style->white_space != WS_NOWRAP && block->style->white_space != WS_PRE)
                last_break = i;
        }
        uint32_t end = i;
        y += place_line(c, block, it.items, start, end, x0, y, avail, &nest, first_baseline_set);
        start = (end < it.count && it.items[end].kind == IT_BREAK) ? end + 1U : end;
        if (end == it.count)
            break;
    }
    return y - y0;
}

/* ---- Boxes ---- */

static void resolve_edges(node_t* n, int32_t cb_w)
{
    style_t* st = n->style;
    for (int i = 0; i < 4; i++)
    {
        n->box.m[i] = len_is_auto(st->margin[i]) ? 0 : len_resolve(st->margin[i], cb_w);
        n->box.p[i] = max32(len_resolve(st->padding[i], cb_w), 0);
        n->box.b[i] = max32(st->border_w[i], 0);
    }
}

static int32_t edges_x(const node_t* n) { return n->box.p[1] + n->box.p[3] + n->box.b[1] + n->box.b[3]; }
static int32_t edges_y(const node_t* n) { return n->box.p[0] + n->box.p[2] + n->box.b[0] + n->box.b[2]; }

/* A size property as a border box size; AUTO_SIZE when it is auto (or a % of an indefinite size) */
static int32_t size_of(const node_t* n, len_t l, int32_t base, int32_t edges)
{
    if (l.kind != LEN_SET || (l.pct != 0 && base == AUTO_SIZE))
        return AUTO_SIZE;
    int32_t v = len_resolve(l, (base == AUTO_SIZE) ? 0 : base);
    return n->style->border_box ? max32(v, edges) : v + edges;
}

static int32_t clamp_width(const node_t* n, int32_t w, int32_t cb_w)
{
    int32_t e = edges_x(n);
    int32_t mx = (n->style->max_w.kind == LEN_SET) ? size_of(n, n->style->max_w, cb_w, e) : AUTO_SIZE;
    int32_t mn = (n->style->min_w.kind == LEN_SET) ? size_of(n, n->style->min_w, cb_w, e) : AUTO_SIZE;
    if (mx != AUTO_SIZE)
        w = min32(w, mx);
    if (mn != AUTO_SIZE)
        w = max32(w, mn);
    return max32(w, e);
}

static int32_t clamp_height(const node_t* n, int32_t h, int32_t cb_h)
{
    int32_t e = edges_y(n);
    int32_t mx = (n->style->max_h.kind == LEN_SET) ? size_of(n, n->style->max_h, cb_h, e) : AUTO_SIZE;
    int32_t mn = (n->style->min_h.kind == LEN_SET) ? size_of(n, n->style->min_h, cb_h, e) : AUTO_SIZE;
    if (mx != AUTO_SIZE)
        h = min32(h, mx);
    if (mn != AUTO_SIZE)
        h = max32(h, mn);
    return max32(h, e);
}

/* <img>: its size from its file (or its width / height attributes); <svg>: from its attributes */
static bool replaced_size(conv_t* c, node_t* n, int32_t* w, int32_t* h)
{
    if (node_is(n, "svg"))
        return svg_size(n, w, h);
    if (!node_is(n, "img"))
        return false;
    int32_t iw = 0, ih = 0;
    const char* src = node_attr(n, "src");
    char* path = (src != NULL) ? resolve_resource(c, c->path, src, strlen(src)) : NULL;
    if (path == NULL || !image_size(path, &iw, &ih))
        iw = ih = 0;
    const char* aw = node_attr(n, "width");
    const char* ah = node_attr(n, "height");
    int32_t vw = 0, vh = 0;
    for (const char* s = aw; s != NULL && *s >= '0' && *s <= '9'; s++)
        vw = vw * 10 + (*s - '0');
    for (const char* s = ah; s != NULL && *s >= '0' && *s <= '9'; s++)
        vh = vh * 10 + (*s - '0');
    if (vw > 0 && vh == 0 && iw > 0)
        vh = vw * ih / iw;
    if (vh > 0 && vw == 0 && ih > 0)
        vw = vh * iw / ih;
    *w = ((vw > 0) ? vw : iw) * U;
    *h = ((vh > 0) ? vh : ih) * U;
    return true;
}

/* ---- Intrinsic sizes ---- */

static int32_t intrinsic_items(conv_t* c, node_t* n, bool max_content)
{
    item_t* storage = arena_alloc(&c->arena, MAX_ITEMS * sizeof(item_t));
    items_t it = { c, storage, 0, true };
    if (storage == NULL)
        return 0;
    inline_items(&it, n, 0, AUTO_SIZE, true, max_content, 0);
    int32_t best = 0, line = 0, word = 0;
    for (uint32_t i = 0; i < it.count; i++)
    {
        item_t* x = &it.items[i];
        if (x->kind == IT_BREAK)
        {
            best = max32(best, line);
            line = 0;
            word = 0;
            continue;
        }
        if (max_content)
            line += x->w;
        else if (x->opportunity)
        {
            best = max32(best, word);
            word = 0;
        }
        else
            word += x->w;
        if (!max_content && x->kind == IT_ATOMIC)
        {
            best = max32(best, word);
            word = 0;
        }
    }
    return max32(best, max_content ? line : word);
}

static int32_t intrinsic(conv_t* c, node_t* n, bool max_content)
{
    if (n->kind != NODE_ELEMENT || n->style == NULL)
        return 0;
    int32_t* cache = max_content ? &n->box.max_content : &n->box.min_content;
    if (*cache != AUTO_SIZE && *cache != 0)
        return *cache;
    style_t* st = n->style;
    resolve_edges(n, 0);
    int32_t e = edges_x(n);
    int32_t fixed = size_of(n, st->width, AUTO_SIZE, e);
    if (fixed != AUTO_SIZE)
    {
        *cache = fixed;
        return fixed;
    }
    int32_t content = 0, rw, rh;
    if (replaced_size(c, n, &rw, &rh))
        content = rw;
    else if (is_flex(st) || is_grid(st))
    {
        bool row = is_flex(st) && (st->flex_direction == FLEX_ROW || st->flex_direction == FLEX_ROW_REVERSE);
        int32_t gap = (st->column_gap.kind == LEN_SET) ? len_resolve(st->column_gap, 0) : 0;
        uint32_t count = 0;
        int32_t widest = 0;
        for (node_t* k = n->first; k != NULL; k = k->next)
        {
            if (!displayed(k) || k->kind != NODE_ELEMENT || out_of_flow(k->style))
                continue;
            int32_t w = intrinsic(c, k, max_content);
            resolve_edges(k, 0);
            w += k->box.m[1] + k->box.m[3];
            if (row)
                content += w;
            widest = max32(widest, w);
            count++;
        }
        if (is_grid(st))
        {
            uint32_t cols = (st->column_count > 0) ? st->column_count : 1U;
            content = (int32_t)cols * widest + (int32_t)(cols - 1U) * gap;
        }
        else if (row)
            content += (count > 1U) ? (int32_t)(count - 1U) * gap : 0;
        else
            content = widest;
    }
    else
    {
        bool blocks = false;
        for (node_t* k = n->first; k != NULL && !blocks; k = k->next)
            blocks = displayed(k) && block_level(k) && !out_of_flow(k->style);
        if (blocks)
        {
            /* The widest child; an inline run among them as one more */
            for (node_t* k = n->first; k != NULL; k = k->next)
            {
                if (!displayed(k) || (k->kind == NODE_ELEMENT && out_of_flow(k->style)))
                    continue;
                if (block_level(k))
                {
                    int32_t w = intrinsic(c, k, max_content);
                    resolve_edges(k, 0);
                    content = max32(content, w + k->box.m[1] + k->box.m[3]);
                }
            }
            content = max32(content, intrinsic_items(c, n, max_content));
        }
        else
            content = intrinsic_items(c, n, max_content);
    }
    resolve_edges(n, 0);
    int32_t w = clamp_width(n, content + e, AUTO_SIZE);
    *cache = (w == 0) ? 1 : w;          /* 0 marks "not yet" */
    return w;
}

/* ---- Block flow ---- */

static int32_t collapse(int32_t a, int32_t b)
{
    if (a >= 0 && b >= 0)
        return max32(a, b);
    if (a < 0 && b < 0)
        return min32(a, b);
    return a + b;
}

/* The children of n (block flow) from content position (x0, y0); returns the content height */
static int32_t layout_blocks(conv_t* c, node_t* n, int32_t x0, int32_t y0, int32_t cw, int32_t ch, bool* first_baseline_set)
{
    int32_t y = 0, prev_margin = 0;
    bool any = false;
    node_t* k = n->first;
    while (k != NULL)
    {
        if (!displayed(k))
        {
            k = k->next;
            continue;
        }
        if (!block_level(k))
        {
            /* An inline run up to the next block: lines of an anonymous block */
            node_t* stop = k;
            while (stop != NULL && !(displayed(stop) && block_level(stop)))
                stop = stop->next;
            int32_t h = layout_inline(c, n, k, stop, x0, y0 + y + prev_margin, cw, ch, first_baseline_set);
            if (h > 0)
            {
                y += prev_margin + h;
                prev_margin = 0;
                any = true;
            }
            k = stop;
            continue;
        }
        if (out_of_flow(k->style))
        {
            k->box.x = x0;
            k->box.y = y0 + y + prev_margin;
            k->box.container = n;
            k->box.out_of_flow = true;
            k = k->next;
            continue;
        }
        resolve_edges(k, cw);
        int32_t w = size_of(k, k->style->width, cw, edges_x(k));
        bool auto_l = len_is_auto(k->style->margin[3]), auto_r = len_is_auto(k->style->margin[1]);
        int32_t rw, rh;
        if (w == AUTO_SIZE && replaced_size(c, k, &rw, &rh))
            w = rw + edges_x(k);
        if (w == AUTO_SIZE)
            w = cw - k->box.m[1] - k->box.m[3];
        w = clamp_width(k, w, cw);
        layout_box(c, k, w, -1, cw, ch);
        int32_t free = cw - w - k->box.m[1] - k->box.m[3];
        int32_t x = x0 + k->box.m[3];
        if (auto_l && auto_r)
            x = x0 + max32(free, 0) / 2;
        else if (auto_l)
            x = x0 + free + k->box.m[3];
        int32_t gap = any ? collapse(prev_margin, k->box.m[0]) : k->box.m[0];
        k->box.x = x;
        k->box.y = y0 + y + gap;
        k->box.container = n;
        if (!*first_baseline_set && k->box.baseline != AUTO_SIZE)
        {
            n->box.baseline = k->box.y + k->box.baseline;
            *first_baseline_set = true;
        }
        y += gap + k->box.h;
        prev_margin = k->box.m[2];
        any = true;
        k = k->next;
    }
    return y + prev_margin;
}

/* ---- Flex ---- */

typedef struct
{
    node_t*     node;
    int32_t     base;                   /* Flex base size (border box, main axis) */
    int32_t     target;
    int32_t     min, max;
    int32_t     margin_main;            /* Both margins along the main axis */
    bool        frozen;
} flex_item_t;

static uint8_t self_align(const style_t* item, const style_t* container)
{
    uint8_t a = (item->align_self == ALIGN_AUTO || item->align_self == ALIGN_NORMAL) ? container->align_items : item->align_self;
    return (a == ALIGN_NORMAL || a == ALIGN_AUTO) ? ALIGN_STRETCH : a;
}

static int32_t layout_flex(conv_t* c, node_t* n, int32_t x0, int32_t y0, int32_t cw, int32_t ch)
{
    style_t* st = n->style;
    bool row = st->flex_direction == FLEX_ROW || st->flex_direction == FLEX_ROW_REVERSE;
    bool reverse = st->flex_direction == FLEX_ROW_REVERSE || st->flex_direction == FLEX_COLUMN_REVERSE;
    int32_t main_avail = row ? cw : ch;
    len_t gap_len = row ? st->column_gap : st->row_gap;
    int32_t gap = (gap_len.kind == LEN_SET) ? len_resolve(gap_len, max32(main_avail, 0)) : 0;
    flex_item_t* items = arena_alloc(&c->arena, MAX_FLEX * sizeof(flex_item_t));
    uint32_t count = 0;
    if (items == NULL)
        return 0;

    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (!displayed(k) || k->kind != NODE_ELEMENT)
            continue;
        if (out_of_flow(k->style))
        {
            k->box.x = x0;
            k->box.y = y0;
            k->box.container = n;
            k->box.out_of_flow = true;
            continue;
        }
        if (count < MAX_FLEX)
            items[count++].node = k;
    }
    /* `order`, stably */
    for (uint32_t i = 1; i < count; i++)
    {
        flex_item_t x = items[i];
        uint32_t j = i;
        while (j > 0 && items[j - 1U].node->style->order > x.node->style->order)
        {
            items[j] = items[j - 1U];
            j--;
        }
        items[j] = x;
    }

    /* Base sizes */
    int32_t sum = 0, grow = 0;
    int64_t shrink = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        flex_item_t* f = &items[i];
        node_t* k = f->node;
        style_t* ks = k->style;
        resolve_edges(k, cw);
        f->margin_main = row ? k->box.m[1] + k->box.m[3] : k->box.m[0] + k->box.m[2];
        int32_t e = row ? edges_x(k) : edges_y(k);
        int32_t main_size = size_of(k, row ? ks->width : ks->height, main_avail, e);
        int32_t basis = (ks->basis.kind == LEN_SET && !(ks->basis.pct != 0 && main_avail == AUTO_SIZE))
                      ? len_resolve(ks->basis, (main_avail == AUTO_SIZE) ? 0 : main_avail) : AUTO_SIZE;
        if (basis != AUTO_SIZE && !ks->border_box)
            basis += e;
        if (basis == AUTO_SIZE)
            basis = main_size;
        if (basis == AUTO_SIZE)
        {
            if (row)
            {
                int32_t rw, rh;
                basis = replaced_size(c, k, &rw, &rh) ? rw + e : intrinsic(c, k, true);
            }
            else
            {
                /* Its height at its cross size */
                int32_t w = size_of(k, ks->width, cw, edges_x(k));
                if (w == AUTO_SIZE)
                    w = (self_align(ks, st) == ALIGN_STRETCH) ? cw - k->box.m[1] - k->box.m[3]
                      : min32(max32(intrinsic(c, k, false), cw - k->box.m[1] - k->box.m[3]), intrinsic(c, k, true));
                layout_box(c, k, clamp_width(k, w, cw), -1, cw, ch);
                basis = k->box.h;
            }
        }
        f->base = max32(basis, e);
        /* min-width / -height: auto is the content's size, unless it scrolls */
        len_t min_len = row ? ks->min_w : ks->min_h;
        len_t max_len = row ? ks->max_w : ks->max_h;
        uint8_t overflow = row ? ks->overflow_x : ks->overflow_y;
        if (min_len.kind == LEN_SET)
            f->min = size_of(k, min_len, main_avail, e);
        else if (overflow != OVERFLOW_VISIBLE)
            f->min = e;
        else if (row)
            f->min = (main_size != AUTO_SIZE) ? min32(main_size, intrinsic(c, k, false)) : intrinsic(c, k, false);
        else
            f->min = min32(f->base, (main_size != AUTO_SIZE) ? main_size : f->base);
        if (f->min == AUTO_SIZE)
            f->min = e;
        f->max = (max_len.kind == LEN_SET) ? size_of(k, max_len, main_avail, e) : INT32_MAX;
        if (f->max == AUTO_SIZE)
            f->max = INT32_MAX;
        f->target = min32(max32(f->base, f->min), f->max);
        sum += f->target + f->margin_main;
        grow += ks->grow;
        shrink += (int64_t)ks->shrink * f->base;
    }
    sum += (count > 1U) ? (int32_t)(count - 1U) * gap : 0;

    /* Growing, shrinking (one round, then clamped) */
    if (main_avail != AUTO_SIZE && count > 0)
    {
        int32_t free = main_avail - sum;
        if (free > 0 && grow > 0)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                flex_item_t* f = &items[i];
                int32_t g = f->node->style->grow;
                if (g > 0)
                    f->target = min32(f->target + (int32_t)((int64_t)free * g / grow), f->max);
            }
        }
        else if (free < 0 && shrink > 0)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                flex_item_t* f = &items[i];
                int64_t weight = (int64_t)f->node->style->shrink * f->base;
                f->target = max32(f->target + (int32_t)((int64_t)free * weight / shrink), f->min);
            }
        }
    }

    /* Sizes: main from flexing, cross from content or stretched */
    int32_t line_cross = 0;
    bool definite_cross = row ? ch != AUTO_SIZE : true;
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            flex_item_t* f = &items[i];
            node_t* k = f->node;
            style_t* ks = k->style;
            uint8_t align = self_align(ks, st);
            if (row)
            {
                bool stretch = align == ALIGN_STRETCH && len_is_auto(ks->height) && !len_is_auto(ks->margin[0]) == true &&
                               !len_is_auto(ks->margin[2]);
                int32_t forced = -1;
                int32_t h = size_of(k, ks->height, ch, edges_y(k));
                if (h != AUTO_SIZE)
                    forced = clamp_height(k, h, ch);
                else if (stretch && pass == 1)
                    forced = clamp_height(k, line_cross - k->box.m[0] - k->box.m[2], ch);
                layout_box(c, k, f->target, forced, cw, ch);
                if (pass == 0)
                    line_cross = max32(line_cross, k->box.h + k->box.m[0] + k->box.m[2]);
            }
            else
            {
                int32_t w = size_of(k, ks->width, cw, edges_x(k));
                if (w == AUTO_SIZE)
                    w = (align == ALIGN_STRETCH && !len_is_auto(ks->margin[1]) && !len_is_auto(ks->margin[3]))
                      ? cw - k->box.m[1] - k->box.m[3]
                      : min32(max32(intrinsic(c, k, false), cw - k->box.m[1] - k->box.m[3]), intrinsic(c, k, true));
                layout_box(c, k, clamp_width(k, w, cw), f->target, cw, ch);
                line_cross = cw;
            }
        }
        if (row && pass == 0 && definite_cross)
            line_cross = ch;
        if (!row)
            break;
    }

    /* Main axis: auto margins, then justify-content */
    int32_t used = 0;
    uint32_t auto_margins = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        node_t* k = items[i].node;
        used += (row ? k->box.w : k->box.h) + items[i].margin_main;
        auto_margins += row ? (len_is_auto(k->style->margin[3]) + len_is_auto(k->style->margin[1]))
                            : (len_is_auto(k->style->margin[0]) + len_is_auto(k->style->margin[2]));
    }
    used += (count > 1U) ? (int32_t)(count - 1U) * gap : 0;
    int32_t free = (main_avail != AUTO_SIZE) ? main_avail - used : 0;
    int32_t offset = 0, between = gap, auto_share = 0;
    if (auto_margins > 0)
        auto_share = max32(free, 0) / (int32_t)auto_margins;
    else
    {
        switch (st->justify_content)
        {
            case ALIGN_END: offset = free; break;
            case ALIGN_CENTER: offset = free / 2; break;
            case ALIGN_BETWEEN: if (count > 1U && free > 0) between += free / (int32_t)(count - 1U); break;
            case ALIGN_AROUND: if (count > 0 && free > 0) { offset = free / (int32_t)(2U * count); between += free / (int32_t)count; } break;
            case ALIGN_EVENLY: if (free > 0) { offset = free / (int32_t)(count + 1U); between += offset; } break;
            default: break;
        }
        if (reverse && (st->justify_content == ALIGN_START || st->justify_content == ALIGN_NORMAL))
            offset = free;
    }

    /* Baselines, for align baseline (rows) */
    int32_t max_base = 0;
    for (uint32_t i = 0; i < count && row; i++)
    {
        node_t* k = items[i].node;
        if (self_align(k->style, st) == ALIGN_BASELINE)
            max_base = max32(max_base, k->box.m[0] + ((k->box.baseline != AUTO_SIZE) ? k->box.baseline : k->box.h));
    }

    int32_t pos = offset;
    bool baseline_set = false;
    for (uint32_t idx = 0; idx < count; idx++)
    {
        uint32_t i = reverse ? count - 1U - idx : idx;
        node_t* k = items[i].node;
        style_t* ks = k->style;
        int32_t m_start = row ? k->box.m[3] : k->box.m[0];
        int32_t m_end = row ? k->box.m[1] : k->box.m[2];
        if (row ? len_is_auto(ks->margin[3]) : len_is_auto(ks->margin[0]))
            m_start += auto_share;
        if (row ? len_is_auto(ks->margin[1]) : len_is_auto(ks->margin[2]))
            m_end += auto_share;
        int32_t main_pos = pos + m_start;
        pos += m_start + (row ? k->box.w : k->box.h) + m_end + between;

        /* Cross axis */
        uint8_t align = self_align(ks, st);
        int32_t size = row ? k->box.h : k->box.w;
        int32_t cm_start = row ? k->box.m[0] : k->box.m[3];
        int32_t cm_end = row ? k->box.m[2] : k->box.m[1];
        int32_t cross_pos = cm_start;
        bool auto_start = row ? len_is_auto(ks->margin[0]) : len_is_auto(ks->margin[3]);
        bool auto_end = row ? len_is_auto(ks->margin[2]) : len_is_auto(ks->margin[1]);
        int32_t room = line_cross - size - cm_start - cm_end;
        if (auto_start && auto_end)
            cross_pos += room / 2;
        else if (auto_start)
            cross_pos += room;
        else if (!auto_end)
        {
            if (align == ALIGN_END)
                cross_pos += room;
            else if (align == ALIGN_CENTER)
                cross_pos += room / 2;
            else if (align == ALIGN_BASELINE && row)
                cross_pos = max_base - ((k->box.baseline != AUTO_SIZE) ? k->box.baseline : k->box.h);
        }
        k->box.x = x0 + (row ? main_pos : cross_pos);
        k->box.y = y0 + (row ? cross_pos : main_pos);
        k->box.container = n;
        if (!baseline_set && k->box.baseline != AUTO_SIZE)
        {
            n->box.baseline = k->box.y + k->box.baseline;
            baseline_set = true;
        }
    }
    if (row)
        return (ch != AUTO_SIZE) ? ch : line_cross;
    return (ch != AUTO_SIZE) ? ch : used;
}

/* ---- Grid ---- */

static int32_t layout_grid(conv_t* c, node_t* n, int32_t x0, int32_t y0, int32_t cw, int32_t ch)
{
    style_t* st = n->style;
    uint32_t cols = (st->column_count > 0) ? st->column_count : 1U;
    int32_t col_gap = (st->column_gap.kind == LEN_SET) ? len_resolve(st->column_gap, cw) : 0;
    int32_t row_gap = (st->row_gap.kind == LEN_SET) ? len_resolve(st->row_gap, max32(ch, 0)) : 0;
    int32_t widths[MAX_TRACKS];
    int32_t fixed = col_gap * (int32_t)(cols - 1U), fr = 0;
    uint32_t autos = 0;

    node_t* items[MAX_FLEX];
    uint32_t count = 0;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (!displayed(k) || k->kind != NODE_ELEMENT)
            continue;
        if (out_of_flow(k->style))
        {
            k->box.x = x0;
            k->box.y = y0;
            k->box.container = n;
            k->box.out_of_flow = true;
            continue;
        }
        if (count < MAX_FLEX)
            items[count++] = k;
    }

    for (uint32_t i = 0; i < cols; i++)
    {
        const track_t* t = (st->column_count > 0) ? &st->columns[i] : NULL;
        widths[i] = 0;
        if (t == NULL || t->kind == TRACK_AUTO)
        {
            /* As wide as its widest item */
            for (uint32_t k = i; k < count; k += cols)
            {
                resolve_edges(items[k], cw);
                widths[i] = max32(widths[i], intrinsic(c, items[k], true) + items[k]->box.m[1] + items[k]->box.m[3]);
            }
            autos++;
            fixed += widths[i];
        }
        else if (t->kind == TRACK_FR)
            fr += t->fr;
        else
        {
            widths[i] = len_resolve(t->length, cw);
            fixed += widths[i];
        }
    }
    int32_t left = max32(cw - fixed, 0);
    for (uint32_t i = 0; i < cols; i++)
    {
        const track_t* t = (st->column_count > 0) ? &st->columns[i] : NULL;
        if (t != NULL && t->kind == TRACK_FR && fr > 0)
            widths[i] = (int32_t)((int64_t)left * t->fr / fr);
        else if ((t == NULL || t->kind == TRACK_AUTO) && fr == 0 && autos > 0)
            widths[i] += left / (int32_t)autos;     /* Auto tracks take what is left (justify-content: normal) */
    }

    /* Placement, row by row, and the rows' heights */
    uint32_t place_row[MAX_FLEX], place_col[MAX_FLEX], spans[MAX_FLEX];
    uint32_t r = 0, col = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t span = items[i]->style->column_span;
        span = (span == 255U || span > cols) ? cols : (span == 0U) ? 1U : span;
        if (col + span > cols)
        {
            r++;
            col = 0;
        }
        place_row[i] = r;
        place_col[i] = col;
        spans[i] = span;
        col += span;
        if (col >= cols)
        {
            r++;
            col = 0;
        }
    }
    uint32_t rows = (count == 0) ? 0 : place_row[count - 1U] + 1U;
    int32_t heights[MAX_FLEX];
    memset(heights, 0, sizeof(heights));
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            node_t* k = items[i];
            style_t* ks = k->style;
            int32_t area = -col_gap;
            for (uint32_t s = 0; s < spans[i]; s++)
                area += widths[place_col[i] + s] + col_gap;
            resolve_edges(k, cw);
            int32_t w = size_of(k, ks->width, area, edges_x(k));
            uint8_t js = (ks->justify_self == ALIGN_AUTO || ks->justify_self == ALIGN_NORMAL) ? st->justify_items : ks->justify_self;
            if (w == AUTO_SIZE)
                w = (js == ALIGN_STRETCH || js == ALIGN_NORMAL || js == ALIGN_AUTO) ? area - k->box.m[1] - k->box.m[3]
                  : min32(max32(intrinsic(c, k, false), area - k->box.m[1] - k->box.m[3]), intrinsic(c, k, true));
            int32_t forced = -1;
            int32_t h = size_of(k, ks->height, AUTO_SIZE, edges_y(k));
            uint8_t align = self_align(ks, st);
            if (h != AUTO_SIZE)
                forced = h;
            else if (pass == 1 && align == ALIGN_STRETCH)
                forced = heights[place_row[i]] - k->box.m[0] - k->box.m[2];
            layout_box(c, k, clamp_width(k, w, area), forced, area, AUTO_SIZE);
            if (pass == 0)
                heights[place_row[i]] = max32(heights[place_row[i]], k->box.h + k->box.m[0] + k->box.m[2]);
        }
        if (pass == 0 && ch != AUTO_SIZE && rows > 0 &&
            (st->align_content == ALIGN_NORMAL || st->align_content == ALIGN_STRETCH))
        {
            /* Auto rows share what is left of a definite height */
            int32_t total = row_gap * (int32_t)(rows - 1U);
            for (uint32_t i = 0; i < rows; i++)
                total += heights[i];
            int32_t extra = ch - total;
            for (uint32_t i = 0; extra > 0 && i < rows; i++)
                heights[i] += extra / (int32_t)rows;
        }
    }

    int32_t total = (rows > 0) ? row_gap * (int32_t)(rows - 1U) : 0;
    for (uint32_t i = 0; i < rows; i++)
        total += heights[i];
    int32_t y_offset = 0;
    if (ch != AUTO_SIZE && st->align_content == ALIGN_CENTER)
        y_offset = (ch - total) / 2;
    else if (ch != AUTO_SIZE && st->align_content == ALIGN_END)
        y_offset = ch - total;
    bool baseline_set = false;
    for (uint32_t i = 0; i < count; i++)
    {
        node_t* k = items[i];
        int32_t x = 0, y = y_offset;
        for (uint32_t s = 0; s < place_col[i]; s++)
            x += widths[s] + col_gap;
        for (uint32_t s = 0; s < place_row[i]; s++)
            y += heights[s] + row_gap;
        int32_t area = -col_gap;
        for (uint32_t s = 0; s < spans[i]; s++)
            area += widths[place_col[i] + s] + col_gap;
        uint8_t align = self_align(k->style, st);
        int32_t room = heights[place_row[i]] - k->box.h - k->box.m[0] - k->box.m[2];
        if (align == ALIGN_CENTER)
            y += room / 2;
        else if (align == ALIGN_END)
            y += room;
        uint8_t js = (k->style->justify_self == ALIGN_AUTO || k->style->justify_self == ALIGN_NORMAL) ? st->justify_items : k->style->justify_self;
        int32_t xroom = area - k->box.w - k->box.m[1] - k->box.m[3];
        if (js == ALIGN_CENTER)
            x += xroom / 2;
        else if (js == ALIGN_END)
            x += xroom;
        k->box.x = x0 + x + k->box.m[3];
        k->box.y = y0 + y + k->box.m[0];
        k->box.container = n;
        if (!baseline_set && k->box.baseline != AUTO_SIZE)
        {
            n->box.baseline = k->box.y + k->box.baseline;
            baseline_set = true;
        }
    }
    return (ch != AUTO_SIZE) ? ch : total;
}

/* ---- A box ---- */

/* n's content laid out in a border box `w` wide; its height from it, or `forced_h` (>= 0) */
static void layout_box(conv_t* c, node_t* n, int32_t w, int32_t forced_h, int32_t cb_w, int32_t cb_h)
{
    style_t* st = n->style;
    resolve_edges(n, cb_w);
    n->box.w = w;
    n->box.frags = NULL;
    n->box.last_frag = NULL;
    n->box.baseline = AUTO_SIZE;
    n->box.laid_out = true;
    int32_t x0 = n->box.b[3] + n->box.p[3], y0 = n->box.b[0] + n->box.p[0];
    int32_t cw = max32(w - edges_x(n), 0);
    int32_t height = (forced_h >= 0) ? forced_h : size_of(n, st->height, cb_h, edges_y(n));
    if (height != AUTO_SIZE && forced_h < 0)
        height = clamp_height(n, height, cb_h);
    int32_t ch = (height != AUTO_SIZE) ? max32(height - edges_y(n), 0) : AUTO_SIZE;

    int32_t content_h = 0, rw, rh;
    bool baseline_set = false;
    if (replaced_size(c, n, &rw, &rh))
        content_h = (rw > 0) ? (int32_t)((int64_t)rh * cw / rw) : rh;     /* Its aspect kept */
    else if (is_flex(st))
        content_h = layout_flex(c, n, x0, y0, cw, ch);
    else if (is_grid(st))
        content_h = layout_grid(c, n, x0, y0, cw, ch);
    else
    {
        bool blocks = false;
        for (node_t* k = n->first; k != NULL && !blocks; k = k->next)
            blocks = displayed(k) && block_level(k);
        content_h = blocks ? layout_blocks(c, n, x0, y0, cw, ch, &baseline_set)
                           : layout_inline(c, n, n->first, NULL, x0, y0, cw, ch, &baseline_set);
    }
    n->box.content_w = cw;
    n->box.content_h = content_h;
    n->box.h = (height != AUTO_SIZE) ? height : clamp_height(n, content_h + edges_y(n), cb_h);
}

/* ---- Positions ---- */

static node_t* containing_block(node_t* n, bool fixed)
{
    for (node_t* a = n->parent; a != NULL && a->kind == NODE_ELEMENT; a = a->parent)
    {
        if (a->pseudo == PSEUDO_ANONYMOUS)
            continue;
        if (!fixed && a->style != NULL && a->style->position != POSITION_STATIC)
            return a;
        if (a->style != NULL && (a->style->translate_x.px != 0 || a->style->translate_x.pct != 0 ||
                                 a->style->translate_y.px != 0 || a->style->translate_y.pct != 0))
            return a;           /* A transformed box contains even fixed ones */
    }
    return NULL;                /* The viewport */
}

/* An absolutely positioned box: sized and placed in its containing block */
static void layout_absolute(conv_t* c, node_t* n)
{
    style_t* st = n->style;
    node_t* cb = containing_block(n, st->position == POSITION_FIXED);
    int32_t cbx = 0, cby = 0, cbw = c->vw, cbh = c->vh;
    if (cb != NULL)
    {
        cbx = cb->box.ax + cb->box.b[3];
        cby = cb->box.ay + cb->box.b[0];
        cbw = cb->box.w - cb->box.b[1] - cb->box.b[3];
        cbh = cb->box.h - cb->box.b[0] - cb->box.b[2];
    }
    resolve_edges(n, cbw);
    int32_t L = len_is_auto(st->inset[3]) ? AUTO_SIZE : len_resolve(st->inset[3], cbw);
    int32_t R = len_is_auto(st->inset[1]) ? AUTO_SIZE : len_resolve(st->inset[1], cbw);
    int32_t T = len_is_auto(st->inset[0]) ? AUTO_SIZE : len_resolve(st->inset[0], cbh);
    int32_t B = len_is_auto(st->inset[2]) ? AUTO_SIZE : len_resolve(st->inset[2], cbh);
    int32_t mx = n->box.m[1] + n->box.m[3], my = n->box.m[0] + n->box.m[2];

    int32_t w = size_of(n, st->width, cbw, edges_x(n)), rw, rh;
    bool replaced = replaced_size(c, n, &rw, &rh);
    if (w == AUTO_SIZE && replaced)
        w = rw + edges_x(n);
    if (w == AUTO_SIZE && L != AUTO_SIZE && R != AUTO_SIZE)
        w = cbw - L - R - mx;
    if (w == AUTO_SIZE)
    {
        int32_t avail = cbw - ((L != AUTO_SIZE) ? L : 0) - ((R != AUTO_SIZE) ? R : 0) - mx;
        w = min32(max32(intrinsic(c, n, false), avail), intrinsic(c, n, true));
    }
    w = clamp_width(n, w, cbw);
    int32_t h = size_of(n, st->height, cbh, edges_y(n));
    if (h == AUTO_SIZE && T != AUTO_SIZE && B != AUTO_SIZE && !replaced)
        h = cbh - T - B - my;
    if (h != AUTO_SIZE)
        h = clamp_height(n, h, cbh);
    layout_box(c, n, w, (h != AUTO_SIZE) ? h : -1, cbw, cbh);

    /* Its static position, where the flow would have put it - in a flex container, as
     * its only item, aligned by justify-content and align-self (or align-items) */
    node_t* fc = n->box.container;
    if (fc != NULL && fc == n->parent && fc->style != NULL &&
        (fc->style->display == DISPLAY_FLEX || fc->style->display == DISPLAY_INLINE_FLEX))
    {
        const style_t* fs = fc->style;
        bool row = fs->flex_direction == FLEX_ROW || fs->flex_direction == FLEX_ROW_REVERSE;
        bool reverse = fs->flex_direction == FLEX_ROW_REVERSE || fs->flex_direction == FLEX_COLUMN_REVERSE;
        int32_t free_x = fc->box.w - fc->box.b[1] - fc->box.b[3] - fc->box.p[1] - fc->box.p[3] - n->box.w - mx;
        int32_t free_y = fc->box.h - fc->box.b[0] - fc->box.b[2] - fc->box.p[0] - fc->box.p[2] - n->box.h - my;
        uint8_t justify = fs->justify_content;
        uint8_t cross = (st->align_self != ALIGN_AUTO) ? st->align_self : fs->align_items;
        int32_t main_free = row ? free_x : free_y, cross_free = row ? free_y : free_x;
        int32_t main_off = 0, cross_off = 0;
        if (justify == ALIGN_CENTER || justify == ALIGN_AROUND || justify == ALIGN_EVENLY)
            main_off = main_free / 2;
        else if ((justify == ALIGN_END) != reverse)
            main_off = main_free;
        if (cross == ALIGN_CENTER)
            cross_off = cross_free / 2;
        else if (cross == ALIGN_END)
            cross_off = cross_free;
        n->box.x = fc->box.b[3] + fc->box.p[3] + (row ? main_off : cross_off);
        n->box.y = fc->box.b[0] + fc->box.p[0] + (row ? cross_off : main_off);
    }
    int32_t sx = n->box.x, sy = n->box.y;
    if (n->box.container != NULL)
    {
        sx += n->box.container->box.ax;
        sy += n->box.container->box.ay;
    }
    int32_t x, y;
    if (L != AUTO_SIZE)
        x = cbx + L + n->box.m[3];
    else if (R != AUTO_SIZE)
        x = cbx + cbw - R - n->box.m[1] - n->box.w;
    else
        x = sx + n->box.m[3];
    if (L != AUTO_SIZE && R != AUTO_SIZE && len_is_auto(st->margin[3]) && len_is_auto(st->margin[1]))
        x = cbx + L + (cbw - L - R - n->box.w) / 2;
    if (T != AUTO_SIZE)
        y = cby + T + n->box.m[0];
    else if (B != AUTO_SIZE)
        y = cby + cbh - B - n->box.m[2] - n->box.h;
    else
        y = sy + n->box.m[0];
    n->box.ax = x;
    n->box.ay = y;
}

static void place(conv_t* c, node_t* n, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT || !displayed(k))
            continue;
        style_t* st = k->style;
        if (out_of_flow(st))
            layout_absolute(c, k);
        else if (k->box.laid_out)
        {
            node_t* cont = k->box.container;
            k->box.ax = k->box.x + ((cont != NULL) ? cont->box.ax : 0);
            k->box.ay = k->box.y + ((cont != NULL) ? cont->box.ay : 0);
            if (st->position == POSITION_RELATIVE)
            {
                int32_t cw = (cont != NULL) ? cont->box.content_w : c->vw;
                int32_t ch = (cont != NULL) ? cont->box.content_h : c->vh;
                if (!len_is_auto(st->inset[3]))
                    k->box.ax += len_resolve(st->inset[3], cw);
                else if (!len_is_auto(st->inset[1]))
                    k->box.ax -= len_resolve(st->inset[1], cw);
                if (!len_is_auto(st->inset[0]))
                    k->box.ay += len_resolve(st->inset[0], ch);
                else if (!len_is_auto(st->inset[2]))
                    k->box.ay -= len_resolve(st->inset[2], ch);
            }
        }
        else
        {
            place(c, k, depth + 1U);    /* Inline: its text is its block's - but not its inline blocks */
            continue;
        }
        k->box.ax += len_resolve(st->translate_x, k->box.w);
        k->box.ay += len_resolve(st->translate_y, k->box.h);
        k->box.placed = true;
        place(c, k, depth + 1U);
    }
}

/* Every element's intrinsic sizes not known yet */
static void reset(node_t* n, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        k->box.min_content = 0;
        k->box.max_content = 0;
        k->box.baseline = AUTO_SIZE;
        reset(k, depth + 1U);
    }
}

void layout_page(conv_t* c, node_t* document)
{
    reset(document, 0);
    for (node_t* html = document->first; html != NULL; html = html->next)
    {
        if (html->kind != NODE_ELEMENT || !displayed(html))
            continue;
        resolve_edges(html, c->vw);
        int32_t w = size_of(html, html->style->width, c->vw, edges_x(html));
        if (w == AUTO_SIZE)
            w = c->vw - html->box.m[1] - html->box.m[3];
        layout_box(c, html, clamp_width(html, w, c->vw), -1, c->vw, c->vh);
        html->box.x = html->box.m[3];
        html->box.y = html->box.m[0];
        html->box.ax = html->box.x;
        html->box.ay = html->box.y;
        html->box.placed = true;
        place(c, html, 0);
    }
}
