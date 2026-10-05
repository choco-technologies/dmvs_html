#include "private.h"
#include <string.h>

/*
 * Inline <svg>: a replaced element, drawn as an image. Its subtree is
 * written into an SVG file of its own - sized as its box, the viewBox kept -
 * which the view shows as an image (dmod's dmimg_svg rasterizes it when the
 * view's images are converted). The file is in $TMPDIR (/tmp), named after
 * the page and the element: <page>-svg<index>.svg.
 *
 * HTML's parser lowercases names; SVG's camelCase ones are restored. A
 * value of currentColor becomes the <svg>'s color.
 */

#define DEFAULT_W       300                     /* An <svg> of no size: CSS's default replaced size */
#define DEFAULT_H       150
#define MAX_PATH        512u

/* Text, not pointers: a module's data is not relocated (only its GOT is) */
static const char g_names[] =
    "|linearGradient|radialGradient|clipPath|textPath|foreignObject|viewBox|preserveAspectRatio"
    "|gradientUnits|gradientTransform|spreadMethod|patternUnits|patternContentUnits|patternTransform"
    "|clipPathUnits|maskUnits|maskContentUnits|pathLength|textLength|lengthAdjust|refX|refY"
    "|markerWidth|markerHeight|markerUnits|stdDeviation|xlink:href|";

typedef struct
{
    void*   file;
    bool    failed;
    char    color[8];                           /* #rrggbb of currentColor */
} writer_t;

/* A number at *s (after spaces and commas), in 1/1000; *s moves past it */
static bool scan(const char** s, int64_t* milli)
{
    const char* p = *s;
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
        p++;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    int64_t v = 0, scale = 1000;
    bool digits = false;
    for (; *p >= '0' && *p <= '9'; p++, digits = true)
        v = v * 10 + (*p - '0') * 1000;
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, digits = true)
            if (scale > 1)
                v += (*p - '0') * (scale /= 10);
    if (!digits)
        return false;
    *milli = negative ? -v : v;
    *s = p;
    return true;
}

/* A length attribute: a number in px or of no unit, in 1/1000 px; false: none (or %, em, ...) */
static bool number(const char* s, int64_t* milli)
{
    if (s == NULL || !scan(&s, milli))
        return false;
    if (strncmp(s, "px", 2) == 0)
        s += 2;
    while (*s == ' ')
        s++;
    return *s == '\0' && *milli > 0;
}

/* The viewBox's width and height in 1/1000 px */
static bool view_box(const node_t* n, int64_t* w, int64_t* h)
{
    const char* s = node_attr(n, "viewbox");
    int64_t v[4];
    for (int i = 0; i < 4; i++)
        if (s == NULL || !scan(&s, &v[i]))
            return false;
    if (v[2] <= 0 || v[3] <= 0)
        return false;
    *w = v[2];
    *h = v[3];
    return true;
}

bool svg_size(const node_t* n, int32_t* w, int32_t* h)
{
    int64_t aw, ah, vw, vh;
    bool has_w = number(node_attr(n, "width"), &aw), has_h = number(node_attr(n, "height"), &ah);
    bool has_box = view_box(n, &vw, &vh);
    if (has_w && !has_h)
        ah = has_box ? aw * vh / vw : DEFAULT_H * 1000;
    if (has_h && !has_w)
        aw = has_box ? ah * vw / vh : DEFAULT_W * 1000;
    if (!has_w && !has_h)
    {
        aw = has_box ? vw : DEFAULT_W * 1000;
        ah = has_box ? vh : DEFAULT_H * 1000;
    }
    *w = (int32_t)(aw * U / 1000);
    *h = (int32_t)(ah * U / 1000);
    return true;
}

/* ---- Writing ---- */

static void put(writer_t* wr, const char* s, size_t length)
{
    if (!wr->failed && length != 0 && Dmod_FileWrite(s, 1, length, wr->file) != length)
        wr->failed = true;
}

static void puts_(writer_t* wr, const char* s)
{
    put(wr, s, strlen(s));
}

/* `s` with &, <, > and " escaped; currentColor as the color */
static void escaped(writer_t* wr, const char* s, size_t length)
{
    const char* end = s + length;
    while (s < end)
    {
        const char* run = s;
        while (s < end && *s != '&' && *s != '<' && *s != '>' && *s != '"' &&
               !((size_t)(end - s) >= 12 && (*s == 'c' || *s == 'C') && strncmp(s + 1, "urrentColor", 11) == 0) &&
               !((size_t)(end - s) >= 12 && strncmp(s, "currentcolor", 12) == 0))
            s++;
        put(wr, run, (size_t)(s - run));
        if (s >= end)
            break;
        switch (*s)
        {
            case '&': puts_(wr, "&amp;"); s++; break;
            case '<': puts_(wr, "&lt;"); s++; break;
            case '>': puts_(wr, "&gt;"); s++; break;
            case '"': puts_(wr, "&quot;"); s++; break;
            default:  puts_(wr, wr->color); s += 12; break;
        }
    }
}

/* A name as SVG spells it */
static void name(writer_t* wr, const char* s)
{
    size_t n = strlen(s);
    for (const char* p = g_names; *p != '\0'; )
    {
        const char* e = strchr(p + 1, '|');
        if (e == NULL)
            break;
        size_t k = (size_t)(e - p - 1);
        if (k == n)
        {
            bool same = true;
            for (size_t i = 0; i < n && same; i++)
            {
                char a = p[1 + i];
                same = ((a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a) == s[i];
            }
            if (same)
            {
                put(wr, p + 1, n);
                return;
            }
        }
        p = e;
    }
    put(wr, s, n);
}

static void attribute(writer_t* wr, const char* n, const char* value)
{
    puts_(wr, " ");
    name(wr, n);
    puts_(wr, "=\"");
    escaped(wr, value, strlen(value));
    puts_(wr, "\"");
}

static void element(writer_t* wr, const node_t* n, uint32_t depth)
{
    if (n->kind == NODE_TEXT)
    {
        escaped(wr, n->text, n->length);
        return;
    }
    if (n->kind != NODE_ELEMENT || n->tag == NULL || depth > 64u)
        return;
    puts_(wr, "<");
    name(wr, n->tag);
    for (const attr_t* a = n->attrs; a != NULL; a = a->next)
        attribute(wr, a->name, (a->value != NULL) ? a->value : "");
    if (n->first == NULL)
    {
        puts_(wr, "/>");
        return;
    }
    puts_(wr, ">");
    for (const node_t* k = n->first; k != NULL; k = k->next)
        element(wr, k, depth + 1U);
    puts_(wr, "</");
    name(wr, n->tag);
    puts_(wr, ">");
}

char* svg_export(conv_t* c, const node_t* n, int32_t width, int32_t height)
{
    char path[MAX_PATH], number_text[64];
    const char* dir = Dmod_GetEnv("TMPDIR");
    if (dir == NULL || *dir == '\0')
        dir = "/tmp";
    const char* page = strrchr(c->path, '/');
    page = (page != NULL) ? page + 1 : c->path;
    const char* dot = strrchr(page, '.');
    int stem = (int)((dot != NULL && dot != page) ? (size_t)(dot - page) : strlen(page));
    Dmod_SnPrintf(path, sizeof(path), "%s/%.*s-svg%u.svg", dir, stem, page, (unsigned)n->index);

    writer_t wr;
    memset(&wr, 0, sizeof(wr));
    uint32_t color = (n->style != NULL) ? n->style->color : 0;
    Dmod_SnPrintf(wr.color, sizeof(wr.color), "#%06x", (unsigned)(color & 0xFFFFFFu));
    if ((wr.file = Dmod_FileOpen(path, "wb")) == NULL)
    {
        DMOD_LOG_ERROR("dmvs_html: cannot write %s\n", path);
        return NULL;
    }

    /* The root: sized as the box; what it was drawn in, its viewBox (or its own size) */
    puts_(&wr, "<svg xmlns=\"http://www.w3.org/2000/svg\"");
    Dmod_SnPrintf(number_text, sizeof(number_text), " width=\"%d\" height=\"%d\"", (int)width, (int)height);
    puts_(&wr, number_text);
    int64_t vw, vh, aw, ah;
    if (!view_box(n, &vw, &vh) && number(node_attr(n, "width"), &aw) && number(node_attr(n, "height"), &ah))
    {
        Dmod_SnPrintf(number_text, sizeof(number_text), " viewBox=\"0 0 %d.%03d %d.%03d\"",
                      (int)(aw / 1000), (int)(aw % 1000), (int)(ah / 1000), (int)(ah % 1000));
        puts_(&wr, number_text);
    }
    for (const attr_t* a = n->attrs; a != NULL; a = a->next)
    {
        if (strcmp(a->name, "width") != 0 && strcmp(a->name, "height") != 0 && strcmp(a->name, "class") != 0 &&
            strcmp(a->name, "xmlns") != 0 && strcmp(a->name, "style") != 0)
            attribute(&wr, a->name, (a->value != NULL) ? a->value : "");
    }
    puts_(&wr, ">");
    for (const node_t* k = n->first; k != NULL; k = k->next)
        element(&wr, k, 1);
    puts_(&wr, "</svg>\n");
    Dmod_FileClose(wr.file);
    if (wr.failed)
    {
        DMOD_LOG_ERROR("dmvs_html: cannot write %s\n", path);
        return NULL;
    }
    return arena_strndup(&c->arena, path, strlen(path));
}
