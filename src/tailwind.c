#include "private.h"
#include <stddef.h>
#include <string.h>

/*
 * Tailwind CSS: a page that loads its Play CDN gets the CSS of its classes
 * the way Tailwind v3 makes it - here, for every class an element has, its
 * declarations, and its rank: the order Tailwind's utilities have in the
 * style sheet it generates (padding before px before pt, font sizes before
 * line heights, ...), which decides between two utilities of an element.
 *
 * Gradients, shadows, filters and transforms are Tailwind's custom
 * properties (--tw-gradient-stops, --tw-shadow, ...), as in its CSS, so
 * from-*, to-*, shadow-<color> work as there. Variants: sm: ... 2xl: hold
 * at their viewport widths; hover:, focus:, dark:, ... never in a still
 * page. Arbitrary values ([454px], [#123456], [color:red]) and the opacity
 * modifier (/10, /[0.15]) are understood.
 *
 * The tables hold text, not pointers: a module's data is not relocated
 * when it is loaded (only its GOT is).
 */

#define MAX_VALUE       512u

/* The order of Tailwind's utilities (its core plugins), and sub-ranks */
enum
{
    R_SR_ONLY = 1, R_POINTER_EVENTS, R_VISIBILITY, R_POSITION, R_INSET, R_INSET_XY, R_INSET_SIDE, R_Z, R_ORDER,
    R_GRID_COLUMN, R_GRID_ROW, R_MARGIN, R_MARGIN_XY, R_MARGIN_SIDE, R_BOX_SIZING, R_DISPLAY, R_SIZE, R_HEIGHT,
    R_MAX_HEIGHT, R_MIN_HEIGHT, R_WIDTH, R_MIN_WIDTH, R_MAX_WIDTH, R_FLEX, R_SHRINK, R_GROW, R_BASIS, R_TRANSLATE,
    R_ROTATE, R_SCALE, R_TRANSFORM, R_CURSOR, R_GRID_COLS, R_GRID_ROWS, R_FLEX_DIRECTION, R_FLEX_WRAP,
    R_PLACE_CONTENT, R_PLACE_ITEMS, R_ALIGN_CONTENT, R_ALIGN_ITEMS, R_JUSTIFY_CONTENT, R_JUSTIFY_ITEMS, R_GAP,
    R_GAP_XY, R_PLACE_SELF, R_ALIGN_SELF, R_JUSTIFY_SELF, R_OVERFLOW, R_OVERFLOW_XY, R_TEXT_OVERFLOW, R_WHITESPACE,
    R_ROUNDED, R_ROUNDED_SIDE, R_ROUNDED_CORNER, R_BORDER_WIDTH, R_BORDER_WIDTH_XY, R_BORDER_WIDTH_SIDE,
    R_BORDER_STYLE, R_BORDER_COLOR, R_BORDER_COLOR_XY, R_BORDER_COLOR_SIDE, R_BG_COLOR, R_BG_IMAGE, R_BG_SIZE, R_BG_POSITION, R_OBJECT_FIT, R_OBJECT_POSITION, R_GRADIENT_FROM,
    R_GRADIENT_VIA, R_GRADIENT_TO, R_PADDING, R_PADDING_XY, R_PADDING_SIDE, R_TEXT_ALIGN, R_VERTICAL_ALIGN,
    R_FONT_FAMILY, R_FONT_SIZE, R_FONT_WEIGHT, R_TEXT_TRANSFORM, R_FONT_STYLE, R_LEADING, R_TRACKING, R_TEXT_COLOR,
    R_OPACITY, R_SHADOW, R_SHADOW_COLOR, R_BLUR, R_DROP_SHADOW, R_FILTER, R_TRANSITION, R_DURATION, R_EASE, R_ARBITRARY
};

static const char g_shades[11][4] = { "50", "100", "200", "300", "400", "500", "600", "700", "800", "900", "950" };

static const struct
{
    char        name[8];
    uint32_t    colors[11];
} g_palette[] = {
    { "slate",   { 0xf8fafc, 0xf1f5f9, 0xe2e8f0, 0xcbd5e1, 0x94a3b8, 0x64748b, 0x475569, 0x334155, 0x1e293b, 0x0f172a, 0x020617 } },
    { "gray",    { 0xf9fafb, 0xf3f4f6, 0xe5e7eb, 0xd1d5db, 0x9ca3af, 0x6b7280, 0x4b5563, 0x374151, 0x1f2937, 0x111827, 0x030712 } },
    { "zinc",    { 0xfafafa, 0xf4f4f5, 0xe4e4e7, 0xd4d4d8, 0xa1a1aa, 0x71717a, 0x52525b, 0x3f3f46, 0x27272a, 0x18181b, 0x09090b } },
    { "neutral", { 0xfafafa, 0xf5f5f5, 0xe5e5e5, 0xd4d4d4, 0xa3a3a3, 0x737373, 0x525252, 0x404040, 0x262626, 0x171717, 0x0a0a0a } },
    { "stone",   { 0xfafaf9, 0xf5f5f4, 0xe7e5e4, 0xd6d3d1, 0xa8a29e, 0x78716c, 0x57534e, 0x44403c, 0x292524, 0x1c1917, 0x0c0a09 } },
    { "red",     { 0xfef2f2, 0xfee2e2, 0xfecaca, 0xfca5a5, 0xf87171, 0xef4444, 0xdc2626, 0xb91c1c, 0x991b1b, 0x7f1d1d, 0x450a0a } },
    { "orange",  { 0xfff7ed, 0xffedd5, 0xfed7aa, 0xfdba74, 0xfb923c, 0xf97316, 0xea580c, 0xc2410c, 0x9a3412, 0x7c2d12, 0x431407 } },
    { "amber",   { 0xfffbeb, 0xfef3c7, 0xfde68a, 0xfcd34d, 0xfbbf24, 0xf59e0b, 0xd97706, 0xb45309, 0x92400e, 0x78350f, 0x451a03 } },
    { "yellow",  { 0xfefce8, 0xfef9c3, 0xfef08a, 0xfde047, 0xfacc15, 0xeab308, 0xca8a04, 0xa16207, 0x854d0e, 0x713f12, 0x422006 } },
    { "lime",    { 0xf7fee7, 0xecfccb, 0xd9f99d, 0xbef264, 0xa3e635, 0x84cc16, 0x65a30d, 0x4d7c0f, 0x3f6212, 0x365314, 0x1a2e05 } },
    { "green",   { 0xf0fdf4, 0xdcfce7, 0xbbf7d0, 0x86efac, 0x4ade80, 0x22c55e, 0x16a34a, 0x15803d, 0x166534, 0x14532d, 0x052e16 } },
    { "emerald", { 0xecfdf5, 0xd1fae5, 0xa7f3d0, 0x6ee7b7, 0x34d399, 0x10b981, 0x059669, 0x047857, 0x065f46, 0x064e3b, 0x022c22 } },
    { "teal",    { 0xf0fdfa, 0xccfbf1, 0x99f6e4, 0x5eead4, 0x2dd4bf, 0x14b8a6, 0x0d9488, 0x0f766e, 0x115e59, 0x134e4a, 0x042f2e } },
    { "cyan",    { 0xecfeff, 0xcffafe, 0xa5f3fc, 0x67e8f9, 0x22d3ee, 0x06b6d4, 0x0891b2, 0x0e7490, 0x155e75, 0x164e63, 0x083344 } },
    { "sky",     { 0xf0f9ff, 0xe0f2fe, 0xbae6fd, 0x7dd3fc, 0x38bdf8, 0x0ea5e9, 0x0284c7, 0x0369a1, 0x075985, 0x0c4a6e, 0x082f49 } },
    { "blue",    { 0xeff6ff, 0xdbeafe, 0xbfdbfe, 0x93c5fd, 0x60a5fa, 0x3b82f6, 0x2563eb, 0x1d4ed8, 0x1e40af, 0x1e3a8a, 0x172554 } },
    { "indigo",  { 0xeef2ff, 0xe0e7ff, 0xc7d2fe, 0xa5b4fc, 0x818cf8, 0x6366f1, 0x4f46e5, 0x4338ca, 0x3730a3, 0x312e81, 0x1e1b4b } },
    { "violet",  { 0xf5f3ff, 0xede9fe, 0xddd6fe, 0xc4b5fd, 0xa78bfa, 0x8b5cf6, 0x7c3aed, 0x6d28d9, 0x5b21b6, 0x4c1d95, 0x2e1065 } },
    { "purple",  { 0xfaf5ff, 0xf3e8ff, 0xe9d5ff, 0xd8b4fe, 0xc084fc, 0xa855f7, 0x9333ea, 0x7e22ce, 0x6b21a8, 0x581c87, 0x3b0764 } },
    { "fuchsia", { 0xfdf4ff, 0xfae8ff, 0xf5d0fe, 0xf0abfc, 0xe879f9, 0xd946ef, 0xc026d3, 0xa21caf, 0x86198f, 0x701a75, 0x4a044e } },
    { "pink",    { 0xfdf2f8, 0xfce7f3, 0xfbcfe8, 0xf9a8d4, 0xf472b6, 0xec4899, 0xdb2777, 0xbe185d, 0x9d174d, 0x831843, 0x500724 } },
    { "rose",    { 0xfff1f2, 0xffe4e6, 0xfecdd3, 0xfda4af, 0xfb7185, 0xf43f5e, 0xe11d48, 0xbe123c, 0x9f1239, 0x881337, 0x4c0519 } },
};

/* Font sizes and their line heights (rem) */
static const struct
{
    char        name[5];
    char        size[10];
    char        line_height[8];
} g_font_sizes[] = {
    { "xs", "0.75rem", "1rem" }, { "sm", "0.875rem", "1.25rem" }, { "base", "1rem", "1.5rem" },
    { "lg", "1.125rem", "1.75rem" }, { "xl", "1.25rem", "1.75rem" }, { "2xl", "1.5rem", "2rem" },
    { "3xl", "1.875rem", "2.25rem" }, { "4xl", "2.25rem", "2.5rem" }, { "5xl", "3rem", "1" }, { "6xl", "3.75rem", "1" },
    { "7xl", "4.5rem", "1" }, { "8xl", "6rem", "1" }, { "9xl", "8rem", "1" },
};

typedef struct
{
    char        name[12];
    char        value[24];
} pair_t;

typedef struct
{
    char        name[8];
    char        value[192];
} long_pair_t;

static const pair_t g_weights[] = {
    { "thin", "100" }, { "extralight", "200" }, { "light", "300" }, { "normal", "400" }, { "medium", "500" },
    { "semibold", "600" }, { "bold", "700" }, { "extrabold", "800" }, { "black", "900" },
};

static const pair_t g_tracking[] = {
    { "tighter", "-0.05em" }, { "tight", "-0.025em" }, { "normal", "0em" }, { "wide", "0.025em" },
    { "wider", "0.05em" }, { "widest", "0.1em" },
};

static const pair_t g_leading[] = {
    { "none", "1" }, { "tight", "1.25" }, { "snug", "1.375" }, { "normal", "1.5" }, { "relaxed", "1.625" },
    { "loose", "2" }, { "3", ".75rem" }, { "4", "1rem" }, { "5", "1.25rem" }, { "6", "1.5rem" }, { "7", "1.75rem" },
    { "8", "2rem" }, { "9", "2.25rem" }, { "10", "2.5rem" },
};

static const pair_t g_radius[] = {
    { "none", "0px" }, { "sm", "0.125rem" }, { "", "0.25rem" }, { "md", "0.375rem" }, { "lg", "0.5rem" },
    { "xl", "0.75rem" }, { "2xl", "1rem" }, { "3xl", "1.5rem" }, { "full", "9999px" },
};

static const long_pair_t g_shadows[] = {
    { "sm", "0 1px 2px 0 rgb(0 0 0 / 0.05)|0 1px 2px 0 var(--tw-shadow-color)" },
    { "", "0 1px 3px 0 rgb(0 0 0 / 0.1), 0 1px 2px -1px rgb(0 0 0 / 0.1)|0 1px 3px 0 var(--tw-shadow-color), 0 1px 2px -1px var(--tw-shadow-color)" },
    { "md", "0 4px 6px -1px rgb(0 0 0 / 0.1), 0 2px 4px -2px rgb(0 0 0 / 0.1)|0 4px 6px -1px var(--tw-shadow-color), 0 2px 4px -2px var(--tw-shadow-color)" },
    { "lg", "0 10px 15px -3px rgb(0 0 0 / 0.1), 0 4px 6px -4px rgb(0 0 0 / 0.1)|0 10px 15px -3px var(--tw-shadow-color), 0 4px 6px -4px var(--tw-shadow-color)" },
    { "xl", "0 20px 25px -5px rgb(0 0 0 / 0.1), 0 8px 10px -6px rgb(0 0 0 / 0.1)|0 20px 25px -5px var(--tw-shadow-color), 0 8px 10px -6px var(--tw-shadow-color)" },
    { "2xl", "0 25px 50px -12px rgb(0 0 0 / 0.25)|0 25px 50px -12px var(--tw-shadow-color)" },
    { "inner", "inset 0 2px 4px 0 rgb(0 0 0 / 0.05)|inset 0 2px 4px 0 var(--tw-shadow-color)" },
    { "none", "0 0 #0000|0 0 #0000" },
};

static const long_pair_t g_drop_shadows[] = {
    { "sm", "drop-shadow(0 1px 1px rgb(0 0 0 / 0.05))" },
    { "", "drop-shadow(0 1px 2px rgb(0 0 0 / 0.1)) drop-shadow(0 1px 1px rgb(0 0 0 / 0.06))" },
    { "md", "drop-shadow(0 4px 3px rgb(0 0 0 / 0.07)) drop-shadow(0 2px 2px rgb(0 0 0 / 0.06))" },
    { "lg", "drop-shadow(0 10px 8px rgb(0 0 0 / 0.04)) drop-shadow(0 4px 3px rgb(0 0 0 / 0.1))" },
    { "xl", "drop-shadow(0 20px 13px rgb(0 0 0 / 0.03)) drop-shadow(0 8px 5px rgb(0 0 0 / 0.08))" },
    { "2xl", "drop-shadow(0 25px 25px rgb(0 0 0 / 0.15))" },
    { "none", "drop-shadow(0 0 #0000)" },
};

static const pair_t g_blurs[] = {
    { "none", "0" }, { "sm", "4px" }, { "", "8px" }, { "md", "12px" }, { "lg", "16px" }, { "xl", "24px" },
    { "2xl", "40px" }, { "3xl", "64px" },
};

static const pair_t g_max_widths[] = {
    { "none", "none" }, { "xs", "20rem" }, { "sm", "24rem" }, { "md", "28rem" }, { "lg", "32rem" }, { "xl", "36rem" },
    { "2xl", "42rem" }, { "3xl", "48rem" }, { "4xl", "56rem" }, { "5xl", "64rem" }, { "6xl", "72rem" }, { "7xl", "80rem" },
    { "full", "100%" }, { "min", "min-content" }, { "max", "max-content" }, { "fit", "fit-content" }, { "prose", "65ch" },
};

static const pair_t g_align[] = {
    { "start", "flex-start" }, { "end", "flex-end" }, { "center", "center" }, { "between", "space-between" },
    { "around", "space-around" }, { "evenly", "space-evenly" }, { "stretch", "stretch" }, { "baseline", "baseline" },
    { "normal", "normal" }, { "auto", "auto" },
};

static const char filter_value[] =
    "var(--tw-blur) var(--tw-brightness) var(--tw-contrast) var(--tw-grayscale) var(--tw-hue-rotate) var(--tw-invert) "
    "var(--tw-saturate) var(--tw-sepia) var(--tw-drop-shadow)";
static const char transform_value[] =
    "translate(var(--tw-translate-x), var(--tw-translate-y)) rotate(var(--tw-rotate)) skewX(var(--tw-skew-x)) "
    "skewY(var(--tw-skew-y)) scaleX(var(--tw-scale-x)) scaleY(var(--tw-scale-y))";

/* Tailwind's preflight (v3), with its defaults of the custom properties above */
const char tailwind_preflight[] =
    "*,::after,::before{box-sizing:border-box;border-width:0;border-style:solid;border-color:#e5e7eb;"
    "--tw-translate-x:0;--tw-translate-y:0;--tw-rotate:0;--tw-skew-x:0;--tw-skew-y:0;--tw-scale-x:1;--tw-scale-y:1;"
    "--tw-ring-offset-shadow:0 0 #0000;--tw-ring-shadow:0 0 #0000;--tw-shadow:0 0 #0000;--tw-shadow-colored:0 0 #0000;"
    "--tw-blur: ;--tw-brightness: ;--tw-contrast: ;--tw-grayscale: ;--tw-hue-rotate: ;--tw-invert: ;--tw-saturate: ;"
    "--tw-sepia: ;--tw-drop-shadow: }"
    "::after,::before{--tw-content:''}"
    "html{line-height:1.5;font-family:ui-sans-serif,system-ui,sans-serif}"
    "body{margin:0;line-height:inherit}"
    "hr{height:0;color:inherit;border-top-width:1px}"
    "h1,h2,h3,h4,h5,h6{font-size:inherit;font-weight:inherit}"
    "a{color:inherit;text-decoration:inherit}"
    "b,strong{font-weight:bolder}"
    "small{font-size:80%}"
    "table{text-indent:0;border-color:inherit;border-collapse:collapse}"
    "button,input,optgroup,select,textarea{font-family:inherit;font-size:100%;font-weight:inherit;line-height:inherit;"
    "letter-spacing:inherit;color:inherit;margin:0;padding:0}"
    "button,select{text-transform:none}"
    "button,input[type=button],input[type=reset],input[type=submit]{background-color:transparent;background-image:none}"
    "blockquote,dd,dl,figure,h1,h2,h3,h4,h5,h6,hr,p,pre{margin:0}"
    "fieldset{margin:0;padding:0}legend{padding:0}"
    "menu,ol,ul{list-style:none;margin:0;padding:0}"
    "dialog{padding:0}"
    "audio,canvas,embed,iframe,img,object,svg,video{display:block;vertical-align:middle}"
    "img,video{max-width:100%;height:auto}"
    "[hidden]{display:none}";

/* ---- Building declarations ---- */

/* The first n bytes of s into out */
static void cut(char* out, size_t size, const char* s, size_t n)
{
    if (n + 1U > size)
        n = size - 1U;
    memcpy(out, s, n);
    out[n] = '\0';
}

typedef struct
{
    conv_t*     c;
    decl_t*     decls;
    uint32_t    count;
    uint32_t    max;
    bool        important;
} out_t;

static void decl(out_t* o, const char* name, const char* value)
{
    if (o->count >= o->max)
        return;
    o->decls[o->count].name = arena_strndup(&o->c->arena, name, strlen(name));
    o->decls[o->count].value = arena_strndup(&o->c->arena, value, strlen(value));
    o->decls[o->count].important = o->important;
    if (o->decls[o->count].name != NULL && o->decls[o->count].value != NULL)
        o->count++;
}

static bool starts(const char* s, const char* prefix, const char** rest)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0)
        return false;
    *rest = s + n;
    return true;
}

/* A table of { char name[]; char value[]; } entries: the value of `name`, NULL when none */
static const char* lookup(const void* table, size_t count, size_t stride, size_t value_at, const char* name)
{
    const char* entry = table;
    for (size_t i = 0; i < count; i++, entry += stride)
    {
        if (strcmp(entry, name) == 0)
            return entry + value_at;
    }
    return NULL;
}

#define LOOKUP(table, name)     lookup(table, sizeof(table) / sizeof(table[0]), sizeof(table[0]), \
                                       offsetof(__typeof__(table[0]), value), name)

/* "[...]" -> its inside, '_' as spaces; NULL when not one */
static bool arbitrary(const char* v, char* out, size_t size)
{
    size_t n = strlen(v);
    if (n < 3U || v[0] != '[' || v[n - 1U] != ']' || n - 2U >= size)
        return false;
    for (size_t i = 0; i < n - 2U; i++)
        out[i] = (v[i + 1U] == '_') ? ' ' : v[i + 1U];
    out[n - 2U] = '\0';
    return true;
}

static bool is_number(const char* s)
{
    bool digits = false;
    for (; *s != '\0'; s++)
    {
        if (*s >= '0' && *s <= '9')
            digits = true;
        else if (*s != '.')
            return false;
    }
    return digits;
}

/* "2.5" -> 2500 (thousandths) */
static int32_t thousandths(const char* s)
{
    int32_t v = 0, scale = 1000;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
    v *= 1000;
    if (*s == '.')
    {
        for (s++; *s >= '0' && *s <= '9' && scale > 1; s++)
        {
            scale /= 10;
            v += (*s - '0') * scale;
        }
    }
    return v;
}

static void format_rem(char* out, size_t size, int32_t quarters_thousandths, bool negative)
{
    /* n * 0.25rem, n in thousandths */
    int32_t rem = quarters_thousandths / 4;        /* thousandths of a rem */
    Dmod_SnPrintf(out, size, "%s%d.%03drem", negative ? "-" : "", (int)(rem / 1000), (int)(rem % 1000));
}

/* The spacing scale, fractions, keywords; false when `v` is none of them */
static bool spacing(const char* v, bool negative, bool fractions, char* out, size_t size)
{
    char a[MAX_VALUE];
    if (arbitrary(v, a, sizeof(a)))
    {
        Dmod_SnPrintf(out, size, negative ? "calc(%s * -1)" : "%s", a);
        return true;
    }
    if (strcmp(v, "px") == 0)
    {
        Dmod_SnPrintf(out, size, negative ? "-1px" : "1px");
        return true;
    }
    if (strcmp(v, "0") == 0)
    {
        Dmod_SnPrintf(out, size, "0px");
        return true;
    }
    if (is_number(v))
    {
        format_rem(out, size, thousandths(v), negative);
        return true;
    }
    if (!fractions)
        return false;
    if (strcmp(v, "full") == 0)
    {
        Dmod_SnPrintf(out, size, negative ? "-100%%" : "100%%");
        return true;
    }
    const char* slash = strchr(v, '/');
    if (slash != NULL)
    {
        int32_t num = 0, den = 0;
        for (const char* s = v; s < slash; s++)
        {
            if (*s < '0' || *s > '9')
                return false;
            num = num * 10 + (*s - '0');
        }
        for (const char* s = slash + 1; *s != '\0'; s++)
        {
            if (*s < '0' || *s > '9')
                return false;
            den = den * 10 + (*s - '0');
        }
        if (den == 0)
            return false;
        int32_t pct = num * 100000 / den;           /* 1/1000 % */
        Dmod_SnPrintf(out, size, "%s%d.%03d%%", negative ? "-" : "", (int)(pct / 1000), (int)(pct % 1000));
        return true;
    }
    return false;
}

/* A color of the palette (with /opacity), black, white, ...; false when `v` is none */
static bool color(const char* v, char* out, size_t size)
{
    char name[64], a[MAX_VALUE];
    const char* slash = NULL;
    int depth = 0;
    for (const char* s = v; *s != '\0'; s++)
    {
        if (*s == '[')
            depth++;
        else if (*s == ']')
            depth--;
        else if (*s == '/' && depth == 0)
            slash = s;
    }
    size_t n = (slash != NULL) ? (size_t)(slash - v) : strlen(v);
    if (n >= sizeof(name))
        return false;
    memcpy(name, v, n);
    name[n] = '\0';

    /* The opacity: /10 -> 0.1, /[0.15] */
    char alpha[32] = "1";
    if (slash != NULL)
    {
        if (arbitrary(slash + 1, a, sizeof(a)))
            Dmod_SnPrintf(alpha, sizeof(alpha), "%s", a);
        else if (is_number(slash + 1))
        {
            int32_t f = thousandths(slash + 1);          /* "10" (%) -> 10000 */
            Dmod_SnPrintf(alpha, sizeof(alpha), "%d.%04d", (int)(f / 100000), (int)((f % 100000) / 10));
        }
        else
            return false;
    }

    uint32_t rgb = 0;
    bool found = false;
    if (strcmp(name, "black") == 0 || strcmp(name, "white") == 0)
    {
        rgb = (name[0] == 'b') ? 0x000000u : 0xFFFFFFu;
        found = true;
    }
    else if (strcmp(name, "transparent") == 0)
    {
        Dmod_SnPrintf(out, size, "transparent");
        return slash == NULL;
    }
    else if (strcmp(name, "current") == 0)
    {
        Dmod_SnPrintf(out, size, "currentColor");
        return slash == NULL;
    }
    else if (strcmp(name, "inherit") == 0)
    {
        Dmod_SnPrintf(out, size, "inherit");
        return slash == NULL;
    }
    else if (arbitrary(name, a, sizeof(a)))
    {
        if (a[0] != '#' && strncmp(a, "rgb", 3) != 0 && strncmp(a, "hsl", 3) != 0 && strncmp(a, "color:", 6) != 0)
            return false;
        if (strncmp(a, "color:", 6) == 0)
            memmove(a, a + 6, strlen(a + 6) + 1U);
        if (slash == NULL || a[0] != '#')
        {
            Dmod_SnPrintf(out, size, "%s", a);
            return true;
        }
        /* #rrggbb with an opacity */
        uint32_t h = 0;
        size_t len = strlen(a);
        for (size_t i = 1; i < len; i++)
        {
            char d = a[i];
            h = h * 16U + (uint32_t)((d >= '0' && d <= '9') ? d - '0' : (d >= 'a' && d <= 'f') ? d - 'a' + 10 : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : 0);
        }
        if (len == 4U)
            h = ((h & 0xF00u) << 12) | ((h & 0xF00u) << 8) | ((h & 0x0F0u) << 8) | ((h & 0x0F0u) << 4) | ((h & 0x00Fu) << 4) | (h & 0x00Fu);
        rgb = h & 0xFFFFFFu;
        found = true;
    }
    else
    {
        const char* dash = strrchr(name, '-');
        if (dash == NULL)
            return false;
        for (size_t i = 0; i < sizeof(g_palette) / sizeof(g_palette[0]) && !found; i++)
        {
            if (strlen(g_palette[i].name) != (size_t)(dash - name) || strncmp(g_palette[i].name, name, (size_t)(dash - name)) != 0)
                continue;
            for (size_t k = 0; k < 11U && !found; k++)
            {
                if (strcmp(dash + 1, g_shades[k]) == 0)
                {
                    rgb = g_palette[i].colors[k];
                    found = true;
                }
            }
        }
    }
    if (!found)
        return false;
    Dmod_SnPrintf(out, size, "rgb(%u %u %u / %s)", (unsigned)((rgb >> 16) & 0xFFu), (unsigned)((rgb >> 8) & 0xFFu),
                  (unsigned)(rgb & 0xFFu), alpha);
    return true;
}

/* The same color, transparent: "rgb(r g b / 0)" */
static void transparent_of(const char* color_value, char* out, size_t size)
{
    const char* slash = strrchr(color_value, '/');
    if (strncmp(color_value, "rgb(", 4) == 0 && slash != NULL)
    {
        cut(out, size, color_value, (size_t)(slash - color_value));
        strncat(out, "/ 0)", size - strlen(out) - 1U);
    }
    else
        Dmod_SnPrintf(out, size, "transparent");
}

/* ---- Utilities ---- */

static const char g_sides[4][2] = { "t", "r", "b", "l" };
static const char g_side_names[4][8] = { "top", "right", "bottom", "left" };

/* m-, mx-, mt-, p-, ...: `prop` is "margin" or "padding" */
static bool box_sides(out_t* o, const char* u, char letter, const char* prop, bool negative, uint32_t base_rank, uint32_t* rank)
{
    char value[MAX_VALUE], name[32];
    const char* rest;
    if (u[0] != letter)
        return false;
    if (u[1] == '-')
    {
        if (!(spacing(u + 2, negative, false, value, sizeof(value)) || (letter == 'm' && strcmp(u + 2, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
            return false;
        decl(o, prop, value);
        *rank = base_rank;
        return true;
    }
    if ((u[1] == 'x' || u[1] == 'y') && u[2] == '-')
    {
        rest = u + 3;
        if (!(spacing(rest, negative, false, value, sizeof(value)) || (letter == 'm' && strcmp(rest, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
            return false;
        for (uint32_t i = (u[1] == 'x') ? 1U : 0U; i < 4U; i += 2U)
        {
            Dmod_SnPrintf(name, sizeof(name), "%s-%s", prop, g_side_names[i]);
            decl(o, name, value);
        }
        *rank = base_rank + 1U;
        return true;
    }
    for (uint32_t i = 0; i < 4U; i++)
    {
        if (u[1] == g_sides[i][0] && u[2] == '-')
        {
            rest = u + 3;
            if (!(spacing(rest, negative, false, value, sizeof(value)) || (letter == 'm' && strcmp(rest, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
                return false;
            Dmod_SnPrintf(name, sizeof(name), "%s-%s", prop, g_side_names[i]);
            decl(o, name, value);
            *rank = base_rank + 2U;
            return true;
        }
    }
    return false;
}

static bool sizes(out_t* o, const char* u, bool negative, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    static const struct { char prefix[8]; char prop[12]; uint32_t rank; } sz[] = {
        { "w-", "width", R_WIDTH }, { "h-", "height", R_HEIGHT }, { "min-w-", "min-width", R_MIN_WIDTH },
        { "min-h-", "min-height", R_MIN_HEIGHT }, { "max-h-", "max-height", R_MAX_HEIGHT }, { "size-", "width", R_SIZE },
    };
    if (negative)
        return false;
    if (starts(u, "max-w-", &v))
    {
        const char* m = LOOKUP(g_max_widths, v);
        if (m == NULL && !arbitrary(v, value, sizeof(value)))
            return false;
        decl(o, "max-width", (m != NULL) ? m : value);
        *rank = R_MAX_WIDTH;
        return true;
    }
    for (size_t i = 0; i < sizeof(sz) / sizeof(sz[0]); i++)
    {
        if (!starts(u, sz[i].prefix, &v))
            continue;
        if (strcmp(v, "auto") == 0)
            Dmod_SnPrintf(value, sizeof(value), "auto");
        else if (strcmp(v, "screen") == 0)
            Dmod_SnPrintf(value, sizeof(value), (sz[i].prop[0] == 'w' || sz[i].prop[4] == 'w') ? "100vw" : "100vh");
        else if (strcmp(v, "min") == 0 || strcmp(v, "max") == 0 || strcmp(v, "fit") == 0)
            Dmod_SnPrintf(value, sizeof(value), "%s-content", v);
        else if (strcmp(v, "svh") == 0 || strcmp(v, "dvh") == 0 || strcmp(v, "lvh") == 0)
            Dmod_SnPrintf(value, sizeof(value), "100vh");
        else if (!spacing(v, false, true, value, sizeof(value)))
            return false;
        decl(o, sz[i].prop, value);
        if (sz[i].rank == R_SIZE)
            decl(o, "height", value);
        *rank = sz[i].rank;
        return true;
    }
    return false;
}

static bool inset(out_t* o, const char* u, bool negative, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    if (starts(u, "inset-", &v))
    {
        bool x = starts(v, "x-", &v), y = !x && starts(v, "y-", &v);
        if (!(spacing(v, negative, true, value, sizeof(value)) || (strcmp(v, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
            return false;
        for (uint32_t i = 0; i < 4U; i++)
        {
            if ((x && i % 2U == 0U) || (y && i % 2U == 1U))
                continue;
            decl(o, g_side_names[i], value);
        }
        *rank = (x || y) ? R_INSET_XY : R_INSET;
        return true;
    }
    for (uint32_t i = 0; i < 4U; i++)
    {
        char prefix[16];
        Dmod_SnPrintf(prefix, sizeof(prefix), "%s-", g_side_names[i]);
        if (!starts(u, prefix, &v))
            continue;
        if (!(spacing(v, negative, true, value, sizeof(value)) || (strcmp(v, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
            return false;
        decl(o, g_side_names[i], value);
        *rank = R_INSET_SIDE;
        return true;
    }
    return false;
}

static bool borders(out_t* o, const char* u, uint32_t* rank)
{
    char value[MAX_VALUE], name[48];
    const char* v;
    if (strcmp(u, "border") == 0 || strcmp(u, "border-0") == 0 || strcmp(u, "border-2") == 0 || strcmp(u, "border-4") == 0 ||
        strcmp(u, "border-8") == 0)
    {
        decl(o, "border-width", (u[6] == '\0') ? "1px" : (u[7] == '0') ? "0px" : (u[7] == '2') ? "2px" : (u[7] == '4') ? "4px" : "8px");
        *rank = R_BORDER_WIDTH;
        return true;
    }
    if (strcmp(u, "border-solid") == 0 || strcmp(u, "border-dashed") == 0 || strcmp(u, "border-dotted") == 0 ||
        strcmp(u, "border-double") == 0 || strcmp(u, "border-none") == 0 || strcmp(u, "border-hidden") == 0)
    {
        decl(o, "border-style", u + 7);
        *rank = R_BORDER_STYLE;
        return true;
    }
    if (!starts(u, "border-", &v))
        return false;
    if (arbitrary(v, value, sizeof(value)) && value[0] >= '0' && value[0] <= '9')
    {
        decl(o, "border-width", value);
        *rank = R_BORDER_WIDTH;
        return true;
    }
    if (color(v, value, sizeof(value)))
    {
        decl(o, "border-color", value);
        *rank = R_BORDER_COLOR;
        return true;
    }
    /* border-x, border-t-2, border-b-white/10 */
    uint32_t first = 0, step = 1, n_sides = 0;
    uint32_t r_width = R_BORDER_WIDTH_SIDE, r_color = R_BORDER_COLOR_SIDE;
    if (v[0] == 'x' || v[0] == 'y')
    {
        first = (v[0] == 'x') ? 1U : 0U;
        step = 2;
        n_sides = 2;
        r_width = R_BORDER_WIDTH_XY;
        r_color = R_BORDER_COLOR_XY;
    }
    else
    {
        for (uint32_t i = 0; i < 4U; i++)
        {
            if (v[0] == g_sides[i][0])
            {
                first = i;
                n_sides = 1;
            }
        }
    }
    if (n_sides == 0 || (v[1] != '\0' && v[1] != '-'))
        return false;
    const char* rest = (v[1] == '-') ? v + 2 : "";
    const char* width = NULL;
    if (rest[0] == '\0')
        width = "1px";
    else if (strcmp(rest, "0") == 0)
        width = "0px";
    else if (strcmp(rest, "2") == 0)
        width = "2px";
    else if (strcmp(rest, "4") == 0)
        width = "4px";
    else if (strcmp(rest, "8") == 0)
        width = "8px";
    else if (arbitrary(rest, value, sizeof(value)) && value[0] >= '0' && value[0] <= '9')
        width = value;
    for (uint32_t k = 0, i = first; k < n_sides; k++, i += step)
    {
        if (width != NULL)
            Dmod_SnPrintf(name, sizeof(name), "border-%s-width", g_side_names[i]);
        else
            Dmod_SnPrintf(name, sizeof(name), "border-%s-color", g_side_names[i]);
        if (width != NULL)
            decl(o, name, width);
        else
        {
            char col[MAX_VALUE];
            if (!color(rest, col, sizeof(col)))
                return false;
            decl(o, name, col);
        }
    }
    *rank = (width != NULL) ? r_width : r_color;
    return true;
}

static bool rounded(out_t* o, const char* u, uint32_t* rank)
{
    static const struct { char side[3]; char corners[2][14]; uint32_t rank; } sides[] = {
        { "t", { "top-left", "top-right" }, R_ROUNDED_SIDE }, { "r", { "top-right", "bottom-right" }, R_ROUNDED_SIDE },
        { "b", { "bottom-right", "bottom-left" }, R_ROUNDED_SIDE }, { "l", { "top-left", "bottom-left" }, R_ROUNDED_SIDE },
        { "tl", { "top-left", "" }, R_ROUNDED_CORNER }, { "tr", { "top-right", "" }, R_ROUNDED_CORNER },
        { "br", { "bottom-right", "" }, R_ROUNDED_CORNER }, { "bl", { "bottom-left", "" }, R_ROUNDED_CORNER },
    };
    char value[MAX_VALUE], name[48];
    const char* v;
    if (!starts(u, "rounded", &v))
        return false;
    if (v[0] == '\0' || v[0] == '-')
    {
        const char* size = (v[0] == '\0') ? "" : v + 1;
        const char* r = LOOKUP(g_radius, size);
        if (r != NULL || arbitrary(size, value, sizeof(value)))
        {
            decl(o, "border-radius", (r != NULL) ? r : value);
            *rank = R_ROUNDED;
            return true;
        }
        for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++)
        {
            size_t n = strlen(sides[i].side);
            if (strncmp(size, sides[i].side, n) != 0 || (size[n] != '\0' && size[n] != '-'))
                continue;
            const char* s = (size[n] == '-') ? size + n + 1 : "";
            r = LOOKUP(g_radius, s);
            if (r == NULL && !arbitrary(s, value, sizeof(value)))
                return false;
            for (uint32_t k = 0; k < 2U && sides[i].corners[k][0] != '\0'; k++)
            {
                Dmod_SnPrintf(name, sizeof(name), "border-%s-radius", sides[i].corners[k]);
                decl(o, name, (r != NULL) ? r : value);
            }
            *rank = sides[i].rank;
            return true;
        }
    }
    return false;
}

static bool texts(out_t* o, const char* u, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    if (!starts(u, "text-", &v))
        return false;
    static const pair_t aligns[] = { { "left", "left" }, { "center", "center" }, { "right", "right" },
                                     { "justify", "justify" }, { "start", "left" }, { "end", "right" } };
    const char* a = LOOKUP(aligns, v);
    if (a != NULL)
    {
        decl(o, "text-align", a);
        *rank = R_TEXT_ALIGN;
        return true;
    }
    for (size_t i = 0; i < sizeof(g_font_sizes) / sizeof(g_font_sizes[0]); i++)
    {
        size_t n = strlen(g_font_sizes[i].name);
        if (strncmp(v, g_font_sizes[i].name, n) == 0 && (v[n] == '\0' || v[n] == '/'))
        {
            decl(o, "font-size", g_font_sizes[i].size);
            if (v[n] == '/')
            {
                const char* lh = LOOKUP(g_leading, v + n + 1);
                decl(o, "line-height", (lh != NULL) ? lh : g_font_sizes[i].line_height);
            }
            else
                decl(o, "line-height", g_font_sizes[i].line_height);
            *rank = R_FONT_SIZE;
            return true;
        }
    }
    if (arbitrary(v, value, sizeof(value)) && ((value[0] >= '0' && value[0] <= '9') || value[0] == '.'))
    {
        decl(o, "font-size", value);
        *rank = R_FONT_SIZE;
        return true;
    }
    if (color(v, value, sizeof(value)))
    {
        decl(o, "color", value);
        *rank = R_TEXT_COLOR;
        return true;
    }
    return false;
}

static bool flexbox(out_t* o, const char* u, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    static const struct { char name[18]; char value[32]; } simple[] = {
        { "flex-row", "flex-direction:row" }, { "flex-row-reverse", "flex-direction:row-reverse" },
        { "flex-col", "flex-direction:column" }, { "flex-col-reverse", "flex-direction:column-reverse" },
        { "flex-wrap", "flex-wrap:wrap" }, { "flex-wrap-reverse", "flex-wrap:wrap-reverse" }, { "flex-nowrap", "flex-wrap:nowrap" },
        { "flex-1", "flex:1 1 0%" }, { "flex-auto", "flex:1 1 auto" }, { "flex-initial", "flex:0 1 auto" }, { "flex-none", "flex:none" },
        { "shrink", "flex-shrink:1" }, { "shrink-0", "flex-shrink:0" }, { "flex-shrink", "flex-shrink:1" }, { "flex-shrink-0", "flex-shrink:0" },
        { "grow", "flex-grow:1" }, { "grow-0", "flex-grow:0" }, { "flex-grow", "flex-grow:1" }, { "flex-grow-0", "flex-grow:0" },
    };
    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); i++)
    {
        if (strcmp(u, simple[i].name) != 0)
            continue;
        const char* colon = strchr(simple[i].value, ':');
        char name[32];
        cut(name, sizeof(name), simple[i].value, (size_t)(colon - simple[i].value));
        decl(o, name, colon + 1);
        *rank = (name[5] == 'd') ? R_FLEX_DIRECTION : (name[5] == 'w') ? R_FLEX_WRAP : (strcmp(name, "flex") == 0) ? R_FLEX
              : (name[5] == 's') ? R_SHRINK : R_GROW;
        return true;
    }
    if (starts(u, "basis-", &v) && (spacing(v, false, true, value, sizeof(value)) || (strcmp(v, "auto") == 0 && Dmod_SnPrintf(value, sizeof(value), "auto") > 0)))
    {
        decl(o, "flex-basis", value);
        *rank = R_BASIS;
        return true;
    }
    static const struct { char prefix[16]; char prop[16]; uint32_t rank; } aligns[] = {
        { "justify-items-", "justify-items", R_JUSTIFY_ITEMS }, { "justify-self-", "justify-self", R_JUSTIFY_SELF },
        { "justify-", "justify-content", R_JUSTIFY_CONTENT }, { "items-", "align-items", R_ALIGN_ITEMS },
        { "content-", "align-content", R_ALIGN_CONTENT }, { "self-", "align-self", R_ALIGN_SELF },
        { "place-content-", "place-content", R_PLACE_CONTENT }, { "place-items-", "place-items", R_PLACE_ITEMS },
        { "place-self-", "place-self", R_PLACE_SELF },
    };
    for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++)
    {
        if (!starts(u, aligns[i].prefix, &v))
            continue;
        const char* a = LOOKUP(g_align, v);
        if (a == NULL)
            return false;
        decl(o, aligns[i].prop, a);
        *rank = aligns[i].rank;
        return true;
    }
    if (starts(u, "gap-", &v))
    {
        bool x = starts(v, "x-", &v), y = !x && starts(v, "y-", &v);
        if (!spacing(v, false, false, value, sizeof(value)))
            return false;
        if (!y)
            decl(o, "column-gap", value);
        if (!x)
            decl(o, "row-gap", value);
        *rank = (x || y) ? R_GAP_XY : R_GAP;
        return true;
    }
    if (starts(u, "order-", &v))
    {
        if (strcmp(v, "first") == 0)
            decl(o, "order", "-9999");
        else if (strcmp(v, "last") == 0)
            decl(o, "order", "9999");
        else if (strcmp(v, "none") == 0)
            decl(o, "order", "0");
        else if (is_number(v))
            decl(o, "order", v);
        else
            return false;
        *rank = R_ORDER;
        return true;
    }
    return false;
}

static bool grids(out_t* o, const char* u, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    if (starts(u, "grid-cols-", &v) || starts(u, "grid-rows-", &v))
    {
        bool cols = u[5] == 'c';
        if (strcmp(v, "none") == 0)
            Dmod_SnPrintf(value, sizeof(value), "none");
        else if (is_number(v))
            Dmod_SnPrintf(value, sizeof(value), "repeat(%s, minmax(0, 1fr))", v);
        else if (!arbitrary(v, value, sizeof(value)))
            return false;
        decl(o, cols ? "grid-template-columns" : "grid-template-rows", value);
        *rank = cols ? R_GRID_COLS : R_GRID_ROWS;
        return true;
    }
    if (starts(u, "col-span-", &v) || starts(u, "row-span-", &v))
    {
        bool cols = u[0] == 'c';
        if (strcmp(v, "full") == 0)
            Dmod_SnPrintf(value, sizeof(value), "1 / -1");
        else if (is_number(v))
            Dmod_SnPrintf(value, sizeof(value), "span %s / span %s", v, v);
        else
            return false;
        decl(o, cols ? "grid-column" : "grid-row", value);
        *rank = cols ? R_GRID_COLUMN : R_GRID_ROW;
        return true;
    }
    return false;
}

static bool effects(out_t* o, const char* u, bool negative, uint32_t* rank)
{
    char value[MAX_VALUE], a[MAX_VALUE];
    const char* v;
    if (strcmp(u, "shadow") == 0 || starts(u, "shadow-", &v))
    {
        const char* size = (u[6] == '\0') ? "" : u + 7;
        const char* s = LOOKUP(g_shadows, size);
        if (s != NULL)
        {
            const char* bar = strchr(s, '|');
            cut(value, sizeof(value), s, (size_t)(bar - s));
            decl(o, "--tw-shadow", value);
            decl(o, "--tw-shadow-colored", bar + 1);
            decl(o, "box-shadow", "var(--tw-ring-offset-shadow, 0 0 #0000), var(--tw-ring-shadow, 0 0 #0000), var(--tw-shadow)");
            *rank = R_SHADOW;
            return true;
        }
        if (color(size, value, sizeof(value)))
        {
            decl(o, "--tw-shadow-color", value);
            decl(o, "--tw-shadow", "var(--tw-shadow-colored)");
            *rank = R_SHADOW_COLOR;
            return true;
        }
        return false;
    }
    if (strcmp(u, "blur") == 0 || starts(u, "blur-", &v))
    {
        const char* b = LOOKUP(g_blurs, (u[4] == '\0') ? "" : u + 5);
        if (b == NULL && !arbitrary(u + 5, a, sizeof(a)))
            return false;
        Dmod_SnPrintf(value, sizeof(value), "blur(%s)", (b != NULL) ? b : a);
        decl(o, "--tw-blur", value);
        decl(o, "filter", filter_value);
        *rank = R_BLUR;
        return true;
    }
    if (strcmp(u, "drop-shadow") == 0 || starts(u, "drop-shadow-", &v))
    {
        const char* d = LOOKUP(g_drop_shadows, (u[11] == '\0') ? "" : u + 12);
        if (d == NULL)
            return false;
        decl(o, "--tw-drop-shadow", d);
        decl(o, "filter", filter_value);
        *rank = R_DROP_SHADOW;
        return true;
    }
    if (starts(u, "opacity-", &v))
    {
        if (is_number(v))
        {
            int32_t t = thousandths(v);         /* "80" -> 80000 */
            Dmod_SnPrintf(value, sizeof(value), "%d.%03d", (int)(t / 100000), (int)((t % 100000) / 100));
        }
        else if (!arbitrary(v, value, sizeof(value)))
            return false;
        decl(o, "opacity", value);
        *rank = R_OPACITY;
        return true;
    }
    if (starts(u, "translate-x-", &v) || starts(u, "translate-y-", &v))
    {
        bool x = u[10] == 'x';
        if (!spacing(v, negative, true, value, sizeof(value)))
            return false;
        decl(o, x ? "--tw-translate-x" : "--tw-translate-y", value);
        decl(o, "transform", transform_value);
        *rank = R_TRANSLATE;
        return true;
    }
    if (strcmp(u, "transform") == 0 || strcmp(u, "transform-gpu") == 0 || strcmp(u, "transform-cpu") == 0)
    {
        decl(o, "transform", transform_value);
        *rank = R_TRANSFORM;
        return true;
    }
    if (strcmp(u, "transform-none") == 0)
    {
        decl(o, "transform", "none");
        *rank = R_TRANSFORM;
        return true;
    }
    if (starts(u, "scale-", &v) || starts(u, "rotate-", &v))
    {
        bool scale = u[1] == 'c';
        bool xy = scale && (starts(v, "x-", &v) || starts(v, "y-", &v));
        if (scale && is_number(v))
        {
            int32_t t = thousandths(v);
            Dmod_SnPrintf(value, sizeof(value), "%d.%03d", (int)(t / 100000), (int)((t % 100000) / 100));
        }
        else if (!scale && is_number(v))
            Dmod_SnPrintf(value, sizeof(value), "%s%sdeg", negative ? "-" : "", v);
        else
            return false;
        if (scale)
        {
            if (!xy || u[6] == 'x')
                decl(o, "--tw-scale-x", value);
            if (!xy || u[6] == 'y')
                decl(o, "--tw-scale-y", value);
        }
        else
            decl(o, "--tw-rotate", value);
        decl(o, "transform", transform_value);
        *rank = scale ? R_SCALE : R_ROTATE;
        return true;
    }
    return false;
}

static bool backgrounds(out_t* o, const char* u, uint32_t* rank)
{
    char value[MAX_VALUE], t[MAX_VALUE];
    const char* v;
    static const pair_t directions[] = {
        { "t", "to top" }, { "tr", "to top right" }, { "r", "to right" }, { "br", "to bottom right" },
        { "b", "to bottom" }, { "bl", "to bottom left" }, { "l", "to left" }, { "tl", "to top left" },
    };
    if (starts(u, "bg-gradient-to-", &v))
    {
        const char* d = LOOKUP(directions, v);
        if (d == NULL)
            return false;
        Dmod_SnPrintf(value, sizeof(value), "linear-gradient(%s, var(--tw-gradient-stops))", d);
        decl(o, "background-image", value);
        *rank = R_BG_IMAGE;
        return true;
    }
    if (strcmp(u, "bg-none") == 0)
    {
        decl(o, "background-image", "none");
        *rank = R_BG_IMAGE;
        return true;
    }
    if (starts(u, "bg-", &v) && color(v, value, sizeof(value)))
    {
        decl(o, "background-color", value);
        *rank = R_BG_COLOR;
        return true;
    }
    if (starts(u, "from-", &v) && color(v, value, sizeof(value)))
    {
        transparent_of(value, t, sizeof(t));
        decl(o, "--tw-gradient-from", value);
        decl(o, "--tw-gradient-to", t);
        decl(o, "--tw-gradient-stops", "var(--tw-gradient-from), var(--tw-gradient-to)");
        *rank = R_GRADIENT_FROM;
        return true;
    }
    if (starts(u, "via-", &v) && color(v, value, sizeof(value)))
    {
        char stops[MAX_VALUE];
        transparent_of(value, t, sizeof(t));
        decl(o, "--tw-gradient-to", t);
        Dmod_SnPrintf(stops, sizeof(stops), "var(--tw-gradient-from), %s, var(--tw-gradient-to)", value);
        decl(o, "--tw-gradient-stops", stops);
        *rank = R_GRADIENT_VIA;
        return true;
    }
    if (starts(u, "to-", &v) && color(v, value, sizeof(value)))
    {
        decl(o, "--tw-gradient-to", value);
        *rank = R_GRADIENT_TO;
        return true;
    }
    return false;
}

/* ---- The page's tailwind.config ---- */

static const char* skip_js(const char* s, const char* end)
{
    while (s < end)
    {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == ',')
            s++;
        else if (s + 1 < end && s[0] == '/' && s[1] == '/')
            while (s < end && *s != '\n')
                s++;
        else
            break;
    }
    return s;
}

/* A key ("sans", 'sans' or sans) or a string: its text at *text, its length returned, 0 if none */
static size_t js_word(const char** s, const char* end, const char** text)
{
    const char* p = skip_js(*s, end);
    size_t n = 0;
    if (p < end && (*p == '\'' || *p == '"' || *p == '`'))
    {
        char q = *p++;
        *text = p;
        while (p + n < end && p[n] != q)
            n++;
        *s = (p + n < end) ? p + n + 1 : end;
        return n;
    }
    *text = p;
    while (p + n < end && ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_' || p[n] == '-' || p[n] == '$'))
        n++;
    *s = p + n;
    return n;
}

/* fontFamily: { sans: ['Inter', 'sans-serif'], ... } of the theme (or its extend): what
 * font-sans and the others are on the page, as "|sans=Inter, sans-serif|..." */
void tailwind_config(conv_t* c, const char* text, size_t length)
{
    const char* end = text + length;
    const char* s = text;
    while (s + 15 <= end && strncmp(s, "tailwind.config", 15) != 0)
        s++;
    while (s + 10 <= end && strncmp(s, "fontFamily", 10) != 0)
        s++;
    if (s + 10 > end)
        return;
    s = skip_js(s + 10, end);
    if (s >= end || *s != ':')
        return;
    s = skip_js(s + 1, end);
    if (s >= end || *s != '{')
        return;
    s++;
    char all[512];
    size_t used = 0;
    const char* old = (c->tw_fonts != NULL) ? c->tw_fonts : "|";
    Dmod_SnPrintf(all, sizeof(all), "%s", old);
    used = strlen(all);
    for (;;)
    {
        const char* key;
        size_t kn = js_word(&s, end, &key);
        s = skip_js(s, end);
        if (kn == 0 || s >= end || *s != ':')
            break;
        s = skip_js(s + 1, end);
        char value[256];
        size_t vn = 0;
        value[0] = '\0';
        bool list = (s < end && *s == '[');
        if (list)
            s++;
        for (;;)
        {
            const char* f;
            const char* before = skip_js(s, end);
            if (before >= end || *before == ']' || *before == '}')
                break;
            size_t fn = js_word(&s, end, &f);
            if (fn == 0)
                break;
            bool quote = false;
            for (size_t i = 0; i < fn; i++)
                quote = quote || f[i] == ' ';
            if (vn + fn + 5U < sizeof(value))
                vn += (size_t)Dmod_SnPrintf(value + vn, sizeof(value) - vn, "%s%s", (vn > 0) ? ", " : "", quote ? "'" : "");
            if (vn + fn + 3U < sizeof(value))
            {
                memcpy(value + vn, f, fn);
                vn += fn;
                value[vn] = '\0';
                if (quote)
                    value[vn++] = '\'', value[vn] = '\0';
            }
            if (!list)
                break;
        }
        s = skip_js(s, end);
        if (list && s < end && *s == ']')
            s++;
        if (vn > 0 && used + kn + vn + 3U < sizeof(all))
        {
            memcpy(all + used, key, kn);
            used += kn;
            all[used++] = '=';
            memcpy(all + used, value, vn);
            used += vn;
            all[used++] = '|';
            all[used] = '\0';
        }
    }
    c->tw_fonts = arena_strndup(&c->arena, all, used);
}

/* font-<name> of the page's tailwind.config */
static bool configured_font(out_t* o, const char* u, uint32_t* rank)
{
    const char* name;
    if (o->c->tw_fonts == NULL || !starts(u, "font-", &name))
        return false;
    size_t n = strlen(name);
    for (const char* s = o->c->tw_fonts; *s != '\0'; s++)
    {
        if (s[0] == '|' && strncmp(s + 1, name, n) == 0 && s[1 + n] == '=')
        {
            const char* v = s + 2 + n;
            const char* e = strchr(v, '|');
            char value[256];
            cut(value, sizeof(value), v, (e != NULL) ? (size_t)(e - v) : strlen(v));
            decl(o, "font-family", value);
            *rank = R_FONT_FAMILY;
            return true;
        }
    }
    return false;
}

static bool keywords(out_t* o, const char* u, uint32_t* rank)
{
    static const struct { char name[20]; char decls[56]; uint32_t rank; } table[] = {
        { "block", "display:block", R_DISPLAY }, { "inline-block", "display:inline-block", R_DISPLAY },
        { "inline", "display:inline", R_DISPLAY }, { "flex", "display:flex", R_DISPLAY },
        { "inline-flex", "display:inline-flex", R_DISPLAY }, { "grid", "display:grid", R_DISPLAY },
        { "inline-grid", "display:inline-grid", R_DISPLAY }, { "contents", "display:contents", R_DISPLAY },
        { "hidden", "display:none", R_DISPLAY }, { "flow-root", "display:block", R_DISPLAY },
        { "table", "display:block", R_DISPLAY }, { "list-item", "display:block", R_DISPLAY },
        { "static", "position:static", R_POSITION }, { "fixed", "position:fixed", R_POSITION },
        { "absolute", "position:absolute", R_POSITION }, { "relative", "position:relative", R_POSITION },
        { "sticky", "position:relative", R_POSITION },
        { "visible", "visibility:visible", R_VISIBILITY }, { "invisible", "visibility:hidden", R_VISIBILITY },
        { "collapse", "visibility:hidden", R_VISIBILITY }, { "sr-only", "display:none", R_SR_ONLY },
        { "box-border", "box-sizing:border-box", R_BOX_SIZING }, { "box-content", "box-sizing:content-box", R_BOX_SIZING },
        { "uppercase", "text-transform:uppercase", R_TEXT_TRANSFORM }, { "lowercase", "text-transform:lowercase", R_TEXT_TRANSFORM },
        { "capitalize", "text-transform:capitalize", R_TEXT_TRANSFORM }, { "normal-case", "text-transform:none", R_TEXT_TRANSFORM },
        { "italic", "font-style:italic", R_FONT_STYLE }, { "not-italic", "font-style:normal", R_FONT_STYLE },
        { "truncate", "overflow:hidden;white-space:nowrap", R_TEXT_OVERFLOW },
        { "whitespace-normal", "white-space:normal", R_WHITESPACE }, { "whitespace-nowrap", "white-space:nowrap", R_WHITESPACE },
        { "whitespace-pre", "white-space:pre", R_WHITESPACE }, { "whitespace-pre-line", "white-space:pre-line", R_WHITESPACE },
        { "whitespace-pre-wrap", "white-space:pre-wrap", R_WHITESPACE },
        { "font-sans", "font-family:ui-sans-serif, system-ui, sans-serif", R_FONT_FAMILY },
        { "font-serif", "font-family:ui-serif, Georgia, serif", R_FONT_FAMILY },
        { "font-mono", "font-family:ui-monospace, monospace", R_FONT_FAMILY },
        { "align-baseline", "vertical-align:baseline", R_VERTICAL_ALIGN }, { "align-top", "vertical-align:top", R_VERTICAL_ALIGN },
        { "align-middle", "vertical-align:middle", R_VERTICAL_ALIGN }, { "align-bottom", "vertical-align:bottom", R_VERTICAL_ALIGN },
        { "align-text-top", "vertical-align:text-top", R_VERTICAL_ALIGN },
        { "align-text-bottom", "vertical-align:text-bottom", R_VERTICAL_ALIGN },
        { "z-auto", "z-index:auto", R_Z },
        { "bg-cover", "background-size:cover", R_BG_SIZE }, { "bg-contain", "background-size:contain", R_BG_SIZE },
        { "bg-auto", "background-size:auto", R_BG_SIZE },
        { "bg-center", "background-position:center", R_BG_POSITION }, { "bg-top", "background-position:top", R_BG_POSITION },
        { "bg-bottom", "background-position:bottom", R_BG_POSITION }, { "bg-left", "background-position:left", R_BG_POSITION },
        { "bg-right", "background-position:right", R_BG_POSITION },
        { "object-cover", "object-fit:cover", R_OBJECT_FIT }, { "object-contain", "object-fit:contain", R_OBJECT_FIT },
        { "object-fill", "object-fit:fill", R_OBJECT_FIT }, { "object-none", "object-fit:none", R_OBJECT_FIT },
        { "object-scale-down", "object-fit:scale-down", R_OBJECT_FIT },
        { "object-center", "object-position:center", R_OBJECT_POSITION }, { "object-top", "object-position:top", R_OBJECT_POSITION },
        { "object-bottom", "object-position:bottom", R_OBJECT_POSITION }, { "object-left", "object-position:left", R_OBJECT_POSITION },
        { "object-right", "object-position:right", R_OBJECT_POSITION },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        if (strcmp(table[i].name, u) != 0)
            continue;
        const char* d = table[i].decls;
        while (*d != '\0')
        {
            const char* colon = strchr(d, ':');
            const char* semi = strchr(colon, ';');
            size_t vl = (semi != NULL) ? (size_t)(semi - colon - 1) : strlen(colon + 1);
            char name[32], value[64];
            cut(name, sizeof(name), d, (size_t)(colon - d));
            cut(value, sizeof(value), colon + 1, vl);
            decl(o, name, value);
            d = (semi != NULL) ? semi + 1 : colon + 1 + vl;
        }
        *rank = table[i].rank;
        return true;
    }
    return false;
}

/* transition, transition-<what>, duration-<ms>, ease-<curve> */
static bool transitions(out_t* o, const char* u, uint32_t* rank)
{
    static const struct { char name[12]; char props[160]; } kinds[] = {
        { "", "color, background-color, border-color, text-decoration-color, fill, stroke, opacity, box-shadow, transform, filter, backdrop-filter" },
        { "all", "all" }, { "colors", "color, background-color, border-color, text-decoration-color, fill, stroke" },
        { "opacity", "opacity" }, { "shadow", "box-shadow" }, { "transform", "transform" }, { "none", "none" },
    };
    static const struct { char name[8]; char value[32]; } eases[] = {
        { "linear", "linear" }, { "in", "cubic-bezier(0.4, 0, 1, 1)" }, { "out", "cubic-bezier(0, 0, 0.2, 1)" },
        { "in-out", "cubic-bezier(0.4, 0, 0.2, 1)" },
    };
    const char* v;
    char value[MAX_VALUE];
    if (strcmp(u, "transition") == 0 || starts(u, "transition-", &v))
    {
        const char* kind = (u[10] == '\0') ? "" : u + 11;
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        {
            if (strcmp(kinds[i].name, kind) != 0)
                continue;
            decl(o, "transition-property", kinds[i].props);
            if (strcmp(kind, "none") != 0)
            {
                decl(o, "transition-timing-function", "cubic-bezier(0.4, 0, 0.2, 1)");
                decl(o, "transition-duration", "150ms");
            }
            *rank = R_TRANSITION;
            return true;
        }
        return false;
    }
    if (starts(u, "duration-", &v) && is_number(v))
    {
        Dmod_SnPrintf(value, sizeof(value), "%sms", v);
        decl(o, "transition-duration", value);
        *rank = R_DURATION;
        return true;
    }
    if (starts(u, "ease-", &v))
    {
        const char* e = LOOKUP(eases, v);
        if (e == NULL)
            return false;
        decl(o, "transition-timing-function", e);
        *rank = R_EASE;
        return true;
    }
    if (starts(u, "delay-", &v))
    {
        *rank = R_EASE;
        return true;                    /* Known: nothing to animate later */
    }
    return false;
}

static bool others(out_t* o, const char* u, bool negative, uint32_t* rank)
{
    char value[MAX_VALUE];
    const char* v;
    if (starts(u, "overflow-", &v))
    {
        bool x = starts(v, "x-", &v), y = !x && starts(v, "y-", &v);
        if (strcmp(v, "auto") != 0 && strcmp(v, "hidden") != 0 && strcmp(v, "clip") != 0 && strcmp(v, "visible") != 0 &&
            strcmp(v, "scroll") != 0)
            return false;
        decl(o, x ? "overflow-x" : y ? "overflow-y" : "overflow", v);
        *rank = (x || y) ? R_OVERFLOW_XY : R_OVERFLOW;
        return true;
    }
    if (starts(u, "z-", &v))
    {
        if (is_number(v))
            Dmod_SnPrintf(value, sizeof(value), "%s%s", negative ? "-" : "", v);
        else if (!arbitrary(v, value, sizeof(value)))
            return false;
        decl(o, "z-index", value);
        *rank = R_Z;
        return true;
    }
    if (starts(u, "font-", &v))
    {
        const char* w = LOOKUP(g_weights, v);
        if (w == NULL && !(arbitrary(v, value, sizeof(value)) && value[0] >= '0' && value[0] <= '9'))
            return false;
        decl(o, "font-weight", (w != NULL) ? w : value);
        *rank = R_FONT_WEIGHT;
        return true;
    }
    if (starts(u, "leading-", &v))
    {
        const char* l = LOOKUP(g_leading, v);
        if (l == NULL && !arbitrary(v, value, sizeof(value)))
            return false;
        decl(o, "line-height", (l != NULL) ? l : value);
        *rank = R_LEADING;
        return true;
    }
    if (starts(u, "tracking-", &v))
    {
        const char* t = LOOKUP(g_tracking, v);
        if (t == NULL && !arbitrary(v, value, sizeof(value)))
            return false;
        if (t != NULL && negative)
        {
            Dmod_SnPrintf(value, sizeof(value), "calc(%s * -1)", t);
            t = NULL;
        }
        decl(o, "letter-spacing", (t != NULL) ? t : value);
        *rank = R_TRACKING;
        return true;
    }
    if (arbitrary(u, value, sizeof(value)))
    {
        /* [property:value] */
        char* colon = strchr(value, ':');
        if (colon == NULL || colon == value)
            return false;
        *colon = '\0';
        decl(o, value, colon + 1);
        *rank = R_ARBITRARY;
        return true;
    }
    return false;
}

/* ---- Classes ---- */

/* Breakpoints of the responsive variants: rank above every utility, as Tailwind orders them */
static const struct
{
    char        name[4];
    int32_t     min_width;
} g_breakpoints[] = { { "sm", 640 }, { "md", 768 }, { "lg", 1024 }, { "xl", 1280 }, { "2xl", 1536 } };

uint32_t tailwind_class(conv_t* c, const char* name, decl_t* decls, uint32_t max, uint32_t* rank, bool* active)
{
    out_t o = { c, decls, 0, max, false };
    const char* u = name;
    uint32_t variant = 0;
    *active = false;

    /* Variants: "md:hover:bg-x" - every one must hold */
    for (;;)
    {
        const char* colon = NULL;
        int depth = 0;
        for (const char* s = u; *s != '\0' && colon == NULL; s++)
        {
            if (*s == '[')
                depth++;
            else if (*s == ']')
                depth--;
            else if (*s == ':' && depth == 0)
                colon = s;
        }
        if (colon == NULL)
            break;
        size_t n = (size_t)(colon - u);
        bool known = false;
        for (size_t i = 0; i < sizeof(g_breakpoints) / sizeof(g_breakpoints[0]); i++)
        {
            if (strlen(g_breakpoints[i].name) == n && strncmp(u, g_breakpoints[i].name, n) == 0)
            {
                if (c->vw < g_breakpoints[i].min_width * U)
                    return 0;
                variant = (uint32_t)i + 1U;
                known = true;
            }
        }
        if (n == 6 && strncmp(u, "active", 6) == 0)
        {
            *active = true;             /* Only while it is pressed */
            c->has_active = true;
            known = true;
        }
        if (!known && !(n == 11 && strncmp(u, "motion-safe", 11) == 0))
            return 0;                   /* hover:, focus:, dark:, group-hover:, ... */
        u = colon + 1;
    }
    if (*u == '!')
    {
        o.important = true;
        u++;
    }
    bool negative = false;
    if (*u == '-')
    {
        negative = true;
        u++;
    }

    uint32_t r = 0;
    bool ok = configured_font(&o, u, &r) || keywords(&o, u, &r) || box_sides(&o, u, 'm', "margin", negative, R_MARGIN, &r) ||
              box_sides(&o, u, 'p', "padding", false, R_PADDING, &r) || sizes(&o, u, negative, &r) ||
              inset(&o, u, negative, &r) || texts(&o, u, &r) || backgrounds(&o, u, &r) || borders(&o, u, &r) ||
              rounded(&o, u, &r) || flexbox(&o, u, &r) || grids(&o, u, &r) || effects(&o, u, negative, &r) ||
              transitions(&o, u, &r) || others(&o, u, negative, &r);
    if (!ok)
        return 0;
    *rank = variant * 1000U + r;
    return o.count;
}
