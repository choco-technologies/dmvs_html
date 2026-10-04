#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Painting: the laid out boxes into the document, in CSS's painting order -
 * per stacking context its own background and border, the positioned boxes
 * of negative z-index, the normal flow (each box's background, then its
 * lines of text), the positioned boxes of z-index auto / 0 in tree order,
 * the positive ones.
 *
 * A box that clips (overflow) is a group with a clip (that scrolls when its
 * content is larger); a translucent one a group with an opacity. A box that
 * clips holds the positioned boxes inside it, so they are clipped with it.
 *
 * Effects:
 *  - box-shadow: shadows of the box (outer under it, inset inside it);
 *  - filter: blur() - the box is its blurred shadow (a glow);
 *  - filter: drop-shadow() - everything of the box once more, first, in the
 *    shadow's color, moved by its offset (text not blurred, boxes blurred).
 */

#define MAX_LAYERS      256u
#define MAX_DEPTH       200u

typedef struct
{
    conv_t*     c;
    int32_t     ox, oy;                 /* The view's origin on the page */
    bool        shadow;                 /* Painting a drop-shadow */
    int32_t     dx, dy;                 /* Its offset */
    uint32_t    shadow_color;
    int32_t     sigma;
    const style_t* canvas;              /* Its background is the canvas's: not painted again */
    int         status;
} painter_t;

static int32_t max32(int32_t a, int32_t b) { return (a > b) ? a : b; }
static int32_t min32(int32_t a, int32_t b) { return (a < b) ? a : b; }

static bool displayed(const node_t* n)
{
    return n->kind == NODE_ELEMENT && n->style != NULL && n->style->display != DISPLAY_NONE;
}

static bool positioned(const style_t* st) { return st->position != POSITION_STATIC; }

static bool clips(const style_t* st)
{
    return st->overflow_x != OVERFLOW_VISIBLE || st->overflow_y != OVERFLOW_VISIBLE;
}

static bool transformed(const style_t* st)
{
    return st->translate_x.px != 0 || st->translate_x.pct != 0 || st->translate_y.px != 0 || st->translate_y.pct != 0;
}

/* A flex or grid item: its z-index counts as a positioned box's */
static bool item_of_flex_or_grid(const node_t* n)
{
    const style_t* p = (n->parent != NULL) ? n->parent->style : NULL;
    return p != NULL && (p->display == DISPLAY_FLEX || p->display == DISPLAY_INLINE_FLEX || p->display == DISPLAY_GRID ||
                         p->display == DISPLAY_INLINE_GRID);
}

/* Its z-index applies (and makes it a stacking context) */
static bool has_z(const node_t* n)
{
    return !n->style->z_auto && (positioned(n->style) || item_of_flex_or_grid(n));
}

/* It is painted as a layer of its stacking context */
static bool is_layer(const node_t* n)
{
    const style_t* st = n->style;
    return positioned(st) || has_z(n) || st->opacity < 255 || transformed(st) || st->blur > 0 || st->drop_count > 0;
}

static void check(painter_t* p, int status)
{
    if (status != 0 && p->status == 0)
        p->status = status;
}

static uint32_t times_alpha(uint32_t color, uint32_t alpha)
{
    uint32_t a = ((color >> 24) * alpha + 127U) / 255U;
    return (a << 24) | (color & 0x00FFFFFFu);
}

/* ---- Shapes ---- */

static dmvsi_rect_t border_box(const painter_t* p, const node_t* n)
{
    dmvsi_rect_t r = { n->box.ax - p->ox + p->dx, n->box.ay - p->oy + p->dy, n->box.w, n->box.h };
    return r;
}

static dmvsi_rect_t inner(const dmvsi_rect_t* r, const int32_t* b)
{
    dmvsi_rect_t i = { r->x + b[3], r->y + b[0], max32(r->w - b[1] - b[3], 0), max32(r->h - b[0] - b[2], 0) };
    return i;
}

/* One radius for the box: dmview rounds all of a box's corners the same - the largest */
static int32_t radius_of(const style_t* st, int32_t w, int32_t h)
{
    int32_t r = 0;
    for (int i = 0; i < 4; i++)
        r = max32(r, len_resolve(st->radius[i], w));
    return min32(r, min32(w, h) / 2);
}

/* A CSS gradient placed on a box w x h: its dmvsi paint */
static bool gradient_paint(const gradient_t* g, int32_t w, int32_t h, dmvsi_paint_t* out)
{
    memset(out, 0, sizeof(*out));
    out->kind = (g->kind == CSS_LINEAR) ? DMVSI_PAINT_LINEAR : DMVSI_PAINT_RADIAL;
    out->count = g->count;
    if (g->kind == CSS_LINEAR)
    {
        int32_t angle = g->angle;
        if (g->to != 0)
        {
            bool top = (g->to & TO_TOP) != 0, bottom = (g->to & TO_BOTTOM) != 0;
            bool left = (g->to & TO_LEFT) != 0, right = (g->to & TO_RIGHT) != 0;
            if ((top || bottom) && (left || right))
            {
                /* To a corner: perpendicular to the diagonal between the other two - atan(w / h) from the side */
                int32_t a = 0;
                if (h > 0)
                {
                    /* atan(w / h) in degrees by bisection on tan, without libm */
                    double t = (double)w / (double)h, lo = 0.0, hi = 90.0;
                    for (int k = 0; k < 40; k++)
                    {
                        double mid = (lo + hi) / 2.0, rad = mid * 3.14159265358979 / 180.0;
                        /* tan by its series of sin / cos */
                        double x2 = rad * rad;
                        double sn = rad * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
                        double cs = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56)));
                        if (sn / cs < t)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    a = (int32_t)(lo + 0.5);
                }
                else
                    a = 90;
                if (top && right) angle = 90 - a;
                else if (bottom && right) angle = 90 + a;
                else if (bottom && left) angle = 270 - a;
                else angle = 270 + a;
            }
            else
                angle = top ? 0 : right ? 90 : bottom ? 180 : 270;
        }
        out->angle = (int16_t)(((angle % 360) + 360) % 360);
    }
    else
    {
        /* Center and radii, in 1/100 % of the box */
        int32_t cx = len_resolve(g->cx, w), cy = len_resolve(g->cy, h);
        int32_t dx = max32(cx, w - cx), dy = max32(cy, h - cy);           /* farthest sides */
        int32_t sx = min32(cx, w - cx), sy = min32(cy, h - cy);           /* closest */
        int32_t rx, ry;
        switch (g->extent)
        {
            case 1: rx = sx; ry = sy; break;
            case 2: rx = dx; ry = dy; break;
            default:
                /* A corner: the ellipse through it with the sides' aspect (sqrt 2 x the sides) */
                rx = (g->extent == 3) ? sx : dx;
                ry = (g->extent == 3) ? sy : dy;
                rx = (int32_t)((int64_t)rx * 14142 / 10000);
                ry = (int32_t)((int64_t)ry * 14142 / 10000);
                break;
        }
        if (g->circle)
            rx = ry = max32(rx, ry);
        out->cx = (w > 0) ? (int32_t)((int64_t)cx * 10000 / w) : 5000;
        out->cy = (h > 0) ? (int32_t)((int64_t)cy * 10000 / h) : 5000;
        out->rx = (w > 0) ? max32((int32_t)((int64_t)rx * 10000 / w), 1) : 5000;
        out->ry = (h > 0) ? max32((int32_t)((int64_t)ry * 10000 / h), 1) : 5000;
    }

    /* Stops: left-out positions as CSS fills them in */
    int32_t pos[DMVSI_MAX_STOPS];
    for (uint32_t i = 0; i < g->count; i++)
        pos[i] = g->positions[i];
    if (pos[0] < 0)
        pos[0] = 0;
    if (pos[g->count - 1U] < 0)
        pos[g->count - 1U] = 10000;
    for (uint32_t i = 1; i < g->count; i++)
    {
        if (pos[i] >= 0)
        {
            pos[i] = max32(pos[i], pos[i - 1U]);
            continue;
        }
        uint32_t e = i;
        while (pos[e] < 0)
            e++;
        for (uint32_t k = i; k < e; k++)
            pos[k] = pos[i - 1U] + (pos[e] - pos[i - 1U]) * (int32_t)(k - i + 1U) / (int32_t)(e - i + 1U);
    }
    /* CSS interpolates premultiplied, dmview each channel on its own: where the
     * alpha changes, stops in between, as CSS has the colors there */
    uint32_t n = 0;
    uint32_t alpha_steps = 0;
    for (uint32_t i = 1; i < g->count; i++)
        alpha_steps += ((g->colors[i] >> 24) != (g->colors[i - 1U] >> 24)) ? 1U : 0U;
    uint32_t split = (alpha_steps > 0) ? (DMVSI_MAX_STOPS - g->count) / alpha_steps + 1U : 1U;
    for (uint32_t i = 0; i < g->count && n < DMVSI_MAX_STOPS; i++)
    {
        if (i > 0 && split > 1U && (g->colors[i] >> 24) != (g->colors[i - 1U] >> 24))
        {
            uint32_t a = g->colors[i - 1U], b = g->colors[i];
            for (uint32_t k = 1; k < split && n + 1U < DMVSI_MAX_STOPS; k++)
            {
                int32_t t = (int32_t)(k * 1000U / split);        /* 1/1000 of the way */
                int32_t aa = (int32_t)(a >> 24), ba = (int32_t)(b >> 24);
                int32_t alpha = aa + (ba - aa) * t / 1000;
                uint32_t color = (uint32_t)min32(max32(alpha, 0), 255) << 24;
                for (int sh = 0; sh < 24; sh += 8)
                {
                    int32_t ca = (int32_t)((a >> sh) & 0xFFu) * aa, cb = (int32_t)((b >> sh) & 0xFFu) * ba;
                    int32_t pm = ca + (cb - ca) * t / 1000;               /* Premultiplied */
                    int32_t c = (alpha > 0) ? pm / alpha : 0;
                    color |= (uint32_t)min32(max32(c, 0), 255) << sh;
                }
                out->stops[n].color = color;
                out->stops[n].position = (uint16_t)min32(max32(pos[i - 1U] + (pos[i] - pos[i - 1U]) * t / 1000, 0), 10000);
                n++;
            }
        }
        out->stops[n].color = g->colors[i];
        out->stops[n].position = (uint16_t)min32(max32(pos[i], 0), 10000);
        n++;
    }
    out->count = (uint8_t)n;
    return true;
}

static void fill(painter_t* p, dmvsi_rect_t r, int32_t radius, const dmvsi_paint_t* paint, const style_t* st)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    if (p->shadow || st->blur > 0)
    {
        /* A drop-shadow's copy, or a blurred box: a shadow of it */
        dmvsi_shadow_t s;
        memset(&s, 0, sizeof(s));
        s.shape = r;
        s.radius = radius;
        uint32_t color = (paint->kind == DMVSI_PAINT_COLOR) ? paint->color : paint->stops[0].color;
        s.color = p->shadow ? times_alpha(p->shadow_color, color >> 24) : color;
        s.sigma = p->shadow ? p->sigma : st->blur;
        check(p, dmvsi_add_shadow(p->c->doc, &s));
        return;
    }
    dmvsi_fill_t f;
    f.rect = r;
    f.radius = radius;
    f.paint = *paint;
    check(p, dmvsi_add_fill(p->c->doc, &f));
}

/* Background, shadows, border of a box at r */
static void paint_decoration(painter_t* p, const style_t* st, dmvsi_rect_t r, const int32_t* borders)
{
    if (st->hidden || r.w <= 0 || r.h <= 0)
        return;
    int32_t radius = radius_of(st, r.w, r.h);

    /* Outer shadows, the last one at the bottom */
    for (int i = (int)st->shadow_count - 1; i >= 0 && !p->shadow; i--)
    {
        const shadow_t* sh = &st->shadows[i];
        if (sh->inset)
            continue;
        dmvsi_shadow_t s;
        memset(&s, 0, sizeof(s));
        s.shape.x = r.x + sh->x - sh->spread;
        s.shape.y = r.y + sh->y - sh->spread;
        s.shape.w = r.w + 2 * sh->spread;
        s.shape.h = r.h + 2 * sh->spread;
        s.radius = max32(radius + sh->spread, 0);
        s.sigma = sh->blur / 2;
        s.color = sh->color;
        s.hole = r;
        s.hole_radius = radius;
        if (s.shape.w > 0 && s.shape.h > 0)
            check(p, dmvsi_add_shadow(p->c->doc, &s));
    }

    dmvsi_paint_t paint;
    memset(&paint, 0, sizeof(paint));
    /* The color is not seen beneath an opaque gradient: not painted */
    bool covered = false;
    if (st->background_image != NULL)
    {
        covered = true;
        for (uint32_t i = 0; i < st->background_image->count; i++)
            covered = covered && (st->background_image->colors[i] >> 24) == 0xFFu;
    }
    if ((st->background >> 24) != 0 && st != p->canvas && !covered)
    {
        paint.color = st->background;
        fill(p, r, radius, &paint, st);
    }
    if (st->background_image != NULL && st != p->canvas && gradient_paint(st->background_image, r.w, r.h, &paint))
        fill(p, r, radius, &paint, st);

    /* Inset shadows, inside the padding box */
    dmvsi_rect_t pad = inner(&r, borders);
    for (int i = (int)st->shadow_count - 1; i >= 0 && !p->shadow; i--)
    {
        const shadow_t* sh = &st->shadows[i];
        if (!sh->inset)
            continue;
        dmvsi_shadow_t s;
        memset(&s, 0, sizeof(s));
        s.shape.x = pad.x + sh->x + sh->spread;
        s.shape.y = pad.y + sh->y + sh->spread;
        s.shape.w = max32(pad.w - 2 * sh->spread, 0);
        s.shape.h = max32(pad.h - 2 * sh->spread, 0);
        s.radius = max32(radius - max32(borders[0], borders[3]) - sh->spread, 0);
        s.sigma = sh->blur / 2;
        s.color = sh->color;
        s.hole = pad;
        s.hole_radius = max32(radius - max32(borders[0], borders[3]), 0);
        s.flags = DMVSI_SHADOW_INSET;
        check(p, dmvsi_add_shadow(p->c->doc, &s));
    }

    /* The border: one outline when its sides are alike, else a rectangle per side */
    if (p->shadow || st->blur > 0)
        return;
    bool alike = true;
    for (int i = 1; i < 4; i++)
        alike = alike && borders[i] == borders[0] && st->border_color[i] == st->border_color[0];
    if (alike && borders[0] > 0 && (st->border_color[0] >> 24) != 0)
    {
        dmvsi_frame_t f;
        memset(&f, 0, sizeof(f));
        f.rect = r;
        f.radius = radius;
        f.width = borders[0];
        f.paint.color = st->border_color[0];
        check(p, dmvsi_add_frame(p->c->doc, &f));
        return;
    }
    if (alike)
        return;
    dmvsi_rect_t sides[4] = {
        { r.x, r.y, r.w, borders[0] }, { r.x + r.w - borders[1], r.y + borders[0], borders[1], r.h - borders[0] - borders[2] },
        { r.x, r.y + r.h - borders[2], r.w, borders[2] }, { r.x, r.y + borders[0], borders[3], r.h - borders[0] - borders[2] },
    };
    for (int i = 0; i < 4; i++)
    {
        if (borders[i] <= 0 || (st->border_color[i] >> 24) == 0)
            continue;
        dmvsi_fill_t f;
        memset(&f, 0, sizeof(f));
        f.rect = sides[i];
        f.paint.color = st->border_color[i];
        check(p, dmvsi_add_fill(p->c->doc, &f));
    }
}

/* The lines of text (and inline boxes) a block holds */
static void paint_lines(painter_t* p, const node_t* n)
{
    for (const frag_t* f = n->box.frags; f != NULL; f = f->next)
    {
        if (f->style->hidden)
            continue;
        if (f->kind == FRAG_BOX)
        {
            dmvsi_rect_t r = { n->box.ax - p->ox + p->dx + f->x, n->box.ay - p->oy + p->dy + f->y, f->w, f->h };
            paint_decoration(p, f->style, r, f->node->box.b);
            continue;
        }
        if (f->font == NULL || f->length == 0 || (f->style->color >> 24) == 0)
            continue;
        dmvsi_text_t t;
        memset(&t, 0, sizeof(t));
        t.x = n->box.ax - p->ox + p->dx + f->x;
        t.baseline = n->box.ay - p->oy + p->dy + f->y;
        t.text = f->text;
        t.length = f->length;
        t.font = f->font;
        t.paint.color = p->shadow ? times_alpha(p->shadow_color, f->style->color >> 24) : f->style->color;
        check(p, dmvsi_add_text(p->c->doc, &t));
    }
}

static void paint_image(painter_t* p, const node_t* n)
{
    const char* src = node_attr(n, "src");
    char* path = (src != NULL) ? resolve_resource(p->c, p->c->path, src, strlen(src)) : NULL;
    if (path == NULL || p->shadow || n->style->hidden)
        return;
    dmvsi_image_t im;
    memset(&im, 0, sizeof(im));
    dmvsi_rect_t r = border_box(p, n);
    dmvsi_rect_t b = inner(&r, n->box.b);
    im.rect = inner(&b, n->box.p);
    im.path = path;
    check(p, dmvsi_add_image(p->c->doc, &im));
}

/* ---- Stacking ---- */

typedef struct
{
    node_t*     node;
    int32_t     z;
    uint32_t    order;
} layer_t;

typedef struct
{
    layer_t     items[MAX_LAYERS];
    uint32_t    count;
} layers_t;

static void paint_context(painter_t* p, node_t* n, uint32_t depth);
static void paint_flow(painter_t* p, node_t* n, uint32_t depth);

/* The layers of a stacking context: positioned and other layer boxes in its flow */
static void collect_layers(node_t* n, layers_t* out, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (!displayed(k))
            continue;
        if (is_layer(k))
        {
            if (out->count < MAX_LAYERS)
            {
                layer_t* l = &out->items[out->count];
                l->node = k;
                l->z = has_z(k) ? k->style->z_index : 0;
                l->order = out->count;
                out->count++;
            }
            continue;
        }
        if (clips(k->style))
            continue;               /* It holds its own */
        collect_layers(k, out, depth + 1U);
    }
}

static void sort_layers(layers_t* l)
{
    for (uint32_t i = 1; i < l->count; i++)
    {
        layer_t x = l->items[i];
        uint32_t j = i;
        while (j > 0 && (l->items[j - 1U].z > x.z || (l->items[j - 1U].z == x.z && l->items[j - 1U].order > x.order)))
        {
            l->items[j] = l->items[j - 1U];
            j--;
        }
        l->items[j] = x;
    }
}

/* The box itself, then what it holds (in a clipping group when it clips), its layers among them */
static void paint_inner(painter_t* p, node_t* n, uint32_t depth)
{
    style_t* st = n->style;
    dmvsi_rect_t r = border_box(p, n);
    if (!(n->parent != NULL && n->parent->kind == NODE_DOCUMENT && p->c->options->root == NULL))
        paint_decoration(p, st, r, n->box.b);       /* The root's is the canvas's (paint_page) */
    if (node_is(n, "img"))
        paint_image(p, n);

    bool clip = clips(st);
    if (clip)
    {
        dmvsi_group_t g;
        memset(&g, 0, sizeof(g));
        g.rect = inner(&r, n->box.b);
        g.flags = DMVSI_GROUP_CLIP;
        g.opacity = 255;
        g.name = n->id;
        int32_t content_h = n->box.content_h + n->box.p[0] + n->box.p[2];
        if ((st->overflow_y == OVERFLOW_AUTO || st->overflow_y == OVERFLOW_SCROLL) && content_h > g.rect.h)
        {
            g.scroll_w = g.rect.w;
            g.scroll_h = content_h;
        }
        check(p, dmvsi_begin_group(p->c->doc, &g));
    }

    layers_t* layers = arena_alloc(&p->c->arena, sizeof(layers_t));
    if (layers != NULL)
    {
        collect_layers(n, layers, 0);
        sort_layers(layers);
    }
    uint32_t i = 0;
    for (; layers != NULL && i < layers->count && layers->items[i].z < 0; i++)
        paint_context(p, layers->items[i].node, depth + 1U);
    paint_lines(p, n);
    paint_flow(p, n, depth + 1U);
    for (; layers != NULL && i < layers->count; i++)
        paint_context(p, layers->items[i].node, depth + 1U);

    if (clip)
        check(p, dmvsi_end_group(p->c->doc));
}

/* The normal flow under n: each box's decoration and lines, in tree order - not the layers */
static void paint_flow(painter_t* p, node_t* n, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (!displayed(k) || is_layer(k))
            continue;
        if (clips(k->style))
        {
            paint_inner(p, k, depth + 1U);
            continue;
        }
        if (k->box.laid_out)
        {
            paint_decoration(p, k->style, border_box(p, k), k->box.b);
            if (node_is(k, "img"))
                paint_image(p, k);
            paint_lines(p, k);
        }
        paint_flow(p, k, depth + 1U);
    }
}

static void paint_context(painter_t* p, node_t* n, uint32_t depth)
{
    if (depth > MAX_DEPTH || !n->box.placed)
        return;
    style_t* st = n->style;
    bool group = st->opacity < 255;
    if (group)
    {
        if (st->opacity == 0)
            return;
        dmvsi_group_t g;
        memset(&g, 0, sizeof(g));
        g.opacity = st->opacity;
        g.name = n->id;
        check(p, dmvsi_begin_group(p->c->doc, &g));
    }
    /* drop-shadow(): everything once in each shadow's color, the last at the bottom */
    for (int i = (int)st->drop_count - 1; i >= 0 && !p->shadow; i--)
    {
        painter_t s = *p;
        s.shadow = true;
        s.dx = p->dx + st->drops[i].x;
        s.dy = p->dy + st->drops[i].y;
        s.shadow_color = st->drops[i].color;
        s.sigma = st->drops[i].blur / 2;
        paint_inner(&s, n, depth);
        check(p, s.status);
    }
    paint_inner(p, n, depth);
    if (group)
        check(p, dmvsi_end_group(p->c->doc));
}

/* ---- The page ---- */

static node_t* find_id(node_t* n, const char* id, uint32_t depth)
{
    if (depth > MAX_DEPTH)
        return NULL;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->id != NULL && strcmp(k->id, id) == 0)
            return k;
        node_t* found = find_id(k, id, depth + 1U);
        if (found != NULL)
            return found;
    }
    return NULL;
}

static node_t* html_of(node_t* document)
{
    for (node_t* k = document->first; k != NULL; k = k->next)
    {
        if (k->kind == NODE_ELEMENT && displayed(k))
            return k;
    }
    return NULL;
}

int paint_page(conv_t* c, node_t* document)
{
    painter_t p;
    memset(&p, 0, sizeof(p));
    p.c = c;
    node_t* html = html_of(document);
    node_t* root = html;
    if (c->options->root != NULL)
    {
        root = find_id(document, c->options->root, 0);
        if (root == NULL || !displayed(root) || !root->box.placed)
        {
            WARN(c, "no element with the id '%s' is shown\n", c->options->root);
            return -EINVAL;
        }
    }
    if (root == NULL)
        return -EBADMSG;

    int32_t w, h;
    if (c->options->root != NULL)
    {
        p.ox = root->box.ax;
        p.oy = root->box.ay;
        w = (root->box.w + U / 2) / U;
        h = (root->box.h + U / 2) / U;
    }
    else
    {
        w = c->vw / U;
        h = c->vh / U;
    }
    if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF)
        return -EINVAL;

    /* The view's name: the option's, else the file's */
    char name[64];
    const char* base = (c->options->name != NULL) ? c->options->name : c->path;
    const char* slash = strrchr(base, '/');
    base = (slash != NULL && c->options->name == NULL) ? slash + 1 : base;
    size_t n = 0;
    for (; base[n] != '\0' && base[n] != '.' && n + 1U < sizeof(name); n++)
        name[n] = base[n];
    name[n] = '\0';
    int status = dmvsi_set_view(c->doc, (n > 0) ? name : "view", (uint16_t)w, (uint16_t)h);
    if (status != 0)
        return status;

    if (c->options->root == NULL)
    {
        /* The canvas: the root's background, or the body's */
        const style_t* st = html->style;
        if ((st->background >> 24) == 0 && st->background_image == NULL)
        {
            for (node_t* k = html->first; k != NULL; k = k->next)
            {
                if (node_is(k, "body") && k->style != NULL)
                    st = k->style;
            }
        }
        p.canvas = st;
        dmvsi_fill_t f;
        memset(&f, 0, sizeof(f));
        f.rect.w = DMVSI_PX(w);
        f.rect.h = DMVSI_PX(h);
        f.paint.color = ((st->background >> 24) != 0) ? st->background : 0xFFFFFFFFu;
        check(&p, dmvsi_add_fill(c->doc, &f));
        if (st->background_image != NULL && gradient_paint(st->background_image, f.rect.w, f.rect.h, &f.paint))
            check(&p, dmvsi_add_fill(c->doc, &f));
    }
    paint_context(&p, root, 0);
    return p.status;
}
