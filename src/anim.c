#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * CSS animations, of what dmview can move: a box's position (transform:
 * translate) and opacity. An element animated by @keyframes gets its
 * group's variables (x, y, opacity), and the view one timer: every 20 ms
 * it starts the segment of each animation that is due - an ANIMATE of its
 * variables to the next keyframe, its timing function, its duration -
 * then the next, round and round (infinite; alternate: there and back).
 * What else a keyframe does (rotate, scale) is not drawn.
 */

#define MAX_FRAMES          8u
#define MAX_ANIMATIONS      24u
#define POLL_MS             20u

typedef struct
{
    uint32_t    at;                         /* 1/1000 of the animation */
    bool        moves, fades;                   /* A transform, an opacity */
    len_t       x, y;                       /* translate */
    int32_t     opacity;                    /* 0 ... 255 */
    int16_t     easing[4];
    bool        has_easing;
} frame_t;

typedef struct
{
    node_t*         element;
    frame_t         frames[2u * MAX_FRAMES];
    uint32_t        count;
    uint32_t        ms;
    dmvsi_var_t     phase, due;
} anim_t;

static const char* skip_space(const char* s, const char* end)
{
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        s++;
    return s;
}

static bool number_at(const char** p, const char* end, double* out)
{
    const char* s = *p;
    bool neg = false;
    if (s < end && (*s == '-' || *s == '+'))
        neg = *s++ == '-';
    double v = 0, scale = 0;
    bool any = false;
    for (; s < end && ((*s >= '0' && *s <= '9') || (*s == '.' && scale == 0)); s++)
    {
        if (*s == '.')
        {
            scale = 1;
            continue;
        }
        any = true;
        if (scale > 0)
        {
            scale /= 10;
            v += (*s - '0') * scale;
        }
        else
            v = v * 10 + (*s - '0');
    }
    if (!any)
        return false;
    *out = neg ? -v : v;
    *p = s;
    return true;
}

/* A length of translate(): px (1/64) or %; false when it is none of them */
static bool length_at(const char** p, const char* end, len_t* out)
{
    double v;
    const char* s = skip_space(*p, end);
    if (!number_at(&s, end, &v))
        return false;
    out->px = 0;
    out->pct = 0;
    if (s < end && *s == '%')
    {
        out->pct = (int32_t)(v * 100.0);         /* 1/100 % */
        s++;
    }
    else
    {
        out->px = (int32_t)(v * 64.0);
        if (s + 1 < end && s[0] == 'p' && s[1] == 'x')
            s += 2;
        else if (s + 2 < end && s[0] == 'r' && s[1] == 'e' && s[2] == 'm')
        {
            out->px *= 16;
            s += 3;
        }
    }
    *p = s;
    return true;
}

static bool starts_with(const char* s, const char* end, const char* w)
{
    size_t n = strlen(w);
    return (size_t)(end - s) >= n && strncmp(s, w, n) == 0;
}

/* transform: translate / translateX / translateY (none: 0) - false when it does what is not drawn */
static bool translation(const char* s, const char* end, frame_t* f)
{
    s = skip_space(s, end);
    f->x.px = f->x.pct = f->y.px = f->y.pct = 0;
    if (starts_with(s, end, "none"))
        return true;
    bool drawn = true;
    while (s < end)
    {
        s = skip_space(s, end);
        const char* name = s;
        while (s < end && *s != '(')
            s++;
        size_t n = (size_t)(s - name);
        if (s >= end)
            break;
        s++;
        len_t a = { 0, 0, 0 }, b = { 0, 0, 0 };
        bool has_a = length_at(&s, end, &a);
        s = skip_space(s, end);
        if (s < end && *s == ',')
            s++;
        bool has_b = length_at(&s, end, &b);
        while (s < end && *s != ')')
            s++;
        if (s < end)
            s++;
        if (n == 10 && strncmp(name, "translateY", 10) == 0 && has_a)
            f->y = a;
        else if (n == 10 && strncmp(name, "translateX", 10) == 0 && has_a)
            f->x = a;
        else if ((n == 9 && strncmp(name, "translate", 9) == 0) && has_a)
        {
            f->x = a;
            if (has_b)
                f->y = b;
        }
        else
            drawn = false;                  /* rotate, scale, ...: not drawn */
    }
    return drawn;
}

/* A timing function: cubic-bezier(...), ease, linear, ease-in, ease-out, ease-in-out */
static bool timing(const char* s, const char* end, int16_t* curve)
{
    static const struct { char name[12]; int16_t curve[4]; } keywords[] = {
        { "ease-in-out", { 420, 0, 580, 1000 } }, { "ease-in", { 420, 0, 1000, 1000 } }, { "ease-out", { 0, 0, 580, 1000 } },
        { "ease", { 250, 100, 250, 1000 } }, { "linear", { 0, 0, 1000, 1000 } },
    };
    s = skip_space(s, end);
    if (starts_with(s, end, "cubic-bezier("))
    {
        s += 13;
        for (int i = 0; i < 4; i++)
        {
            double v;
            s = skip_space(s, end);
            if (!number_at(&s, end, &v))
                return false;
            curve[i] = (int16_t)(v * 1000.0);
            s = skip_space(s, end);
            if (s < end && *s == ',')
                s++;
        }
        return true;
    }
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
    {
        if (starts_with(s, end, keywords[i].name))
        {
            memcpy(curve, keywords[i].curve, sizeof(keywords[i].curve));
            return true;
        }
    }
    return false;
}

static int frame_order(const frame_t* a, const frame_t* b)
{
    return (a->at < b->at) ? -1 : (a->at > b->at) ? 1 : 0;
}

/* The keyframes of a block: each selector (0%, 50%, from, to) a frame of its declarations */
static uint32_t read_frames(conv_t* c, const keyframes_t* kf, frame_t* frames, bool* drawn)
{
    uint32_t count = 0;
    const char* s = kf->body;
    const char* end = kf->body + kf->length;
    *drawn = true;
    while (s < end && count < MAX_FRAMES)
    {
        s = skip_space(s, end);
        const char* open = s;
        while (open < end && *open != '{')
            open++;
        const char* close = open;
        while (close < end && *close != '}')
            close++;
        if (open >= end || close >= end)
            break;
        /* Its declarations */
        frame_t f;
        memset(&f, 0, sizeof(f));
        f.opacity = 255;
        decl_t* decls = NULL;
        uint16_t n = css_parse_decls(c, open + 1, (size_t)(close - open - 1), &decls);
        for (uint16_t i = 0; i < n; i++)
        {
            const char* v = decls[i].value;
            const char* ve = v + strlen(v);
            if (strcmp(decls[i].name, "transform") == 0)
            {
                *drawn = translation(v, ve, &f) && *drawn;
                f.moves = true;
            }
            else if (strcmp(decls[i].name, "opacity") == 0)
            {
                double o;
                const char* q = v;
                if (number_at(&q, ve, &o))
                {
                    f.opacity = (int32_t)(o * 255.0 + 0.5);
                    f.fades = true;
                }
            }
            else if (strcmp(decls[i].name, "animation-timing-function") == 0)
                f.has_easing = timing(v, ve, f.easing);
        }
        /* Its selectors: a frame each */
        for (const char* q = s; q < open && count < MAX_FRAMES; )
        {
            q = skip_space(q, open);
            double pct = -1;
            if (starts_with(q, open, "from"))
                pct = 0;
            else if (starts_with(q, open, "to"))
                pct = 100;
            else
                (void)number_at(&q, open, &pct);
            while (q < open && *q != ',')
                q++;
            if (q < open)
                q++;
            if (pct < 0 || pct > 100)
                continue;
            frame_t g = f;
            g.at = (uint32_t)(pct * 10.0);
            frames[count++] = g;
        }
        s = close + 1;
    }
    /* In order; 0% and 100% as the element is when there are none */
    for (uint32_t i = 1; i < count; i++)
        for (uint32_t k = i; k > 0 && frame_order(&frames[k - 1U], &frames[k]) > 0; k--)
        {
            frame_t t = frames[k];
            frames[k] = frames[k - 1U];
            frames[k - 1U] = t;
        }
    if (count > 0 && frames[0].at != 0 && count < MAX_FRAMES)
    {
        memmove(&frames[1], &frames[0], count * sizeof(frame_t));
        memset(&frames[0], 0, sizeof(frame_t));
        frames[0].opacity = 255;
        count++;
    }
    if (count > 0 && frames[count - 1U].at != 1000 && count < MAX_FRAMES)
    {
        memset(&frames[count], 0, sizeof(frame_t));
        frames[count].opacity = 255;
        frames[count++].at = 1000;
    }
    return count;
}

static const keyframes_t* keyframes_named(const conv_t* c, const char* name)
{
    for (const keyframes_t* k = c->keyframes; k != NULL; k = k->next)
    {
        if (strcmp(k->name, name) == 0)
            return k;
    }
    return NULL;
}

static void find_animated(conv_t* c, node_t* n, anim_t* anims, uint32_t* count, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT || k->style == NULL || k->style->display == DISPLAY_NONE)
            continue;
        const style_t* st = k->style;
        if (st->animation != NULL && st->animation_ms > 0 && k->box.placed && *count < MAX_ANIMATIONS)
        {
            const keyframes_t* kf = keyframes_named(c, st->animation);
            anim_t* a = &anims[*count];
            memset(a, 0, sizeof(*a));
            bool drawn = true;
            a->count = (kf != NULL) ? read_frames(c, kf, a->frames, &drawn) : 0;
            if (kf == NULL)
                WARN(c, "the animation %s - its keyframes are not there\n", st->animation);
            else if (!drawn)
                WARN(c, "the animation %s of #%s - what it does but move and fade is not drawn\n", st->animation,
                     (k->id != NULL) ? k->id : k->tag);
            bool moves = false;
            for (uint32_t i = 0; i < a->count; i++)
                moves = moves || a->frames[i].moves || a->frames[i].fades;
            if (a->count >= 2 && moves && st->animation_infinite)
            {
                /* There and back: the frames again, the other way */
                if (st->animation_alternate)
                {
                    uint32_t n0 = a->count;
                    for (uint32_t i = n0 - 1U; i-- > 0 && a->count < 2u * MAX_FRAMES;)
                    {
                        frame_t f = a->frames[i];
                        f.at = 2000u - a->frames[i].at;
                        a->frames[a->count++] = f;
                    }
                    a->ms = st->animation_ms * 2u;
                }
                else
                    a->ms = st->animation_ms;
                a->element = k;
                (*count)++;
            }
        }
        find_animated(c, k, anims, count, depth + 1U);
    }
}

static int32_t offset(const len_t* l, int32_t size)
{
    return l->px + (int32_t)((int64_t)size * l->pct / 10000);
}

static void put(dmvsi_action_t* a, uint32_t* n, uint32_t max, uint8_t kind, dmvsi_var_t var, dmvsi_var_t operand, int32_t value)
{
    if (*n >= max)
        return;
    memset(&a[*n], 0, sizeof(dmvsi_action_t));
    a[*n].kind = kind;
    a[*n].var = var;
    a[*n].operand = operand;
    a[*n].value = value;
    (*n)++;
}

int animate_page(conv_t* c)
{
    anim_t* anims = Dmod_Malloc(MAX_ANIMATIONS * sizeof(anim_t));
    if (anims == NULL)
        return -ENOMEM;
    uint32_t count = 0;
    find_animated(c, c->document, anims, &count, 0);
    if (count == 0)
    {
        Dmod_Free(anims);
        return 0;
    }
    uint32_t max = 64u + count * (8u + 6u * 2u * MAX_FRAMES);
    dmvsi_action_t* actions = Dmod_Malloc(max * sizeof(dmvsi_action_t));
    if (actions == NULL)
    {
        Dmod_Free(anims);
        return -ENOMEM;
    }
    uint32_t n = 0;
    int status = 0;
    dmvsi_var_t left = dmvsi_add_var(c->doc, "anim_left", 0);
    int32_t ox, oy;
    view_origin(c, &ox, &oy);
    for (uint32_t i = 0; i < count && status == 0; i++)
    {
        anim_t* a = &anims[i];
        node_t* e = a->element;
        if (e->dynamic == NULL && (e->dynamic = arena_alloc(&c->arena, sizeof(dynamic_t))) == NULL)
        {
            status = -ENOMEM;
            break;
        }
        dynamic_t* d = e->dynamic;
        dmvsi_rect_t r = group_rect(c, e, ox, oy);
        bool mx = false, my = false, fades = false;
        for (uint32_t k = 0; k < a->count; k++)
        {
            mx = mx || offset(&a->frames[k].x, e->box.w) != 0;
            my = my || offset(&a->frames[k].y, e->box.h) != 0;
            fades = fades || a->frames[k].fades;
        }
        const char* base = (e->id != NULL) ? e->id : e->tag;
        char name[48];
        int32_t opacity = e->style->hidden ? 0 : e->style->opacity;
        if (mx && d->bind[DMVSI_BIND_X] == 0)
        {
            Dmod_SnPrintf(name, sizeof(name), "%s_ax", base);
            d->bind[DMVSI_BIND_X] = dmvsi_add_var(c->doc, name, r.x + offset(&a->frames[0].x, e->box.w));
        }
        if (my && d->bind[DMVSI_BIND_Y] == 0)
        {
            Dmod_SnPrintf(name, sizeof(name), "%s_ay", base);
            d->bind[DMVSI_BIND_Y] = dmvsi_add_var(c->doc, name, r.y + offset(&a->frames[0].y, e->box.h));
        }
        if (fades && d->bind[DMVSI_BIND_OPACITY] == 0)
        {
            Dmod_SnPrintf(name, sizeof(name), "%s_aalpha", base);
            d->bind[DMVSI_BIND_OPACITY] = dmvsi_add_var(c->doc, name, a->frames[0].opacity * opacity / 255);
        }
        Dmod_SnPrintf(name, sizeof(name), "%s_aphase", base);
        a->phase = dmvsi_add_var(c->doc, name, 0);
        Dmod_SnPrintf(name, sizeof(name), "%s_adue", base);
        a->due = dmvsi_add_var(c->doc, name, 0);

        /* Due: the next segment - to the next frame, in its time, by the timing of the frame it leaves */
        uint32_t span = a->frames[a->count - 1U].at;
        put(actions, &n, max, DMVSI_ACT_SET, left, a->due, 0);
        put(actions, &n, max, DMVSI_ACT_SUB, left, DMVSI_VAR_TIME, 0);
        put(actions, &n, max, DMVSI_ACT_IF_LE, left, 0, 0);
        for (uint32_t k = 0; k + 1U < a->count; k++)
        {
            const frame_t* from = &a->frames[k];
            const frame_t* to = &a->frames[k + 1U];
            uint32_t ms = (uint32_t)((uint64_t)(to->at - from->at) * a->ms / ((span > 0) ? span : 1u));
            if (ms == 0)
                ms = 1;
            const int16_t* easing = from->has_easing ? from->easing : e->style->animation_easing;
            put(actions, &n, max, DMVSI_ACT_IF_EQ, a->phase, 0, (int32_t)k);
            dmvsi_var_t vars[3] = { mx ? d->bind[DMVSI_BIND_X] : 0, my ? d->bind[DMVSI_BIND_Y] : 0,
                                    fades ? d->bind[DMVSI_BIND_OPACITY] : 0 };
            int32_t values[3] = { r.x + offset(&to->x, e->box.w), r.y + offset(&to->y, e->box.h), to->opacity * opacity / 255 };
            for (int v = 0; v < 3; v++)
            {
                if (vars[v] == 0)
                    continue;
                put(actions, &n, max, DMVSI_ACT_ANIMATE, vars[v], 0, values[v]);
                if (n > 0)
                {
                    actions[n - 1U].duration = (uint16_t)((ms > 60000u) ? 60000u : ms);
                    memcpy(actions[n - 1U].easing, easing, sizeof(actions[n - 1U].easing));
                }
            }
            /* due: from when it was due (not now: no drift) - unless far behind (the view was not running) */
            put(actions, &n, max, DMVSI_ACT_ADD, a->due, 0, (int32_t)ms);
            put(actions, &n, max, DMVSI_ACT_END, 0, 0, 0);
        }
        put(actions, &n, max, DMVSI_ACT_ADD, a->phase, 0, 1);
        put(actions, &n, max, DMVSI_ACT_IF_GE, a->phase, 0, (int32_t)(a->count - 1U));
        put(actions, &n, max, DMVSI_ACT_SET, a->phase, 0, 0);
        put(actions, &n, max, DMVSI_ACT_END, 0, 0, 0);
        put(actions, &n, max, DMVSI_ACT_SET, left, a->due, 0);
        put(actions, &n, max, DMVSI_ACT_SUB, left, DMVSI_VAR_TIME, 0);
        put(actions, &n, max, DMVSI_ACT_IF_LT, left, 0, -1000);
        put(actions, &n, max, DMVSI_ACT_SET, a->due, DMVSI_VAR_TIME, 0);
        put(actions, &n, max, DMVSI_ACT_END, 0, 0, 0);
        put(actions, &n, max, DMVSI_ACT_END, 0, 0, 0);
    }
    if (status == 0 && n > 0)
    {
        dmvsi_handler_t h = dmvsi_add_handler(c->doc, actions, n);
        if (h == 0 || dmvsi_add_timer(c->doc, POLL_MS, h) != 0)
            status = -EINVAL;
    }
    Dmod_Free(actions);
    Dmod_Free(anims);
    return status;
}
