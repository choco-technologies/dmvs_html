#include "private.h"
#include <string.h>

/*
 * The cascade: for every element the declarations of the rules that match
 * it - the user agent's, the page's style sheets, Tailwind's classes, its
 * style attribute - in the order of importance, origin, specificity and
 * appearance; custom properties and var() substituted; shorthands into
 * their longhands; values computed (em, rem, %, vw, calc(), currentColor,
 * inheritance).
 *
 * ::before and ::after with a content become child elements of their own;
 * text in a flex or grid container goes into an anonymous block, as CSS
 * makes it a flex item.
 */

#define MAX_MATCHED     512u
#define MAX_TW_DECLS    12u
#define MAX_VAR_DEPTH   16u
#define MAX_VALUE       2048u
#define TW_CACHE        1024u

#define SPEC_INLINE     (1u << 24)      /* A style attribute: above any selector */
#define ORDER_TAILWIND  0x40000000u     /* Tailwind's style sheet comes after the page's */

static const char g_user_agent[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,"
    "center,dir,hr,menu,pre,article,aside,footer,header,hgroup,main,nav,section,figure,figcaption,details,summary,"
    "legend,dialog,li,table,thead,tbody,tfoot,tr,caption{display:block}"
    "td,th{display:block}"
    "head,script,style,title,meta,link,template,noscript,base,area,datalist,param{display:none}"
    "[hidden]{display:none}"
    "body{margin:8px}"
    "p,blockquote,figure,dl,ul,ol,pre,menu{margin-top:1em;margin-bottom:1em}"
    "h1{font-size:2em;margin-top:.67em;margin-bottom:.67em;font-weight:bold}"
    "h2{font-size:1.5em;margin-top:.83em;margin-bottom:.83em;font-weight:bold}"
    "h3{font-size:1.17em;margin-top:1em;margin-bottom:1em;font-weight:bold}"
    "h4{margin-top:1.33em;margin-bottom:1.33em;font-weight:bold}"
    "h5{font-size:.83em;margin-top:1.67em;margin-bottom:1.67em;font-weight:bold}"
    "h6{font-size:.67em;margin-top:2.33em;margin-bottom:2.33em;font-weight:bold}"
    "ul,ol,menu{padding-left:40px}"
    "b,strong,th{font-weight:bold}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "small{font-size:.83em}"
    "center,th{text-align:center}"
    "pre{white-space:pre}"
    "hr{border-top-width:1px;border-style:solid;border-color:#808080;margin-top:.5em;margin-bottom:.5em}"
    "button{display:inline-block;padding:1px 6px;border:2px solid #767676;background-color:#efefef;"
    "text-align:center;font-size:13.333px;color:black}"
    "input,select,textarea{display:inline-block;font-size:13.333px}"
    "img,svg,video,canvas{display:inline-block}"
    "a{color:#0000ee}";

/* ---- Lengths ---- */

int32_t len_resolve(len_t l, int32_t base)
{
    if (l.kind != LEN_SET)
        return 0;
    return l.px + (int32_t)(((int64_t)l.pct * base) / 10000);
}

typedef struct
{
    int32_t     font_size;              /* The element's (for em) */
    int32_t     vw, vh;
} units_t;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* Whether `s` has `what` in it */
static bool contains(const char* s, const char* what)
{
    size_t n = strlen(what);
    for (; *s != '\0'; s++)
    {
        if (strncmp(s, what, n) == 0)
            return true;
    }
    return false;
}

static const char* skip(const char* s)
{
    while (is_space(*s))
        s++;
    return s;
}

static bool word_is(const char* s, const char* w)
{
    size_t n = strlen(w);
    for (size_t i = 0; i < n; i++)
    {
        if (lower(s[i]) != w[i])
            return false;
    }
    char next = s[n];
    return !((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9') || next == '-');
}

/* A number at *p: its value x 1000; false when none */
static bool number(const char** p, int64_t* v)
{
    const char* s = *p;
    bool minus = false;
    if (*s == '-' || *s == '+')
    {
        minus = *s == '-';
        s++;
    }
    int64_t whole = 0, frac = 0, scale = 1000;
    bool digits = false;
    for (; *s >= '0' && *s <= '9'; s++)
    {
        whole = whole * 10 + (*s - '0');
        digits = true;
        if (whole > 100000000)
            whole = 100000000;
    }
    if (*s == '.' && s[1] >= '0' && s[1] <= '9')
    {
        for (s++; *s >= '0' && *s <= '9'; s++)
        {
            if (scale > 1)
            {
                scale /= 10;
                frac += (*s - '0') * scale;
            }
            digits = true;
        }
    }
    if (!digits)
        return false;
    /* An exponent: "1e3", "2.5E-2" - not the "e" of "em", "ex" */
    if ((*s == 'e' || *s == 'E') && ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s[2] >= '0' && s[2] <= '9')))
    {
        const char* e = s + 1;
        bool neg = *e == '-';
        if (*e == '-' || *e == '+')
            e++;
        int exp = 0;
        for (; *e >= '0' && *e <= '9'; e++)
            exp = exp * 10 + (*e - '0');
        s = e;
        int64_t t = whole * 1000 + frac;
        for (; exp > 0; exp--)
            t = neg ? t / 10 : t * 10;
        *v = minus ? -t : t;
        *p = s;
        return true;
    }
    *v = (whole * 1000 + frac) * (minus ? -1 : 1);
    *p = s;
    return true;
}

typedef struct
{
    int64_t     px;                     /* x 1000, 1/64 px */
    int64_t     pct;                    /* x 1000, 1/100 % */
    int64_t     num;                    /* x 1000, a plain number */
    uint8_t     kind;                   /* 0 length / percentage, 1 number */
} term_t;

static bool calc_sum(const char** p, const units_t* u, term_t* out);

/* A length, a percentage, a number, calc() / min() / max() / clamp() or (...) */
static bool calc_value(const char** p, const units_t* u, term_t* out)
{
    const char* s = skip(*p);
    memset(out, 0, sizeof(*out));
    if (*s == '(' || word_is(s, "calc"))
    {
        s = (*s == '(') ? s + 1 : skip(s + 4) + 1;
        if (!calc_sum(&s, u, out))
            return false;
        s = skip(s);
        if (*s != ')')
            return false;
        *p = s + 1;
        return true;
    }
    if (word_is(s, "min") || word_is(s, "max") || word_is(s, "clamp"))
    {
        bool is_min = s[1] == 'i', is_clamp = s[0] == 'c';
        s = skip(s + (is_clamp ? 5 : 3));
        if (*s != '(')
            return false;
        s++;
        term_t args[3];
        int n = 0;
        while (n < 3)
        {
            if (!calc_sum(&s, u, &args[n]))
                return false;
            n++;
            s = skip(s);
            if (*s == ',')
                s++;
            else
                break;
        }
        if (*s != ')')
            return false;
        *p = s + 1;
        /* Percentages cannot be compared here: their px part decides */
        if (is_clamp && n == 3)
        {
            *out = args[1];
            if (out->px < args[0].px)
                *out = args[0];
            if (out->px > args[2].px)
                *out = args[2];
            return true;
        }
        *out = args[0];
        for (int i = 1; i < n; i++)
        {
            if ((is_min && args[i].px < out->px) || (!is_min && args[i].px > out->px))
                *out = args[i];
        }
        return true;
    }
    int64_t v;
    if (!number(&s, &v))
        return false;
    if (*s == '%')
    {
        out->pct = v * 100;
        s++;
    }
    else if (word_is(s, "px"))
    {
        out->px = v * U;
        s += 2;
    }
    else if (word_is(s, "rem"))
    {
        out->px = v * 16 * U;
        s += 3;
    }
    else if (word_is(s, "em"))
    {
        out->px = v * u->font_size;
        s += 2;
    }
    else if (word_is(s, "vw") || word_is(s, "vh"))
    {
        out->px = v * ((s[1] == 'w') ? u->vw : u->vh) / 100;
        s += 2;
    }
    else if (word_is(s, "vmin") || word_is(s, "vmax"))
    {
        int32_t a = (u->vw < u->vh) ? u->vw : u->vh, b = (u->vw < u->vh) ? u->vh : u->vw;
        out->px = v * ((s[2] == 'i') ? a : b) / 100;
        s += 4;
    }
    else if (word_is(s, "pt"))
    {
        out->px = v * U * 4 / 3;
        s += 2;
    }
    else if (word_is(s, "ch") || word_is(s, "ex"))
    {
        out->px = v * u->font_size / 2;
        s += 2;
    }
    else if (word_is(s, "deg") || word_is(s, "s") || word_is(s, "ms"))
        return false;
    else
    {
        out->num = v;
        out->kind = 1;
    }
    *p = s;
    return true;
}

static bool calc_product(const char** p, const units_t* u, term_t* out)
{
    if (!calc_value(p, u, out))
        return false;
    for (;;)
    {
        const char* s = skip(*p);
        if (*s != '*' && *s != '/')
            return true;
        char op = *s++;
        term_t b;
        if (!calc_value(&s, u, &b))
            return false;
        *p = s;
        if (op == '*')
        {
            if (out->kind == 1 && b.kind == 1)
                out->num = out->num * b.num / 1000;
            else if (out->kind == 1)
            {
                int64_t k = out->num;
                *out = b;
                out->px = b.px * k / 1000;
                out->pct = b.pct * k / 1000;
            }
            else if (b.kind == 1)
            {
                out->px = out->px * b.num / 1000;
                out->pct = out->pct * b.num / 1000;
            }
            else
                return false;
        }
        else
        {
            if (b.kind != 1 || b.num == 0)
                return false;
            if (out->kind == 1)
                out->num = out->num * 1000 / b.num;
            else
            {
                out->px = out->px * 1000 / b.num;
                out->pct = out->pct * 1000 / b.num;
            }
        }
    }
}

static bool calc_sum(const char** p, const units_t* u, term_t* out)
{
    if (!calc_product(p, u, out))
        return false;
    for (;;)
    {
        const char* s = skip(*p);
        if ((*s != '+' && *s != '-') || !is_space(s[-1]))
            return true;
        char op = *s++;
        term_t b;
        if (!calc_product(&s, u, &b))
            return false;
        *p = s;
        int64_t sign = (op == '-') ? -1 : 1;
        if (out->kind == 1 && b.kind == 1)
            out->num += sign * b.num;
        else if (out->kind == 0 && b.kind == 0)
        {
            out->px += sign * b.px;
            out->pct += sign * b.pct;
        }
        else if (out->kind == 1 && out->num == 0)
        {
            *out = b;
            out->px *= sign;
            out->pct *= sign;
        }
        else if (!(b.kind == 1 && b.num == 0))
            return false;
    }
}

/* A length at *p; `unitless`: a number is pixels (0 always is) */
static bool parse_len(const char** p, const units_t* u, len_t* out, bool unitless)
{
    const char* s = skip(*p);
    if (word_is(s, "auto"))
    {
        *out = len_auto();
        *p = s + 4;
        return true;
    }
    if (word_is(s, "none"))
    {
        out->kind = LEN_NONE;
        out->px = out->pct = 0;
        *p = s + 4;
        return true;
    }
    if (word_is(s, "min-content") || word_is(s, "max-content") || word_is(s, "fit-content"))
    {
        out->kind = LEN_CONTENT;
        out->px = out->pct = 0;
        *p = s + 11;
        return true;
    }
    term_t t;
    if (!calc_value(&s, u, &t))
        return false;
    if (t.kind == 1)
    {
        if (!unitless && t.num != 0)
            return false;
        t.px = t.num * U;
    }
    out->kind = LEN_SET;
    out->px = (int32_t)(t.px / 1000);
    out->pct = (int32_t)(t.pct / 1000);
    *p = s;
    return true;
}

static bool parse_px(const char* v, const units_t* u, int32_t* px)
{
    len_t l;
    const char* s = v;
    if (!parse_len(&s, u, &l, false) || l.kind != LEN_SET)
        return false;
    *px = l.px;
    return true;
}

/* ---- Colors ---- */

/* Text, not pointers: a module's data is not relocated (only its GOT is) */
static const struct
{
    char        name[14];
    uint32_t    rgb;
} g_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "purple", 0x800080 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "navy", 0x000080 }, { "teal", 0x008080 }, { "olive", 0x808000 },
    { "lime", 0x00FF00 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "dimgray", 0x696969 }, { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "crimson", 0xDC143C },
    { "tomato", 0xFF6347 }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "khaki", 0xF0E68C }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 },
    { "dodgerblue", 0x1E90FF }, { "deepskyblue", 0x00BFFF }, { "darkblue", 0x00008B }, { "darkgreen", 0x006400 },
    { "darkred", 0x8B0000 }, { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "slategray", 0x708090 },
    { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 }, { "orchid", 0xDA70D6 }, { "plum", 0xDDA0DD },
    { "tan", 0xD2B48C }, { "chocolate", 0xD2691E }, { "firebrick", 0xB22222 }, { "turquoise", 0x40E0D0 },
    { "buttonface", 0xEFEFEF }, { "buttontext", 0x000000 }, { "canvas", 0xFFFFFF }, { "canvastext", 0x000000 },
};

static int32_t clamp255(int64_t v) { return (v < 0) ? 0 : (v > 255) ? 255 : (int32_t)v; }

/* One channel of rgb(): 0 ... 255, or a percentage */
static bool channel(const char** p, int32_t* out)
{
    int64_t v;
    const char* s = skip(*p);
    if (!number(&s, &v))
        return false;
    if (*s == '%')
    {
        s++;
        v = v * 255 / 100;
    }
    *out = clamp255((v + 500) / 1000);
    *p = s;
    return true;
}

static bool alpha_value(const char** p, int32_t* out)
{
    int64_t v;
    const char* s = skip(*p);
    if (!number(&s, &v))
        return false;
    if (*s == '%')
    {
        s++;
        v /= 100;
    }
    *out = clamp255((v * 255 + 500) / 1000);
    *p = s;
    return true;
}

static int32_t hue_to_rgb(int64_t m1, int64_t m2, int64_t h)
{
    /* x 1000 */
    if (h < 0)
        h += 360000;
    if (h >= 360000)
        h -= 360000;
    int64_t v;
    if (h < 60000)
        v = m1 + (m2 - m1) * h / 60000;
    else if (h < 180000)
        v = m2;
    else if (h < 240000)
        v = m1 + (m2 - m1) * (240000 - h) / 60000;
    else
        v = m1;
    return clamp255((v * 255 + 500) / 1000);
}

/* A color at *p (advanced past it); `current` for currentColor */
static bool parse_color(const char** p, uint32_t current, uint32_t* out)
{
    const char* s = skip(*p);
    if (*s == '#')
    {
        uint32_t h = 0;
        size_t n = 0;
        for (s++; ; s++, n++)
        {
            char d = *s;
            uint32_t v = (d >= '0' && d <= '9') ? (uint32_t)(d - '0') : (d >= 'a' && d <= 'f') ? (uint32_t)(d - 'a' + 10)
                       : (d >= 'A' && d <= 'F') ? (uint32_t)(d - 'A' + 10) : 99U;
            if (v == 99U)
                break;
            h = (h << 4) | v;
        }
        uint32_t r, g, b, a = 0xFF;
        if (n == 3 || n == 4)
        {
            if (n == 4)
            {
                a = (h & 0xF) * 17U;
                h >>= 4;
            }
            r = ((h >> 8) & 0xF) * 17U;
            g = ((h >> 4) & 0xF) * 17U;
            b = (h & 0xF) * 17U;
        }
        else if (n == 6 || n == 8)
        {
            if (n == 8)
            {
                a = h & 0xFF;
                h >>= 8;
            }
            r = (h >> 16) & 0xFF;
            g = (h >> 8) & 0xFF;
            b = h & 0xFF;
        }
        else
            return false;
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        *p = s;
        return true;
    }
    if (word_is(s, "rgb") || word_is(s, "rgba") || word_is(s, "hsl") || word_is(s, "hsla"))
    {
        bool hsl = lower(s[0]) == 'h';
        s += (lower(s[3]) == 'a') ? 4 : 3;
        s = skip(s);
        if (*s != '(')
            return false;
        s++;
        int32_t ch[3], a = 255;
        if (hsl)
        {
            int64_t hue, sat, lig;
            const char* q = skip(s);
            if (!number(&q, &hue))
                return false;
            if (word_is(q, "deg"))
                q += 3;
            q = skip(q);
            if (*q == ',')
                q++;
            q = skip(q);
            if (!number(&q, &sat) || *q != '%')
                return false;
            q = skip(q + 1);
            if (*q == ',')
                q++;
            q = skip(q);
            if (!number(&q, &lig) || *q != '%')
                return false;
            s = q + 1;
            sat /= 100;                 /* x 1000 of 1 */
            lig /= 100;
            int64_t m2 = (lig <= 500) ? lig * (1000 + sat) / 1000 : lig + sat - lig * sat / 1000;
            int64_t m1 = 2 * lig - m2;
            ch[0] = hue_to_rgb(m1, m2, hue + 120000);
            ch[1] = hue_to_rgb(m1, m2, hue);
            ch[2] = hue_to_rgb(m1, m2, hue - 120000);
        }
        else
        {
            for (int i = 0; i < 3; i++)
            {
                if (!channel(&s, &ch[i]))
                    return false;
                s = skip(s);
                if (*s == ',' && i < 2)
                    s++;                /* The one before the alpha is its separator */
            }
        }
        s = skip(s);
        if (*s == ',' || *s == '/')
        {
            s++;
            if (!alpha_value(&s, &a))
                return false;
        }
        s = skip(s);
        if (*s != ')')
            return false;
        *out = ((uint32_t)a << 24) | ((uint32_t)ch[0] << 16) | ((uint32_t)ch[1] << 8) | (uint32_t)ch[2];
        *p = s + 1;
        return true;
    }
    if (word_is(s, "transparent"))
    {
        *out = 0;
        *p = s + 11;
        return true;
    }
    if (word_is(s, "currentcolor"))
    {
        *out = current;
        *p = s + 12;
        return true;
    }
    for (size_t i = 0; i < sizeof(g_colors) / sizeof(g_colors[0]); i++)
    {
        if (word_is(s, g_colors[i].name))
        {
            *out = 0xFF000000u | g_colors[i].rgb;
            *p = s + strlen(g_colors[i].name);
            return true;
        }
    }
    return false;
}

/* ---- Gradients ---- */

/* linear-gradient(...) / radial-gradient(...) at v; NULL when it is not one */
static gradient_t* parse_gradient(conv_t* c, const char* v, const units_t* u, uint32_t current)
{
    const char* s = skip(v);
    bool linear;
    if (word_is(s, "linear-gradient") || word_is(s, "repeating-linear-gradient"))
        linear = true;
    else if (word_is(s, "radial-gradient") || word_is(s, "repeating-radial-gradient"))
        linear = false;
    else
        return NULL;
    s = strchr(s, '(');
    if (s == NULL)
        return NULL;
    s++;
    gradient_t* g = arena_alloc(&c->arena, sizeof(*g));
    if (g == NULL)
        return NULL;
    g->kind = linear ? CSS_LINEAR : CSS_RADIAL;
    g->angle = 180;
    g->cx.kind = g->cy.kind = LEN_SET;
    g->cx.pct = g->cy.pct = 5000;

    /* The geometry, before the first comma - when it is not a color */
    const char* first = skip(s);
    uint32_t probe;
    const char* t = first;
    if (!parse_color(&t, current, &probe))
    {
        if (linear)
        {
            int64_t a;
            const char* q = first;
            if (word_is(q, "to"))
            {
                q += 2;
                for (int k = 0; k < 2; k++)
                {
                    q = skip(q);
                    if (word_is(q, "top")) { g->to |= TO_TOP; q += 3; }
                    else if (word_is(q, "bottom")) { g->to |= TO_BOTTOM; q += 6; }
                    else if (word_is(q, "left")) { g->to |= TO_LEFT; q += 4; }
                    else if (word_is(q, "right")) { g->to |= TO_RIGHT; q += 5; }
                }
            }
            else if (number(&q, &a))
            {
                if (word_is(q, "turn"))
                    a *= 360;
                else if (word_is(q, "rad"))
                    a = a * 180000 / 3141593;
                g->angle = (int16_t)(a / 1000);
            }
        }
        else
        {
            const char* q = first;
            while (*q != '\0' && *q != ',')
            {
                q = skip(q);
                if (word_is(q, "circle")) { g->circle = true; q += 6; }
                else if (word_is(q, "ellipse")) { q += 7; }
                else if (word_is(q, "closest-side")) { g->extent = 1; q += 12; }
                else if (word_is(q, "farthest-side")) { g->extent = 2; q += 13; }
                else if (word_is(q, "closest-corner")) { g->extent = 3; q += 14; }
                else if (word_is(q, "farthest-corner")) { g->extent = 0; q += 15; }
                else if (word_is(q, "at"))
                {
                    q += 2;
                    for (int k = 0; k < 2; k++)
                    {
                        q = skip(q);
                        len_t* axis = (k == 0) ? &g->cx : &g->cy;
                        if (word_is(q, "center")) { q += 6; }
                        else if (word_is(q, "left")) { g->cx.pct = 0; q += 4; }
                        else if (word_is(q, "right")) { g->cx.pct = 10000; q += 5; }
                        else if (word_is(q, "top")) { g->cy.pct = 0; q += 3; }
                        else if (word_is(q, "bottom")) { g->cy.pct = 10000; q += 6; }
                        else if (!parse_len(&q, u, axis, false))
                            break;
                    }
                }
                else
                    q++;
            }
        }
        s = strchr(s, ',');
        if (s == NULL)
            return NULL;
        s++;
    }

    /* The stops: color [position [position]] */
    while (*s != '\0' && g->count < DMVSI_MAX_STOPS)
    {
        uint32_t color;
        s = skip(s);
        if (!parse_color(&s, current, &color))
            break;
        for (int k = 0; k < 2 && g->count < DMVSI_MAX_STOPS; k++)
        {
            len_t pos;
            const char* q = skip(s);
            g->colors[g->count] = color;
            g->positions[g->count] = -1;
            if (*q != ',' && *q != ')' && parse_len(&q, u, &pos, false) && pos.kind == LEN_SET)
            {
                g->positions[g->count] = pos.pct;   /* Pixels left as 0: not known here */
                s = q;
                g->count++;
                continue;
            }
            if (k == 0)
                g->count++;
            break;
        }
        s = skip(s);
        if (*s != ',')
            break;
        s++;
    }
    return (g->count >= 2U) ? g : NULL;
}

/* ---- Shadows, filters, transforms ---- */

/* One shadow of a list: [inset] x y [blur [spread]] [color] */
static bool parse_shadow(const char** p, const units_t* u, uint32_t current, shadow_t* sh)
{
    const char* s = *p;
    int32_t lengths[4];
    int n = 0;
    bool has_color = false;
    memset(sh, 0, sizeof(*sh));
    sh->color = current;
    while (*s != '\0' && *s != ',')
    {
        s = skip(s);
        if (*s == '\0' || *s == ',')
            break;
        if (word_is(s, "inset"))
        {
            sh->inset = true;
            s += 5;
            continue;
        }
        len_t l;
        const char* q = s;
        if (n < 4 && parse_len(&q, u, &l, false) && l.kind == LEN_SET)
        {
            lengths[n++] = l.px;
            s = q;
            continue;
        }
        if (!has_color && parse_color(&s, current, &sh->color))
        {
            has_color = true;
            continue;
        }
        return false;
    }
    if (n < 2)
        return false;
    sh->x = lengths[0];
    sh->y = lengths[1];
    sh->blur = (n > 2) ? lengths[2] : 0;
    sh->spread = (n > 3) ? lengths[3] : 0;
    *p = s;
    return true;
}

static void parse_shadows(const char* v, const units_t* u, uint32_t current, shadow_t* out, uint8_t* count, bool drop)
{
    const char* s = skip(v);
    *count = 0;
    if (word_is(s, "none"))
        return;
    while (*s != '\0' && *count < MAX_SHADOWS)
    {
        shadow_t sh;
        if (!parse_shadow(&s, u, current, &sh))
            return;
        if ((sh.color >> 24) != 0 && !(drop && sh.inset))
            out[(*count)++] = sh;
        s = skip(s);
        if (*s == ',')
            s++;
    }
}

/* filter: blur() and drop-shadow() (the others are not drawn) */
static void parse_filter(style_t* st, const char* v, const units_t* u)
{
    const char* s = skip(v);
    st->blur = 0;
    st->drop_count = 0;
    while (*s != '\0')
    {
        s = skip(s);
        const char* open = strchr(s, '(');
        if (open == NULL)
            return;
        const char* close = open + 1;
        int depth = 1;
        while (*close != '\0' && depth > 0)
        {
            if (*close == '(')
                depth++;
            else if (*close == ')')
                depth--;
            if (depth > 0)
                close++;
        }
        char inner[256];
        size_t n = (size_t)(close - open - 1);
        if (n >= sizeof(inner))
            n = sizeof(inner) - 1U;
        memcpy(inner, open + 1, n);
        inner[n] = '\0';
        if (word_is(s, "blur"))
            (void)parse_px(inner, u, &st->blur);
        else if (word_is(s, "drop-shadow") && st->drop_count < MAX_SHADOWS)
        {
            shadow_t sh;
            const char* q = inner;
            if (parse_shadow(&q, u, st->color, &sh) && (sh.color >> 24) != 0)
                st->drops[st->drop_count++] = sh;
        }
        s = (*close != '\0') ? close + 1 : close;
    }
}

/* transform: its translations (rotations and scales are not drawn) */
static void parse_transform(style_t* st, const char* v, const units_t* u)
{
    const char* s = skip(v);
    st->translate_x = len_px(0);
    st->translate_y = len_px(0);
    while (*s != '\0')
    {
        s = skip(s);
        bool x = word_is(s, "translatex"), y = word_is(s, "translatey"), both = word_is(s, "translate") || word_is(s, "translate3d");
        const char* open = strchr(s, '(');
        if (open == NULL)
            return;
        const char* q = open + 1;
        if (x || both)
        {
            len_t l;
            if (parse_len(&q, u, &l, false) && l.kind == LEN_SET)
            {
                st->translate_x.px += l.px;
                st->translate_x.pct += l.pct;
            }
            q = skip(q);
            if (both && *q == ',')
            {
                q++;
                if (parse_len(&q, u, &l, false) && l.kind == LEN_SET)
                {
                    st->translate_y.px += l.px;
                    st->translate_y.pct += l.pct;
                }
            }
        }
        else if (y)
        {
            len_t l;
            if (parse_len(&q, u, &l, false) && l.kind == LEN_SET)
            {
                st->translate_y.px += l.px;
                st->translate_y.pct += l.pct;
            }
        }
        const char* close = strchr(q, ')');
        if (close == NULL)
            return;
        s = close + 1;
    }
}

/* ---- Grid tracks ---- */

static bool parse_track(const char** p, const units_t* u, track_t* t)
{
    const char* s = skip(*p);
    memset(t, 0, sizeof(*t));
    if (word_is(s, "minmax"))
    {
        /* minmax(a, b): b decides (fr, or a length) */
        s = strchr(s, ',');
        if (s == NULL)
            return false;
        s++;
        if (!parse_track(&s, u, t))
            return false;
        s = skip(s);
        if (*s == ')')
            s++;
        *p = s;
        return true;
    }
    if (word_is(s, "auto") || word_is(s, "min-content") || word_is(s, "max-content"))
    {
        t->kind = TRACK_AUTO;
        while (*s != '\0' && !is_space(*s) && *s != ')' && *s != ',')
            s++;
        *p = s;
        return true;
    }
    const char* q = s;
    int64_t v;
    if (number(&q, &v) && word_is(q, "fr"))
    {
        t->kind = TRACK_FR;
        t->fr = (int32_t)(v / 10);
        *p = q + 2;
        return true;
    }
    if (parse_len(&s, u, &t->length, false) && t->length.kind == LEN_SET)
    {
        t->kind = TRACK_LENGTH;
        *p = s;
        return true;
    }
    return false;
}

static void parse_columns(style_t* st, const char* v, const units_t* u)
{
    const char* s = skip(v);
    st->column_count = 0;
    while (*s != '\0' && st->column_count < MAX_TRACKS)
    {
        s = skip(s);
        if (*s == '\0')
            break;
        if (word_is(s, "repeat"))
        {
            const char* q = strchr(s, '(');
            int64_t n;
            if (q == NULL)
                return;
            q = skip(q + 1);
            if (!number(&q, &n))
                return;            /* auto-fill / auto-fit: not supported */
            q = skip(q);
            if (*q != ',')
                return;
            q++;
            track_t t;
            const char* after = q;
            if (!parse_track(&after, u, &t))
                return;
            for (int64_t k = 0; k < n / 1000 && st->column_count < MAX_TRACKS; k++)
                st->columns[st->column_count++] = t;
            const char* close = strchr(after, ')');
            if (close == NULL)
                return;
            s = close + 1;
            continue;
        }
        if (word_is(s, "none"))
            return;
        if (!parse_track(&s, u, &st->columns[st->column_count]))
            return;
        st->column_count++;
    }
}

/* "span 2 / span 2", "1 / -1" (every column), "span 3" */
static uint8_t parse_span(const char* v)
{
    const char* s = skip(v);
    int64_t n;
    if (word_is(s, "span"))
    {
        s = skip(s + 4);
        return (number(&s, &n) && n >= 1000) ? (uint8_t)((n / 1000 > 64) ? 64 : n / 1000) : 1U;
    }
    if (number(&s, &n))
    {
        s = skip(s);
        if (*s == '/')
        {
            s = skip(s + 1);
            int64_t e;
            if (word_is(s, "span"))
            {
                s = skip(s + 4);
                return (number(&s, &e) && e >= 1000) ? (uint8_t)(e / 1000) : 1U;
            }
            if (number(&s, &e))
            {
                if (e < 0)
                    return 255U;        /* To the last line: every column */
                return (e > n) ? (uint8_t)((e - n) / 1000) : 1U;
            }
        }
    }
    return 1U;
}

/* ---- Keywords ---- */

static uint8_t align_keyword(const char* v)
{
    const char* s = skip(v);
    if (word_is(s, "safe") || word_is(s, "unsafe"))
        s = skip(s + ((lower(*s) == 's') ? 4 : 6));
    if (word_is(s, "flex-start") || word_is(s, "start") || word_is(s, "self-start") || word_is(s, "left"))
        return ALIGN_START;
    if (word_is(s, "flex-end") || word_is(s, "end") || word_is(s, "self-end") || word_is(s, "right"))
        return ALIGN_END;
    if (word_is(s, "center"))
        return ALIGN_CENTER;
    if (word_is(s, "stretch"))
        return ALIGN_STRETCH;
    if (word_is(s, "baseline") || word_is(s, "first") || word_is(s, "last"))
        return ALIGN_BASELINE;
    if (word_is(s, "space-between"))
        return ALIGN_BETWEEN;
    if (word_is(s, "space-around"))
        return ALIGN_AROUND;
    if (word_is(s, "space-evenly"))
        return ALIGN_EVENLY;
    if (word_is(s, "auto"))
        return ALIGN_AUTO;
    return ALIGN_NORMAL;
}

static uint8_t display_keyword(const char* v)
{
    const char* s = skip(v);
    if (word_is(s, "none")) return DISPLAY_NONE;
    if (word_is(s, "inline-block")) return DISPLAY_INLINE_BLOCK;
    if (word_is(s, "inline-flex")) return DISPLAY_INLINE_FLEX;
    if (word_is(s, "inline-grid")) return DISPLAY_INLINE_GRID;
    if (word_is(s, "inline-table")) return DISPLAY_INLINE_BLOCK;
    if (word_is(s, "inline")) return DISPLAY_INLINE;
    if (word_is(s, "flex")) return DISPLAY_FLEX;
    if (word_is(s, "grid")) return DISPLAY_GRID;
    if (word_is(s, "contents")) return DISPLAY_CONTENTS;
    return DISPLAY_BLOCK;               /* block, list-item, table, flow-root, ... */
}

static uint8_t overflow_keyword(const char* v)
{
    const char* s = skip(v);
    if (word_is(s, "hidden") || word_is(s, "clip")) return OVERFLOW_HIDDEN;
    if (word_is(s, "scroll")) return OVERFLOW_SCROLL;
    if (word_is(s, "auto") || word_is(s, "overlay")) return OVERFLOW_AUTO;
    return OVERFLOW_VISIBLE;
}

/* "a b c d" -> up to 4 values, CSS's box shorthand order: top right bottom left */
static int split_values(const char* v, const char** parts, size_t* lengths, int max)
{
    int n = 0;
    const char* s = skip(v);
    while (*s != '\0' && n < max)
    {
        const char* start = s;
        int depth = 0;
        while (*s != '\0' && (depth > 0 || !is_space(*s)))
        {
            if (*s == '(')
                depth++;
            else if (*s == ')')
                depth--;
            s++;
        }
        parts[n] = start;
        lengths[n] = (size_t)(s - start);
        n++;
        s = skip(s);
    }
    return n;
}


/* ---- Transitions ---- */

static uint8_t transition_property(const char* s, size_t n)
{
    static const char positions[] =
        "|top|left|right|bottom|inset|margin|margin-top|margin-left|margin-right|margin-bottom|transform|translate|";
    char name[32];
    if (n == 0 || n >= sizeof(name))
        return TRANSITION_OTHER;
    for (size_t i = 0; i < n; i++)
        name[i] = lower(s[i]);
    name[n] = '\0';
    if (strcmp(name, "all") == 0)
        return TRANSITION_ALL;
    if (strcmp(name, "opacity") == 0)
        return TRANSITION_OPACITY;
    if (strcmp(name, "none") == 0)
        return TRANSITION_NONE;
    for (const char* p = positions; *p != '\0'; p++)
    {
        if (*p == '|' && strncmp(p + 1, name, n) == 0 && p[1 + n] == '|')
            return TRANSITION_POSITION;
    }
    return TRANSITION_OTHER;
}

/* "0.3s", "300ms" -> milliseconds; false when it is no time */
static bool parse_time(const char** p, uint16_t* ms)
{
    const char* s = skip(*p);
    int64_t v;
    if (!number(&s, &v))
        return false;
    if (word_is(s, "ms"))
        s += 2;
    else if (word_is(s, "s"))
    {
        s += 1;
        v *= 1000;
    }
    else
        return false;
    v /= 1000;
    *ms = (uint16_t)((v < 0) ? 0 : (v > 60000) ? 60000 : v);
    *p = s;
    return true;
}

/* A timing function: its cubic-bezier, 1/1000; false when it is none */
static bool parse_timing(const char** p, int16_t* curve)
{
    static const struct { char name[12]; int16_t curve[4]; } keywords[] = {
        { "ease", { 250, 100, 250, 1000 } }, { "linear", { 0, 0, 1000, 1000 } }, { "ease-in", { 420, 0, 1000, 1000 } },
        { "ease-out", { 0, 0, 580, 1000 } }, { "ease-in-out", { 420, 0, 580, 1000 } },
    };
    const char* s = skip(*p);
    if (word_is(s, "cubic-bezier"))
    {
        s = skip(s + 12);
        if (*s != '(')
            return false;
        s++;
        for (int i = 0; i < 4; i++)
        {
            int64_t v;
            s = skip(s);
            if (!number(&s, &v))
                return false;
            curve[i] = (int16_t)((v < -2000) ? -2000 : (v > 2000) ? 2000 : v);
            s = skip(s);
            if (*s == ',')
                s++;
        }
        s = skip(s);
        if (*s == ')')
            s++;
        *p = s;
        return true;
    }
    if (word_is(s, "steps") || word_is(s, "step-start") || word_is(s, "step-end"))
    {
        while (*s != '\0' && *s != ',' && *s != ')')
            s++;
        if (*s == ')')
            s++;
        static const int16_t linear[4] = { 0, 0, 1000, 1000 };
        memcpy(curve, linear, sizeof(linear));
        *p = s;
        return true;
    }
    for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++)
    {
        if (word_is(s, keywords[k].name))
        {
            memcpy(curve, keywords[k].curve, sizeof(keywords[k].curve));
            *p = s + strlen(keywords[k].name);
            return true;
        }
    }
    return false;
}

/* transition-property: a, b, ... */
static void transition_properties(style_t* st, const char* v)
{
    const char* s = skip(v);
    st->transition_count = 0;
    while (*s != '\0' && st->transition_count < MAX_TRANSITIONS)
    {
        s = skip(s);
        const char* start = s;
        while (*s != '\0' && *s != ',' && !is_space(*s))
            s++;
        uint8_t prop = transition_property(start, (size_t)(s - start));
        if (prop != TRANSITION_NONE)
            st->transition_props[st->transition_count++] = prop;
        while (*s != '\0' && *s != ',')
            s++;
        if (*s == ',')
            s++;
    }
}

static void transition_durations(style_t* st, const char* v)
{
    const char* s = v;
    st->duration_count = 0;
    while (*s != '\0' && st->duration_count < MAX_TRANSITIONS)
    {
        if (!parse_time(&s, &st->transition_ms[st->duration_count]))
            break;
        st->duration_count++;
        s = skip(s);
        if (*s != ',')
            break;
        s++;
    }
}

static void transition_timings(style_t* st, const char* v)
{
    const char* s = v;
    st->timing_count = 0;
    while (*s != '\0' && st->timing_count < MAX_TRANSITIONS)
    {
        if (!parse_timing(&s, st->transition_easing[st->timing_count]))
            break;
        st->timing_count++;
        s = skip(s);
        if (*s != ',')
            break;
        s++;
    }
}

/* transition: property duration [timing] [delay], ... */
static void transition_shorthand(style_t* st, const char* v)
{
    static const int16_t ease[4] = { 250, 100, 250, 1000 };
    const char* s = skip(v);
    st->transition_count = st->duration_count = st->timing_count = 0;
    if (word_is(s, "none"))
        return;
    while (*s != '\0' && st->transition_count < MAX_TRANSITIONS)
    {
        uint8_t prop = TRANSITION_ALL;
        uint16_t ms = 0, delay;
        int16_t curve[4];
        bool has_time = false, has_timing = false;
        memcpy(curve, ease, sizeof(curve));
        while (*s != '\0' && *s != ',')
        {
            s = skip(s);
            if (*s == '\0' || *s == ',')
                break;
            if (!has_time && parse_time(&s, &ms))
                has_time = true;
            else if (has_time && parse_time(&s, &delay))
                continue;
            else if (!has_timing && parse_timing(&s, curve))
                has_timing = true;
            else
            {
                const char* start = s;
                while (*s != '\0' && *s != ',' && !is_space(*s))
                    s++;
                prop = transition_property(start, (size_t)(s - start));
            }
        }
        uint8_t i = st->transition_count++;
        st->transition_props[i] = prop;
        st->transition_ms[i] = ms;
        memcpy(st->transition_easing[i], curve, sizeof(curve));
        st->duration_count = st->timing_count = st->transition_count;
        if (*s == ',')
            s++;
    }
}

bool style_transition(const style_t* st, uint8_t what, uint16_t* ms, int16_t* easing)
{
    static const int16_t ease[4] = { 250, 100, 250, 1000 };
    for (uint8_t i = 0; i < st->transition_count; i++)
    {
        if (st->transition_props[i] != what && st->transition_props[i] != TRANSITION_ALL)
            continue;
        *ms = (st->duration_count > 0) ? st->transition_ms[i % st->duration_count] : 0;
        if (st->timing_count > 0)
            memcpy(easing, st->transition_easing[i % st->timing_count], 4 * sizeof(int16_t));
        else
            memcpy(easing, ease, sizeof(ease));
        return *ms > 0;
    }
    return false;
}

/* ---- Applying a declaration ---- */

typedef struct
{
    conv_t*         c;
    style_t*        st;
    const style_t*  parent;
    units_t         u;
    const char*     base;               /* Of the rule (url()) */
    bool            border_current[4];  /* border-color: currentColor */
} apply_t;

static void set_len(apply_t* a, len_t* field, const char* v, bool inherit_ok, const len_t* parent_value)
{
    const char* s = skip(v);
    if (word_is(s, "inherit") && parent_value != NULL)
    {
        *field = *parent_value;
        return;
    }
    (void)inherit_ok;
    len_t l;
    if (parse_len(&s, &a->u, &l, false))
        *field = l;
}

static void four_lengths(apply_t* a, len_t* fields, const char* v)
{
    const char* parts[4];
    size_t lengths[4];
    char buf[4][128];
    int n = split_values(v, parts, lengths, 4);
    if (n == 0)
        return;
    len_t l[4];
    for (int i = 0; i < n; i++)
    {
        size_t k = (lengths[i] < sizeof(buf[i])) ? lengths[i] : sizeof(buf[i]) - 1U;
        memcpy(buf[i], parts[i], k);
        buf[i][k] = '\0';
        const char* s = buf[i];
        if (!parse_len(&s, &a->u, &l[i], false))
            return;
    }
    fields[0] = l[0];
    fields[1] = (n > 1) ? l[1] : l[0];
    fields[2] = (n > 2) ? l[2] : l[0];
    fields[3] = (n > 3) ? l[3] : fields[1];
}

static int32_t border_width_of(apply_t* a, const char* v)
{
    const char* s = skip(v);
    if (word_is(s, "thin"))
        return 1 * U;
    if (word_is(s, "medium"))
        return 3 * U;
    if (word_is(s, "thick"))
        return 5 * U;
    int32_t px = 0;
    return parse_px(s, &a->u, &px) ? px : -1;
}

/* border: width style color, in any order (also of one side) */
static void border_shorthand(apply_t* a, const char* v, int side)
{
    const char* parts[3];
    size_t lengths[3];
    char buf[128];
    int n = split_values(v, parts, lengths, 3);
    int32_t width = 3 * U;
    uint32_t color = a->st->color;
    bool current = true, none = false;
    for (int i = 0; i < n; i++)
    {
        size_t k = (lengths[i] < sizeof(buf)) ? lengths[i] : sizeof(buf) - 1U;
        memcpy(buf, parts[i], k);
        buf[k] = '\0';
        const char* s = buf;
        int32_t w = border_width_of(a, buf);
        if (w >= 0)
            width = w;
        else if (word_is(s, "none") || word_is(s, "hidden"))
            none = true;
        else if (word_is(s, "solid") || word_is(s, "dashed") || word_is(s, "dotted") || word_is(s, "double") ||
                 word_is(s, "groove") || word_is(s, "ridge") || word_is(s, "inset") || word_is(s, "outset"))
            continue;
        else if (parse_color(&s, a->st->color, &color))
            current = word_is(buf, "currentcolor");
    }
    for (int i = 0; i < 4; i++)
    {
        if (side >= 0 && i != side)
            continue;
        a->st->border_w[i] = none ? 0 : width;
        a->st->border_color[i] = color;
        a->border_current[i] = current;
    }
}

/* font: [style] [weight] size[/line-height] family */
static void font_shorthand(apply_t* a, const char* v)
{
    const char* s = skip(v);
    for (int k = 0; k < 4; k++)
    {
        if (word_is(s, "italic") || word_is(s, "oblique"))
        {
            a->st->italic = true;
            s = skip(s + ((lower(*s) == 'i') ? 6 : 7));
        }
        else if (word_is(s, "normal"))
            s = skip(s + 6);
        else if (word_is(s, "bold"))
        {
            a->st->font_weight = 700;
            s = skip(s + 4);
        }
        else if (*s >= '1' && *s <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' && is_space(s[3]))
        {
            a->st->font_weight = (uint16_t)((s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0'));
            s = skip(s + 3);
        }
    }
    len_t size;
    if (!parse_len(&s, &a->u, &size, false) || size.kind != LEN_SET)
        return;
    a->st->font_size = len_resolve(size, a->parent->font_size);
    s = skip(s);
    if (*s == '/')
    {
        s = skip(s + 1);
        int64_t num;
        const char* q = s;
        if (number(&q, &num) && (is_space(*q) || *q == '\0'))
        {
            a->st->line_height_kind = LH_NUMBER;
            a->st->line_height = (int32_t)num;
            s = q;
        }
        else
        {
            len_t lh;
            if (parse_len(&s, &a->u, &lh, false) && lh.kind == LEN_SET)
            {
                a->st->line_height_kind = LH_LENGTH;
                a->st->line_height = len_resolve(lh, a->st->font_size);
            }
        }
    }
    s = skip(s);
    if (*s != '\0')
        a->st->font_family = arena_strndup(&a->c->arena, s, strlen(s));
}

/* "normal", 100 ... 900, bold, bolder, lighter */
static uint16_t font_weight(const char* v, uint16_t parent)
{
    const char* s = skip(v);
    if (word_is(s, "normal")) return 400;
    if (word_is(s, "bold")) return 700;
    if (word_is(s, "bolder")) return (parent < 400) ? 400 : (parent < 600) ? 700 : 900;
    if (word_is(s, "lighter")) return (parent < 600) ? 100 : (parent < 800) ? 400 : 700;
    int64_t n;
    if (number(&s, &n) && n >= 1000 && n <= 1000000)
        return (uint16_t)(n / 1000);
    return parent;
}

/* content: "..." "..." (escapes decoded), none, normal */
static void parse_content(apply_t* a, const char* v)
{
    const char* s = skip(v);
    char buf[256];
    size_t n = 0;
    a->st->has_content = false;
    if (word_is(s, "none") || word_is(s, "normal"))
        return;
    while (*s != '\0')
    {
        s = skip(s);
        if (*s != '"' && *s != '\'')
        {
            if (*s == '\0')
                break;
            while (*s != '\0' && !is_space(*s))
                s++;                    /* counter(), attr(), open-quote: not drawn */
            continue;
        }
        char q = *s++;
        const char* end = s + strlen(s);
        while (*s != '\0' && *s != q && n + 4U < sizeof(buf))
        {
            if (*s == '\\')
            {
                uint32_t cp = css_unescape(&s, end);
                char tmp[4];
                size_t k = 0;
                if (cp < 0x80u) tmp[k++] = (char)cp;
                else if (cp < 0x800u) { tmp[k++] = (char)(0xC0u | (cp >> 6)); tmp[k++] = (char)(0x80u | (cp & 0x3Fu)); }
                else if (cp < 0x10000u) { tmp[k++] = (char)(0xE0u | (cp >> 12)); tmp[k++] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); tmp[k++] = (char)(0x80u | (cp & 0x3Fu)); }
                else { tmp[k++] = (char)(0xF0u | (cp >> 18)); tmp[k++] = (char)(0x80u | ((cp >> 12) & 0x3Fu)); tmp[k++] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); tmp[k++] = (char)(0x80u | (cp & 0x3Fu)); }
                memcpy(buf + n, tmp, k);
                n += k;
                continue;
            }
            buf[n++] = *s++;
        }
        if (*s == q)
            s++;
        a->st->has_content = true;
    }
    if (a->st->has_content)
    {
        a->st->content = arena_strndup(&a->c->arena, buf, n);
        a->st->content_length = n;
    }
}

static void line_height(apply_t* a, const char* v)
{
    const char* s = skip(v);
    if (word_is(s, "inherit"))
    {
        a->st->line_height_kind = a->parent->line_height_kind;
        a->st->line_height = a->parent->line_height;
        return;
    }
    if (word_is(s, "normal"))
    {
        a->st->line_height_kind = LH_NORMAL;
        return;
    }
    int64_t n;
    const char* q = s;
    if (number(&q, &n) && *skip(q) == '\0')
    {
        a->st->line_height_kind = LH_NUMBER;
        a->st->line_height = (int32_t)n;
        return;
    }
    len_t l;
    if (parse_len(&s, &a->u, &l, false) && l.kind == LEN_SET)
    {
        a->st->line_height_kind = LH_LENGTH;
        a->st->line_height = len_resolve(l, a->st->font_size);
    }
}

static const char g_side_names[4][8] = { "top", "right", "bottom", "left" };
static const char g_corner_names[4][14] = { "top-left", "top-right", "bottom-right", "bottom-left" };

static void apply(apply_t* a, const char* name, const char* v)
{
    style_t* st = a->st;
    const style_t* pa = a->parent;
    const char* s = skip(v);
    bool inherit = word_is(s, "inherit");
    char buf[96];

    if (strcmp(name, "display") == 0) { st->display = display_keyword(v); return; }
    if (strcmp(name, "position") == 0)
    {
        st->position = word_is(s, "relative") || word_is(s, "sticky") ? POSITION_RELATIVE : word_is(s, "absolute") ? POSITION_ABSOLUTE
                     : word_is(s, "fixed") ? POSITION_FIXED : POSITION_STATIC;
        return;
    }
    if (strcmp(name, "box-sizing") == 0) { st->border_box = word_is(s, "border-box"); return; }
    if (strcmp(name, "visibility") == 0) { st->hidden = inherit ? pa->hidden : !word_is(s, "visible"); return; }
    if (strcmp(name, "z-index") == 0)
    {
        int64_t n;
        st->z_auto = !number(&s, &n);
        st->z_index = st->z_auto ? 0 : (int32_t)(n / 1000);
        return;
    }
    if (strcmp(name, "inset") == 0) { four_lengths(a, st->inset, v); return; }
    for (int i = 0; i < 4; i++)
    {
        if (strcmp(name, g_side_names[i]) == 0) { set_len(a, &st->inset[i], v, false, NULL); return; }
    }
    if (strcmp(name, "width") == 0) { set_len(a, &st->width, v, false, NULL); return; }
    if (strcmp(name, "height") == 0) { set_len(a, &st->height, v, false, NULL); return; }
    if (strcmp(name, "min-width") == 0) { set_len(a, &st->min_w, v, false, NULL); return; }
    if (strcmp(name, "min-height") == 0) { set_len(a, &st->min_h, v, false, NULL); return; }
    if (strcmp(name, "max-width") == 0) { set_len(a, &st->max_w, v, false, NULL); return; }
    if (strcmp(name, "max-height") == 0) { set_len(a, &st->max_h, v, false, NULL); return; }
    if (strcmp(name, "margin") == 0) { four_lengths(a, st->margin, v); return; }
    if (strcmp(name, "padding") == 0) { four_lengths(a, st->padding, v); return; }
    if (strcmp(name, "margin-inline") == 0 || strcmp(name, "margin-block") == 0 || strcmp(name, "padding-inline") == 0 ||
        strcmp(name, "padding-block") == 0)
    {
        /* One or two values: start and end of the inline (left, right) or block (top, bottom) axis */
        len_t* f = (name[0] == 'm') ? st->margin : st->padding;
        len_t two[4];
        bool inline_axis = name[(name[0] == 'm') ? 7 : 8] == 'i';
        four_lengths(a, two, v);
        f[inline_axis ? 3 : 0] = two[0];
        f[inline_axis ? 1 : 2] = two[1];
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        Dmod_SnPrintf(buf, sizeof(buf), "margin-%s", g_side_names[i]);
        if (strcmp(name, buf) == 0) { set_len(a, &st->margin[i], v, false, NULL); return; }
        Dmod_SnPrintf(buf, sizeof(buf), "padding-%s", g_side_names[i]);
        if (strcmp(name, buf) == 0) { set_len(a, &st->padding[i], v, false, NULL); return; }
    }

    /* Borders */
    if (strcmp(name, "border") == 0) { border_shorthand(a, v, -1); return; }
    if (strcmp(name, "border-width") == 0)
    {
        const char* parts[4];
        size_t lengths[4];
        char w[4][32];
        int n = split_values(v, parts, lengths, 4);
        int32_t px[4];
        for (int i = 0; i < n; i++)
        {
            size_t k = (lengths[i] < sizeof(w[i])) ? lengths[i] : sizeof(w[i]) - 1U;
            memcpy(w[i], parts[i], k);
            w[i][k] = '\0';
            px[i] = border_width_of(a, w[i]);
            if (px[i] < 0)
                return;
        }
        if (n == 0)
            return;
        st->border_w[0] = px[0];
        st->border_w[1] = (n > 1) ? px[1] : px[0];
        st->border_w[2] = (n > 2) ? px[2] : px[0];
        st->border_w[3] = (n > 3) ? px[3] : st->border_w[1];
        return;
    }
    if (strcmp(name, "border-color") == 0)
    {
        const char* parts[4];
        size_t lengths[4];
        char w[4][96];
        int n = split_values(v, parts, lengths, 4);
        uint32_t col[4];
        bool cur[4];
        for (int i = 0; i < n; i++)
        {
            size_t k = (lengths[i] < sizeof(w[i])) ? lengths[i] : sizeof(w[i]) - 1U;
            memcpy(w[i], parts[i], k);
            w[i][k] = '\0';
            const char* q = w[i];
            if (!parse_color(&q, st->color, &col[i]))
                return;
            cur[i] = word_is(w[i], "currentcolor");
        }
        if (n == 0)
            return;
        for (int i = 0; i < 4; i++)
        {
            int k = (i < n) ? i : (i == 3 && n > 1) ? 1 : 0;
            st->border_color[i] = col[k];
            a->border_current[i] = cur[k];
        }
        return;
    }
    if (strcmp(name, "border-style") == 0)
    {
        if (word_is(s, "none") || word_is(s, "hidden"))
            st->border_w[0] = st->border_w[1] = st->border_w[2] = st->border_w[3] = 0;
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        Dmod_SnPrintf(buf, sizeof(buf), "border-%s", g_side_names[i]);
        if (strcmp(name, buf) == 0) { border_shorthand(a, v, i); return; }
        Dmod_SnPrintf(buf, sizeof(buf), "border-%s-width", g_side_names[i]);
        if (strcmp(name, buf) == 0)
        {
            int32_t w = border_width_of(a, v);
            if (w >= 0)
                st->border_w[i] = w;
            return;
        }
        Dmod_SnPrintf(buf, sizeof(buf), "border-%s-color", g_side_names[i]);
        if (strcmp(name, buf) == 0)
        {
            const char* q = v;
            if (parse_color(&q, st->color, &st->border_color[i]))
                a->border_current[i] = word_is(skip(v), "currentcolor");
            return;
        }
        Dmod_SnPrintf(buf, sizeof(buf), "border-%s-radius", g_corner_names[i]);
        if (strcmp(name, buf) == 0) { set_len(a, &st->radius[i], v, false, NULL); return; }
    }
    if (strcmp(name, "border-radius") == 0)
    {
        char first[256];
        const char* slash = strchr(v, '/');
        size_t n = (slash != NULL) ? (size_t)(slash - v) : strlen(v);
        if (n >= sizeof(first))
            n = sizeof(first) - 1U;
        memcpy(first, v, n);
        first[n] = '\0';
        four_lengths(a, st->radius, first);
        return;
    }

    /* Flex and grid */
    if (strcmp(name, "flex-direction") == 0)
    {
        st->flex_direction = word_is(s, "row-reverse") ? FLEX_ROW_REVERSE : word_is(s, "column-reverse") ? FLEX_COLUMN_REVERSE
                           : word_is(s, "column") ? FLEX_COLUMN : FLEX_ROW;
        return;
    }
    if (strcmp(name, "flex-wrap") == 0) { st->flex_wrap = !word_is(s, "nowrap"); return; }
    if (strcmp(name, "flex-flow") == 0)
    {
        if (contains(v, "column"))
            st->flex_direction = contains(v, "reverse") ? FLEX_COLUMN_REVERSE : FLEX_COLUMN;
        else if (contains(v, "row"))
            st->flex_direction = contains(v, "row-reverse") ? FLEX_ROW_REVERSE : FLEX_ROW;
        st->flex_wrap = contains(v, "wrap") && !contains(v, "nowrap");
        return;
    }
    if (strcmp(name, "flex-grow") == 0 || strcmp(name, "flex-shrink") == 0)
    {
        int64_t n;
        if (number(&s, &n))
            *((name[5] == 'g') ? &st->grow : &st->shrink) = (int32_t)(n / 10);
        return;
    }
    if (strcmp(name, "flex-basis") == 0) { set_len(a, &st->basis, v, false, NULL); if (word_is(s, "content")) st->basis.kind = LEN_CONTENT; return; }
    if (strcmp(name, "flex") == 0)
    {
        if (word_is(s, "none")) { st->grow = 0; st->shrink = 0; st->basis = len_auto(); return; }
        if (word_is(s, "auto")) { st->grow = 100; st->shrink = 100; st->basis = len_auto(); return; }
        if (word_is(s, "initial")) { st->grow = 0; st->shrink = 100; st->basis = len_auto(); return; }
        int64_t g;
        const char* q = s;
        if (!number(&q, &g))
        {
            len_t b;
            if (parse_len(&s, &a->u, &b, false)) { st->grow = 100; st->shrink = 100; st->basis = b; }
            return;
        }
        st->grow = (int32_t)(g / 10);
        st->shrink = 100;
        st->basis = len_px(0);           /* flex: 1 -> 1 1 0% */
        q = skip(q);
        int64_t sh;
        const char* r = q;
        if (number(&r, &sh) && (is_space(*r) || *r == '\0'))
        {
            st->shrink = (int32_t)(sh / 10);
            q = skip(r);
        }
        if (*q != '\0')
        {
            len_t b;
            if (parse_len(&q, &a->u, &b, true))
                st->basis = b;
        }
        return;
    }
    if (strcmp(name, "order") == 0) { int64_t n; if (number(&s, &n)) st->order = (int32_t)(n / 1000); return; }
    if (strcmp(name, "justify-content") == 0) { st->justify_content = align_keyword(v); return; }
    if (strcmp(name, "align-items") == 0) { st->align_items = align_keyword(v); return; }
    if (strcmp(name, "align-self") == 0) { st->align_self = align_keyword(v); return; }
    if (strcmp(name, "align-content") == 0) { st->align_content = align_keyword(v); return; }
    if (strcmp(name, "justify-items") == 0) { st->justify_items = align_keyword(v); return; }
    if (strcmp(name, "justify-self") == 0) { st->justify_self = align_keyword(v); return; }
    if (strcmp(name, "place-items") == 0) { st->align_items = st->justify_items = align_keyword(v); return; }
    if (strcmp(name, "place-content") == 0) { st->align_content = st->justify_content = align_keyword(v); return; }
    if (strcmp(name, "place-self") == 0) { st->align_self = st->justify_self = align_keyword(v); return; }
    if (strcmp(name, "gap") == 0 || strcmp(name, "grid-gap") == 0)
    {
        len_t two[4];
        four_lengths(a, two, v);
        st->row_gap = two[0];
        st->column_gap = two[1];
        return;
    }
    if (strcmp(name, "row-gap") == 0 || strcmp(name, "grid-row-gap") == 0) { set_len(a, &st->row_gap, v, false, NULL); return; }
    if (strcmp(name, "column-gap") == 0 || strcmp(name, "grid-column-gap") == 0) { set_len(a, &st->column_gap, v, false, NULL); return; }
    if (strcmp(name, "grid-template-columns") == 0) { parse_columns(st, v, &a->u); return; }
    if (strcmp(name, "grid-column") == 0) { st->column_span = parse_span(v); return; }
    if (strcmp(name, "grid-row") == 0) { st->row_span = parse_span(v); return; }

    /* Overflow */
    if (strcmp(name, "overflow") == 0)
    {
        const char* parts[2];
        size_t lengths[2];
        int n = split_values(v, parts, lengths, 2);
        st->overflow_x = overflow_keyword(v);
        st->overflow_y = (n > 1) ? overflow_keyword(parts[1]) : st->overflow_x;
        return;
    }
    if (strcmp(name, "overflow-x") == 0) { st->overflow_x = overflow_keyword(v); return; }
    if (strcmp(name, "overflow-y") == 0) { st->overflow_y = overflow_keyword(v); return; }

    /* Colors, backgrounds */
    if (strcmp(name, "color") == 0)
    {
        const char* q = v;
        if (inherit)
            st->color = pa->color;
        else
            (void)parse_color(&q, pa->color, &st->color);
        return;
    }
    if (strcmp(name, "opacity") == 0)
    {
        int64_t n;
        if (number(&s, &n))
        {
            if (*s == '%')
                n /= 100;
            st->opacity = (uint8_t)clamp255((n * 255 + 500) / 1000);
        }
        return;
    }
    if (strcmp(name, "background-color") == 0) { const char* q = v; (void)parse_color(&q, st->color, &st->background); return; }
    if (strcmp(name, "background-image") == 0) { st->background_image = parse_gradient(a->c, v, &a->u, st->color); return; }
    if (strcmp(name, "background") == 0)
    {
        st->background = 0;
        st->background_image = NULL;
        const char* q = s;
        if (word_is(s, "none"))
            return;
        st->background_image = parse_gradient(a->c, v, &a->u, st->color);
        if (st->background_image != NULL)
            return;
        /* A color, wherever it is among the other values */
        const char* parts[6];
        size_t lengths[6];
        char w[96];
        int n = split_values(v, parts, lengths, 6);
        for (int i = 0; i < n; i++)
        {
            size_t k = (lengths[i] < sizeof(w)) ? lengths[i] : sizeof(w) - 1U;
            memcpy(w, parts[i], k);
            w[k] = '\0';
            q = w;
            if (parse_color(&q, st->color, &st->background))
                return;
        }
        return;
    }

    /* Text */
    if (strcmp(name, "font-family") == 0) { st->font_family = inherit ? pa->font_family : arena_strndup(&a->c->arena, s, strlen(s)); return; }
    if (strcmp(name, "font-weight") == 0) { st->font_weight = font_weight(v, pa->font_weight); return; }
    if (strcmp(name, "font-style") == 0) { st->italic = word_is(s, "italic") || word_is(s, "oblique"); return; }
    if (strcmp(name, "font") == 0) { font_shorthand(a, v); return; }
    if (strcmp(name, "line-height") == 0) { line_height(a, v); return; }
    if (strcmp(name, "letter-spacing") == 0)
    {
        int32_t px = 0;
        st->letter_spacing = inherit ? pa->letter_spacing : word_is(s, "normal") ? 0 : parse_px(v, &a->u, &px) ? px : st->letter_spacing;
        return;
    }
    if (strcmp(name, "text-align") == 0)
    {
        st->text_align = inherit ? pa->text_align : word_is(s, "center") ? TEXT_CENTER : (word_is(s, "right") || word_is(s, "end")) ? TEXT_RIGHT
                       : word_is(s, "justify") ? TEXT_JUSTIFY : TEXT_LEFT;
        return;
    }
    if (strcmp(name, "text-transform") == 0)
    {
        st->text_transform = inherit ? pa->text_transform : word_is(s, "uppercase") ? CASE_UPPER : word_is(s, "lowercase") ? CASE_LOWER
                           : word_is(s, "capitalize") ? CASE_CAPITALIZE : CASE_NONE;
        return;
    }
    if (strcmp(name, "white-space") == 0)
    {
        st->white_space = inherit ? pa->white_space : word_is(s, "nowrap") ? WS_NOWRAP : word_is(s, "pre-wrap") ? WS_PRE_WRAP
                        : word_is(s, "pre-line") ? WS_PRE_LINE : word_is(s, "pre") ? WS_PRE : WS_NORMAL;
        return;
    }
    if (strcmp(name, "vertical-align") == 0)
    {
        st->valign = word_is(s, "middle") ? VALIGN_MIDDLE : word_is(s, "top") ? VALIGN_TOP : word_is(s, "bottom") ? VALIGN_BOTTOM
                   : word_is(s, "text-top") ? VALIGN_TEXT_TOP : word_is(s, "text-bottom") ? VALIGN_TEXT_BOTTOM : VALIGN_BASELINE;
        if (word_is(s, "sub") || word_is(s, "super"))
        {
            st->valign = VALIGN_LENGTH;
            st->valign_by = (lower(s[1]) == 'u' && lower(s[2]) == 'b') ? -st->font_size / 5 : st->font_size / 3;
        }
        len_t l;
        const char* q = s;
        if (st->valign == VALIGN_BASELINE && !word_is(s, "baseline") && parse_len(&q, &a->u, &l, false) && l.kind == LEN_SET)
        {
            st->valign = VALIGN_LENGTH;
            st->valign_by = len_resolve(l, st->font_size);     /* % of the line height: as of the font size */
        }
        return;
    }

    /* Effects */
    if (strcmp(name, "box-shadow") == 0) { parse_shadows(v, &a->u, st->color, st->shadows, &st->shadow_count, false); return; }
    if (strcmp(name, "filter") == 0) { parse_filter(st, v, &a->u); return; }
    if (strcmp(name, "transform") == 0) { parse_transform(st, v, &a->u); return; }
    if (strcmp(name, "translate") == 0)
    {
        const char* q = s;
        len_t x = len_px(0), y = len_px(0);
        if (parse_len(&q, &a->u, &x, false))
            (void)parse_len(&q, &a->u, &y, false);
        st->translate_x = x;
        st->translate_y = y;
        return;
    }
    if (strcmp(name, "content") == 0) { parse_content(a, v); return; }
    if (strcmp(name, "transition") == 0) { transition_shorthand(st, v); return; }
    if (strcmp(name, "transition-property") == 0) { transition_properties(st, v); return; }
    if (strcmp(name, "transition-duration") == 0) { transition_durations(st, v); return; }
    if (strcmp(name, "transition-timing-function") == 0) { transition_timings(st, v); return; }
}

/* ---- Custom properties ---- */

static const char* var_value(const var_t* vars, const char* name, size_t n)
{
    for (const var_t* v = vars; v != NULL; v = v->next)
    {
        if (strlen(v->name) == n && strncmp(v->name, name, n) == 0)
            return v->value;
    }
    return NULL;
}

/* `v` with every var() replaced (into out); false when one has neither a value nor a fallback */
static bool substitute(const var_t* vars, const char* v, char* out, size_t size, size_t* used, uint32_t depth)
{
    if (depth > MAX_VAR_DEPTH)
        return false;
    for (const char* s = v; *s != '\0'; )
    {
        if (strncmp(s, "var(", 4) == 0)
        {
            const char* name = skip(s + 4);
            const char* name_end = name;
            while (*name_end != '\0' && *name_end != ',' && *name_end != ')' && !is_space(*name_end))
                name_end++;
            /* The closing parenthesis, and the fallback after the first comma */
            const char* p = name_end;
            const char* fallback = NULL;
            int d = 1;
            while (*p != '\0' && d > 0)
            {
                if (*p == '(')
                    d++;
                else if (*p == ')')
                    d--;
                else if (*p == ',' && d == 1 && fallback == NULL)
                    fallback = p + 1;
                if (d > 0)
                    p++;
            }
            const char* value = var_value(vars, name, (size_t)(name_end - name));
            char inner[MAX_VALUE];
            if (value == NULL && fallback != NULL)
            {
                size_t n = (size_t)(p - fallback);
                if (n >= sizeof(inner))
                    return false;
                memcpy(inner, fallback, n);
                inner[n] = '\0';
                value = inner;
            }
            if (value == NULL)
                return false;
            if (!substitute(vars, value, out, size, used, depth + 1U))
                return false;
            s = (*p == ')') ? p + 1 : p;
            continue;
        }
        if (*used + 1U >= size)
            return false;
        out[(*used)++] = *s++;
    }
    out[*used] = '\0';
    return true;
}

static void set_var(conv_t* c, style_t* st, const char* name, const char* value)
{
    var_t* v = arena_alloc(&c->arena, sizeof(*v));
    if (v == NULL)
        return;
    v->name = name;
    v->value = value;
    v->next = st->vars;                 /* In front of the inherited ones: found first */
    st->vars = v;
}

/* ---- The cascade ---- */

typedef struct
{
    const decl_t*   decl;
    uint32_t        specificity;
    uint32_t        order;
    uint8_t         layer;
    const char*     base;
} matched_t;

typedef struct
{
    const char*     name;
    decl_t*         decls;
    uint32_t        count;
    uint32_t        rank;
} tw_entry_t;

/* What computing styles needs besides the arena: in the conversion (c->style_work) */
typedef struct
{
    tw_entry_t      cache[TW_CACHE];        /* Tailwind's classes, made once each */
    matched_t       matched[MAX_MATCHED];
    const rule_t*   seen[MAX_MATCHED];
    char            value[MAX_VALUE];
} work_t;

static bool before(const matched_t* a, const matched_t* b)
{
    if (a->decl->important != b->decl->important)
        return !a->decl->important;
    uint8_t la = (a->layer == LAYER_UA) ? 0 : 1, lb = (b->layer == LAYER_UA) ? 0 : 1;
    if (la != lb)
        return a->decl->important ? la > lb : la < lb;
    if (a->specificity != b->specificity)
        return a->specificity < b->specificity;
    return a->order < b->order;
}

static uint32_t add_rule_decls(matched_t* m, uint32_t n, const rule_t* r)
{
    for (uint16_t i = 0; i < r->decl_count && n < MAX_MATCHED; i++)
    {
        m[n].decl = &r->decls[i];
        m[n].specificity = r->specificity;
        m[n].order = r->order;
        m[n].layer = r->layer;
        m[n].base = r->base;
        n++;
    }
    return n;
}

static uint32_t match_list(const rule_ref_t* list, const node_t* n, uint8_t pseudo, matched_t* m, uint32_t count,
                           const rule_t** seen, uint32_t* seen_count)
{
    for (const rule_ref_t* ref = list; ref != NULL; ref = ref->next)
    {
        const rule_t* r = ref->rule;
        if (r->pseudo_element != pseudo || !css_matches(r, n))
            continue;
        bool dup = false;
        for (uint32_t k = 0; k < *seen_count && !dup; k++)
            dup = seen[k] == r;
        if (dup)
            continue;
        if (*seen_count < MAX_MATCHED)
            seen[(*seen_count)++] = r;
        count = add_rule_decls(m, count, r);
    }
    return count;
}

static tw_entry_t* tailwind_entry(conv_t* c, const char* name)
{
    uint32_t h = 2166136261u;
    for (const char* s = name; *s != '\0'; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    for (uint32_t i = 0; i < TW_CACHE; i++)
    {
        tw_entry_t* e = &((work_t*)c->style_work)->cache[(h + i) % TW_CACHE];
        if (e->name == NULL)
        {
            decl_t decls[MAX_TW_DECLS];
            e->name = name;
            e->count = tailwind_class(c, name, decls, MAX_TW_DECLS, &e->rank);
            if (e->count > 0 && (e->decls = arena_alloc(&c->arena, e->count * sizeof(decl_t))) != NULL)
                memcpy(e->decls, decls, e->count * sizeof(decl_t));
            else
                e->count = 0;
            return e;
        }
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

/* The declarations that apply to n (or its pseudo-element), in cascade order */
static uint32_t collect(conv_t* c, const node_t* n, uint8_t pseudo, matched_t* m)
{
    const rule_t** seen = ((work_t*)c->style_work)->seen;
    uint32_t seen_count = 0, count = 0;
    if (n->id != NULL)
        count = match_list(css_bucket(c, "#", n->id), n, pseudo, m, count, seen, &seen_count);
    for (uint32_t i = 0; i < n->class_count; i++)
        count = match_list(css_bucket(c, ".", n->classes[i]), n, pseudo, m, count, seen, &seen_count);
    count = match_list(css_bucket(c, "", n->tag), n, pseudo, m, count, seen, &seen_count);
    count = match_list(c->universal, n, pseudo, m, count, seen, &seen_count);

    if (pseudo == PSEUDO_NONE)
    {
        if (c->tailwind)
        {
            for (uint32_t i = 0; i < n->class_count; i++)
            {
                tw_entry_t* e = tailwind_entry(c, n->classes[i]);
                for (uint32_t k = 0; e != NULL && k < e->count && count < MAX_MATCHED; k++)
                {
                    m[count].decl = &e->decls[k];
                    m[count].specificity = 1U << 8;
                    m[count].order = ORDER_TAILWIND + e->rank;
                    m[count].layer = LAYER_TAILWIND;
                    m[count].base = c->path;
                    count++;
                }
            }
        }
        const char* inline_style = node_attr(n, "style");
        if (inline_style != NULL)
        {
            decl_t* decls = NULL;
            uint16_t k = css_parse_decls(c, inline_style, strlen(inline_style), &decls);
            for (uint16_t i = 0; i < k && count < MAX_MATCHED; i++)
            {
                m[count].decl = &decls[i];
                m[count].specificity = SPEC_INLINE;
                m[count].order = 0;
                m[count].layer = LAYER_AUTHOR;
                m[count].base = c->path;
                count++;
            }
        }
    }

    /* Insertion sort: stable, and the lists are short */
    for (uint32_t i = 1; i < count; i++)
    {
        matched_t x = m[i];
        uint32_t j = i;
        while (j > 0 && before(&x, &m[j - 1U]))
        {
            m[j] = m[j - 1U];
            j--;
        }
        m[j] = x;
    }
    return count;
}

/* What an element starts with: the inherited values of its parent, the initial values of the others */
static void initial(style_t* st, const style_t* pa, int32_t vw)
{
    (void)vw;
    memset(st, 0, sizeof(*st));
    st->display = DISPLAY_INLINE;
    st->z_auto = true;
    st->width = st->height = len_auto();
    st->min_w = st->min_h = len_auto();
    st->max_w.kind = st->max_h.kind = LEN_NONE;
    for (int i = 0; i < 4; i++)
    {
        st->inset[i] = len_auto();
        st->margin[i] = len_px(0);
        st->padding[i] = len_px(0);
        st->radius[i] = len_px(0);
    }
    st->shrink = 100;
    st->basis = len_auto();
    st->row_gap.kind = st->column_gap.kind = LEN_NONE;
    st->column_span = st->row_span = 1;
    st->opacity = 255;
    st->translate_x = st->translate_y = len_px(0);
    if (pa != NULL)
    {
        st->color = pa->color;
        st->font_family = pa->font_family;
        st->font_size = pa->font_size;
        st->font_weight = pa->font_weight;
        st->italic = pa->italic;
        st->line_height_kind = pa->line_height_kind;
        st->line_height = pa->line_height;
        st->letter_spacing = pa->letter_spacing;
        st->text_align = pa->text_align;
        st->text_transform = pa->text_transform;
        st->white_space = pa->white_space;
        st->hidden = pa->hidden;
        st->vars = pa->vars;
    }
    else
    {
        st->color = 0xFF000000u;
        st->font_size = 16 * U;
        st->font_weight = 400;
    }
    for (int i = 0; i < 4; i++)
        st->border_color[i] = st->color;
}

/* A line height given in a number or % stays relative to the font size of
 * the element that inherits it - only a length is inherited as it is */
static style_t* compute(conv_t* c, const node_t* n, uint8_t pseudo, const style_t* pa)
{
    matched_t* m = ((work_t*)c->style_work)->matched;
    char* value = ((work_t*)c->style_work)->value;
    style_t* st = arena_alloc(&c->arena, sizeof(*st));
    if (st == NULL)
        return NULL;
    initial(st, pa, c->vw);
    apply_t a = { c, st, (pa != NULL) ? pa : st, { (pa != NULL) ? pa->font_size : 16 * U, c->vw, c->vh }, c->path, { true, true, true, true } };
    uint32_t count = collect(c, n, pseudo, m);

    /* Custom properties first: every value may use them */
    for (uint32_t i = 0; i < count; i++)
    {
        const decl_t* d = m[i].decl;
        if (d->name[0] == '-' && d->name[1] == '-')
            set_var(c, st, d->name, d->value);
    }
    /* The font size next (em is relative to it), then everything else */
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            const decl_t* d = m[i].decl;
            bool font = strcmp(d->name, "font-size") == 0 || strcmp(d->name, "font") == 0;
            if ((d->name[0] == '-' && d->name[1] == '-') || font != (pass == 0))
                continue;
            size_t used = 0;
            const char* v = d->value;
            if (contains(v, "var("))
            {
                if (!substitute(st->vars, v, value, MAX_VALUE, &used, 0))
                    continue;           /* Invalid at computed-value time: as if not declared */
                v = value;
            }
            a.base = m[i].base;
            if (pass == 0)
            {
                if (strcmp(d->name, "font") == 0)
                {
                    font_shorthand(&a, v);
                    continue;
                }
                const char* s = skip(v);
                len_t l;
                int32_t parent_size = a.parent->font_size;
                units_t pu = { parent_size, c->vw, c->vh };
                if (word_is(s, "inherit"))
                    st->font_size = parent_size;
                else if (word_is(s, "smaller"))
                    st->font_size = parent_size * 5 / 6;
                else if (word_is(s, "larger"))
                    st->font_size = parent_size * 6 / 5;
                else if (parse_len(&s, &pu, &l, false) && l.kind == LEN_SET)
                    st->font_size = len_resolve(l, parent_size);
                else
                {
                    static const struct { char name[10]; int32_t px; } sizes[] = {
                        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
                        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
                    };
                    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
                    {
                        if (word_is(s, sizes[k].name))
                            st->font_size = sizes[k].px * U;
                    }
                }
                a.u.font_size = st->font_size;
                continue;
            }
            apply(&a, d->name, v);
        }
        a.u.font_size = st->font_size;
    }
    for (int i = 0; i < 4; i++)
    {
        if (a.border_current[i])
            st->border_color[i] = st->color;
    }
    return st;
}

/* ---- The tree ---- */

static node_t* new_child(conv_t* c, node_t* parent, node_t* before_node, uint8_t pseudo)
{
    node_t* n = arena_alloc(&c->arena, sizeof(*n));
    if (n == NULL)
        return NULL;
    n->kind = NODE_ELEMENT;
    n->pseudo = pseudo;
    n->tag = (pseudo == PSEUDO_BEFORE) ? "::before" : (pseudo == PSEUDO_AFTER) ? "::after" : "::anonymous";
    n->parent = parent;
    if (before_node == NULL)
    {
        n->prev = parent->last;
        if (parent->last != NULL)
            parent->last->next = n;
        else
            parent->first = n;
        parent->last = n;
    }
    else
    {
        n->next = before_node;
        n->prev = before_node->prev;
        if (before_node->prev != NULL)
            before_node->prev->next = n;
        else
            parent->first = n;
        before_node->prev = n;
    }
    return n;
}

static void add_text_child(conv_t* c, node_t* parent, const char* text, size_t length)
{
    node_t* t = arena_alloc(&c->arena, sizeof(*t));
    if (t == NULL)
        return;
    t->kind = NODE_TEXT;
    t->text = arena_strndup(&c->arena, text, length);
    t->length = length;
    t->parent = parent;
    parent->first = parent->last = t;
    t->style = parent->style;
}

/* ::before / ::after of n, when a rule gives one a content */
static void pseudo_element(conv_t* c, node_t* n, uint8_t pseudo)
{
    style_t* st = compute(c, n, pseudo, n->style);
    if (st == NULL || !st->has_content || st->display == DISPLAY_NONE || st->content_length == 0)
        return;
    node_t* p = new_child(c, n, (pseudo == PSEUDO_BEFORE) ? n->first : NULL, pseudo);
    if (p == NULL)
        return;
    p->style = st;
    add_text_child(c, p, st->content, st->content_length);
}

static bool all_space(const node_t* t)
{
    for (size_t i = 0; i < t->length; i++)
    {
        if (!is_space(t->text[i]))
            return false;
    }
    return true;
}

/* Text of a flex or grid container in anonymous blocks; its whitespace dropped */
static void wrap_text(conv_t* c, node_t* n)
{
    for (node_t* k = n->first; k != NULL; )
    {
        node_t* next = k->next;
        if (k->kind == NODE_TEXT)
        {
            if (all_space(k))
            {
                if (k->prev != NULL)
                    k->prev->next = k->next;
                else
                    n->first = k->next;
                if (k->next != NULL)
                    k->next->prev = k->prev;
                else
                    n->last = k->prev;
            }
            else
            {
                node_t* anon = new_child(c, n, k, PSEUDO_ANONYMOUS);
                if (anon == NULL)
                    return;
                /* k moves into it */
                anon->next = k->next;
                if (k->next != NULL)
                    k->next->prev = anon;
                else
                    n->last = anon;
                k->prev = k->next = NULL;
                k->parent = anon;
                anon->first = anon->last = k;
                anon->style = arena_alloc(&c->arena, sizeof(style_t));
                if (anon->style != NULL)
                {
                    initial(anon->style, n->style, c->vw);
                    anon->style->display = DISPLAY_BLOCK;
                }
                k->style = anon->style;
            }
        }
        k = next;
    }
}

static bool is_flex_or_grid(uint8_t d)
{
    return d == DISPLAY_FLEX || d == DISPLAY_INLINE_FLEX || d == DISPLAY_GRID || d == DISPLAY_INLINE_GRID;
}

/* Blockified: an item of a flex or grid container, positioned out of the flow, the root */
static uint8_t blockify(uint8_t d)
{
    switch (d)
    {
        case DISPLAY_INLINE:
        case DISPLAY_INLINE_BLOCK:
            return DISPLAY_BLOCK;
        case DISPLAY_INLINE_FLEX:
            return DISPLAY_FLEX;
        case DISPLAY_INLINE_GRID:
            return DISPLAY_GRID;
        default:
            return d;
    }
}

static void compute_tree(conv_t* c, node_t* n, const style_t* pa, uint32_t depth)
{
    if (depth > 200U || c->arena.failed)
        return;
    if (n->kind == NODE_TEXT)
    {
        n->style = (style_t*)pa;
        return;
    }
    if (n->kind == NODE_ELEMENT && n->pseudo == PSEUDO_NONE)
    {
        n->style = compute(c, n, PSEUDO_NONE, pa);
        if (n->style == NULL)
            return;
        style_t* st = n->style;
        bool root = n->parent != NULL && n->parent->kind == NODE_DOCUMENT;
        bool item = n->parent != NULL && n->parent->style != NULL && is_flex_or_grid(n->parent->style->display);
        if (root || item || st->position == POSITION_ABSOLUTE || st->position == POSITION_FIXED)
            st->display = blockify(st->display);
        if (st->display == DISPLAY_CONTENTS)
            st->display = DISPLAY_BLOCK;
        if (st->display == DISPLAY_NONE)
            return;
        pseudo_element(c, n, PSEUDO_BEFORE);
        pseudo_element(c, n, PSEUDO_AFTER);
        if (is_flex_or_grid(st->display))
            wrap_text(c, n);
        pa = st;
    }
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind == NODE_ELEMENT && k->pseudo != PSEUDO_NONE)
        {
            /* Its style is made: only its blockification and its text */
            if (k->style != NULL && n->style != NULL && is_flex_or_grid(n->style->display))
                k->style->display = blockify(k->style->display);
            for (node_t* t = k->first; t != NULL; t = t->next)
                t->style = k->style;
            continue;
        }
        compute_tree(c, k, pa, depth + 1U);
    }
}

void style_compute(conv_t* c, node_t* root)
{
    if ((c->style_work = arena_alloc(&c->arena, sizeof(work_t))) == NULL)
        return;
    css_parse(c, g_user_agent, sizeof(g_user_agent) - 1U, c->path, LAYER_UA);
    compute_tree(c, root, NULL, 0);
}
