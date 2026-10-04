#define DMOD_ENABLE_REGISTRATION    ON
#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * dmvs_html - the dmvsi converter of HTML pages: a still page (no scripts)
 * of HTML and CSS - its <style> and <link> style sheets, the style
 * attributes, Tailwind CSS classes when it loads Tailwind's Play CDN -
 * laid out for a screen and described as a dmvsi document.
 *
 * Resources (style sheets, fonts, images) are files next to the page, or
 * somewhere the options' maps say - a URL (a CDN) is read only through a
 * map. The page is laid out in a viewport of the options' size (default
 * 480 x 272); the view is the whole viewport, or the element the options
 * name (root), at its size.
 *
 * DMVS_HTML_DUMP=<file> writes every element's box there (JSON) - to compare
 * the layout with a browser's.
 */

#define DEFAULT_WIDTH       480
#define DEFAULT_HEIGHT      272
#define CHUNK_SIZE          (64u * 1024u)
#define MAX_FILE            (8u * 1024u * 1024u)
#define MAX_URL             1024u

/* ---- Arena ---- */

void* arena_alloc(arena_t* a, size_t size)
{
    size = (size + 7U) & ~(size_t)7U;
    if (a->failed)
        return NULL;
    chunk_t* ch = a->chunks;
    if (ch == NULL || ch->used + size > ch->size)
    {
        size_t capacity = (size > CHUNK_SIZE) ? size : CHUNK_SIZE;
        ch = Dmod_Malloc(sizeof(chunk_t) + capacity);
        if (ch == NULL)
        {
            a->failed = true;
            return NULL;
        }
        ch->next = a->chunks;
        ch->used = 0;
        ch->size = capacity;
        a->chunks = ch;
    }
    void* p = (uint8_t*)(ch + 1) + ch->used;
    ch->used += size;
    memset(p, 0, size);
    return p;
}

char* arena_strndup(arena_t* a, const char* s, size_t length)
{
    char* copy = arena_alloc(a, length + 1U);
    if (copy != NULL)
    {
        memcpy(copy, s, length);
        copy[length] = '\0';
    }
    return copy;
}

void arena_free(arena_t* a)
{
    while (a->chunks != NULL)
    {
        chunk_t* ch = a->chunks;
        a->chunks = ch->next;
        Dmod_Free(ch);
    }
}

/* ---- Resources ---- */

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool starts_with(const char* s, const char* prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* "https://host" of a URL: its length, 0 when it is not one */
static size_t origin_length(const char* url)
{
    const char* scheme = url;
    while ((*scheme >= 'a' && *scheme <= 'z') || (*scheme >= 'A' && *scheme <= 'Z') || *scheme == '+' || *scheme == '-')
        scheme++;
    if (scheme == url || strncmp(scheme, "://", 3) != 0)
        return 0;
    const char* host = scheme + 3;
    while (*host != '\0' && *host != '/' && *host != '?' && *host != '#')
        host++;
    return (size_t)(host - url);
}

/* "a/b/../c/./d" -> "a/c/d", in place, from `from` on */
static void normalize(char* s, size_t from)
{
    char* out = s + from;
    const char* in = s + from;
    while (*in != '\0')
    {
        if (in[0] == '.' && (in[1] == '/' || in[1] == '\0') && (in == s + from || in[-1] == '/'))
        {
            in += (in[1] == '/') ? 2 : 1;
            continue;
        }
        if (in[0] == '.' && in[1] == '.' && (in[2] == '/' || in[2] == '\0') && (in == s + from || in[-1] == '/'))
        {
            /* Back over the last segment written */
            if (out > s + from)
            {
                out--;
                while (out > s + from && out[-1] != '/')
                    out--;
            }
            else if (from == 0)
            {
                /* A relative path going up: kept */
                memmove(out, in, 3);
                out += (in[2] == '/') ? 3 : 2;
            }
            in += (in[2] == '/') ? 3 : 2;
            continue;
        }
        if (in[0] == '?' || in[0] == '#')
        {
            while (*in != '\0')
                *out++ = *in++;
            break;
        }
        *out++ = *in++;
    }
    *out = '\0';
}

char* resource_url(conv_t* c, const char* base, const char* url, size_t length)
{
    char buffer[MAX_URL];
    while (length > 0 && is_space(*url))
    {
        url++;
        length--;
    }
    while (length > 0 && is_space(url[length - 1U]))
        length--;
    if (length >= 2U && (url[0] == '"' || url[0] == '\'') && url[length - 1U] == url[0])
    {
        url++;
        length -= 2U;
    }
    if (length == 0 || length >= MAX_URL || (length >= 5U && strncmp(url, "data:", 5) == 0))
        return NULL;
    memcpy(buffer, url, length);
    buffer[length] = '\0';

    if (origin_length(buffer) > 0)
    {
        if (starts_with(buffer, "file://"))
            return arena_strndup(&c->arena, buffer + 7, strlen(buffer + 7));
        char* out = arena_strndup(&c->arena, buffer, length);
        if (out != NULL)
            normalize(out, origin_length(out));
        return out;
    }
    char joined[2 * MAX_URL];
    size_t base_origin = (base != NULL) ? origin_length(base) : 0;
    if (buffer[0] == '/' && buffer[1] == '/')
        Dmod_SnPrintf(joined, sizeof(joined), "https:%s", buffer);
    else if (buffer[0] == '/')
    {
        if (base_origin > 0)
        {
            memcpy(joined, base, base_origin);
            Dmod_SnPrintf(joined + base_origin, sizeof(joined) - base_origin, "%s", buffer);
        }
        else
            Dmod_SnPrintf(joined, sizeof(joined), "%s", buffer);
    }
    else
    {
        /* Next to the base: its directory */
        size_t dir = 0;
        if (base != NULL)
        {
            size_t n = strlen(base);
            for (size_t i = 0; i < n && base[i] != '?' && base[i] != '#'; i++)
            {
                if (base[i] == '/')
                    dir = i + 1U;
            }
            if (dir < base_origin)
                dir = 0;
        }
        if (dir + strlen(buffer) + 2U > sizeof(joined))
            return NULL;
        memcpy(joined, base, dir);
        strcpy(joined + dir, buffer);
    }
    char* out = arena_strndup(&c->arena, joined, strlen(joined));
    if (out != NULL)
        normalize(out, origin_length(out));
    return out;
}

char* resource_path(conv_t* c, const char* url)
{
    char buffer[2 * MAX_URL];
    const char* path = NULL;
    for (uint32_t i = 0; i < c->options->map_count && path == NULL; i++)
    {
        const dmvsi_map_t* m = &c->options->maps[i];
        if (m->from == NULL || m->to == NULL || !starts_with(url, m->from))
            continue;
        size_t from = strlen(m->from), to = strlen(m->to);
        if (to > 0 && m->to[to - 1U] == '/')
            Dmod_SnPrintf(buffer, sizeof(buffer), "%s%s", m->to, url + from);
        else
            Dmod_SnPrintf(buffer, sizeof(buffer), "%s", m->to);
        path = buffer;
    }
    if (path == NULL)
    {
        if (origin_length(url) > 0)
            return NULL;            /* A URL nothing maps: not read */
        path = url;
    }
    size_t n = 0;
    while (path[n] != '\0' && path[n] != '?' && path[n] != '#')
        n++;
    return arena_strndup(&c->arena, path, n);
}

/* Relative to the base URL first (the maps may say where it is); when nothing
 * maps that, relative to the file the base URL is mapped to - a style sheet
 * that stands in for a CDN's names its files next to itself */
char* resolve_resource(conv_t* c, const char* base, const char* url, size_t length)
{
    char* absolute = resource_url(c, base, url, length);
    char* path = (absolute != NULL) ? resource_path(c, absolute) : NULL;
    if (path == NULL && base != NULL && origin_length(base) > 0)
    {
        char* local = resource_path(c, base);
        char* again = (local != NULL) ? resource_url(c, local, url, length) : NULL;
        path = (again != NULL) ? resource_path(c, again) : NULL;
    }
    return path;
}

char* read_resource(conv_t* c, const char* path, size_t* size)
{
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t total = 0;
    Dmod_FileSize_t fs = Dmod_FileSize(f);
    if (!Dmod_FileSizeToSizeT(fs, &total) || total > MAX_FILE)
    {
        Dmod_FileClose(f);
        return NULL;
    }
    char* text = arena_alloc(&c->arena, total + 1U);
    size_t got = (text != NULL) ? Dmod_FileRead(text, 1, total, f) : 0;
    Dmod_FileClose(f);
    if (text == NULL)
        return NULL;
    text[got] = '\0';
    *size = got;
    return text;
}

/* ---- Images ---- */

static uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le16(const uint8_t* p) { return ((uint32_t)p[1] << 8) | p[0]; }
static uint32_t le32(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }

bool image_size(const char* path, int32_t* width, int32_t* height)
{
    uint8_t h[32];
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return false;
    size_t n = Dmod_FileRead(h, 1, sizeof(h), f);
    bool ok = false;
    if (n >= 24 && memcmp(h, "\x89PNG", 4) == 0)
    {
        *width = (int32_t)be32(h + 16);
        *height = (int32_t)be32(h + 20);
        ok = true;
    }
    else if (n >= 10 && memcmp(h, "GIF8", 4) == 0)
    {
        *width = (int32_t)le16(h + 6);
        *height = (int32_t)le16(h + 8);
        ok = true;
    }
    else if (n >= 26 && h[0] == 'B' && h[1] == 'M')
    {
        *width = (int32_t)le32(h + 18);
        int32_t ht = (int32_t)le32(h + 22);
        *height = (ht < 0) ? -ht : ht;
        ok = true;
    }
    else if (n >= 4 && h[0] == 0xFF && h[1] == 0xD8)
    {
        /* JPEG: the markers up to a start of frame */
        uint32_t at = 2;
        for (int guard = 0; guard < 512 && !ok; guard++)
        {
            uint8_t m[9];
            if (Dmod_FileSeek(f, (Dmod_FileOffset_t)at, DMOD_SEEK_SET) != 0 || Dmod_FileRead(m, 1, sizeof(m), f) != sizeof(m) || m[0] != 0xFF)
                break;
            uint8_t marker = m[1];
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
            {
                *height = (int32_t)be16(m + 5);
                *width = (int32_t)be16(m + 7);
                ok = true;
                break;
            }
            at += 2U + be16(m + 2);
        }
    }
    Dmod_FileClose(f);
    return ok && *width > 0 && *height > 0;
}

/* ---- Style sheets of the page ---- */

static bool attr_has(const char* value, const char* word)
{
    size_t n = strlen(word);
    for (const char* s = value; s != NULL && *s != '\0'; s++)
    {
        bool start = s == value || is_space(s[-1]);
        bool end = s[n] == '\0' || is_space(s[n]);
        bool same = true;
        for (size_t i = 0; i < n && same; i++)
        {
            char ch = s[i];
            same = ((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch) == word[i];
        }
        if (start && end && same)
            return true;
    }
    return false;
}

static bool has_text(const char* s, const char* what)
{
    size_t n = strlen(what);
    for (; s != NULL && *s != '\0'; s++)
    {
        if (strncmp(s, what, n) == 0)
            return true;
    }
    return false;
}

static void load_sheets(conv_t* c, node_t* n, uint32_t depth)
{
    if (depth > 200U)
        return;
    for (node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (node_is(k, "style"))
        {
            const char* type = node_attr(k, "type");
            if ((type == NULL || has_text(type, "css")) && k->first != NULL && k->first->kind == NODE_TEXT)
                css_parse(c, k->first->text, k->first->length, c->path, LAYER_AUTHOR);
            continue;
        }
        if (node_is(k, "link"))
        {
            const char* rel = node_attr(k, "rel");
            const char* href = node_attr(k, "href");
            if (rel == NULL || href == NULL || !attr_has(rel, "stylesheet"))
                continue;
            char* url = resource_url(c, c->path, href, strlen(href));
            char* path = (url != NULL) ? resource_path(c, url) : NULL;
            size_t size = 0;
            char* text = (path != NULL) ? read_resource(c, path, &size) : NULL;
            if (text != NULL)
                css_parse(c, text, size, url, LAYER_AUTHOR);
            else
                WARN(c, "the style sheet %s is not read (map it to a file: -m URL=FILE)\n", (url != NULL) ? url : href);
            continue;
        }
        if (node_is(k, "script"))
        {
            const char* src = node_attr(k, "src");
            if (src != NULL && has_text(src, "tailwindcss"))
                c->tailwind = true;
            continue;
        }
        load_sheets(c, k, depth + 1U);
    }
}

/* ---- The layout, for comparing with a browser's ---- */

static void dump_node(void* f, const node_t* n, uint32_t depth, bool* first)
{
    if (depth > 200U)
        return;
    for (const node_t* k = n->first; k != NULL; k = k->next)
    {
        if (k->kind != NODE_ELEMENT || k->style == NULL || k->style->display == DISPLAY_NONE)
            continue;
        if (k->box.placed)
        {
            Dmod_FPrintf(f, "%s{\"d\":%u,\"tag\":\"%s\",\"id\":\"%s\",\"x\":%d.%02d,\"y\":%d.%02d,\"w\":%d.%02d,\"h\":%d.%02d}\n",
                         *first ? "" : ",", (unsigned)depth, k->tag, (k->id != NULL) ? k->id : "",
                         (int)(k->box.ax / U), (int)(((k->box.ax < 0 ? -k->box.ax : k->box.ax) % U) * 100 / U),
                         (int)(k->box.ay / U), (int)(((k->box.ay < 0 ? -k->box.ay : k->box.ay) % U) * 100 / U),
                         (int)(k->box.w / U), (int)((k->box.w % U) * 100 / U), (int)(k->box.h / U), (int)((k->box.h % U) * 100 / U));
            *first = false;
        }
        dump_node(f, k, depth + 1U, first);
    }
}

static void dump(const conv_t* c, const char* path)
{
    void* f = Dmod_FileOpen(path, "wb");
    if (f == NULL)
        return;
    bool first = true;
    Dmod_FPrintf(f, "[\n");
    dump_node(f, c->document, 0, &first);
    Dmod_FPrintf(f, "]\n");
    Dmod_FileClose(f);
}

/* ---- DIF ---- */

dmod_dmvsi_dif_api_declaration(1.0, dmvs_html, bool, _probe, ( const char* path, const uint8_t* head, size_t size ))
{
    const char* dot = (path != NULL) ? strrchr(path, '.') : NULL;
    if (dot != NULL)
    {
        char ext[8];
        size_t n = 0;
        for (const char* s = dot + 1; *s != '\0' && n + 1U < sizeof(ext); s++)
            ext[n++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s - 'A' + 'a') : *s;
        ext[n] = '\0';
        if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0 || strcmp(ext, "xhtml") == 0)
            return true;
    }
    size_t i = 0;
    if (size >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
        i = 3;
    while (i < size && is_space((char)head[i]))
        i++;
    static const char tags[2][16] = { "<!doctype html", "<html" };
    for (size_t t = 0; t < 2U; t++)
    {
        size_t n = strlen(tags[t]);
        if (size - i < n)
            continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++)
        {
            char ch = (char)head[i + k];
            same = ((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch) == tags[t][k];
        }
        if (same)
            return true;
    }
    return false;
}

/* ---- A page as a script changed it ---- */

static node_t* element_at(node_t* n, uint32_t index, uint32_t depth)
{
    for (node_t* k = n->first; k != NULL && depth < 200U; k = k->next)
    {
        if (k->kind != NODE_ELEMENT)
            continue;
        if (k->index == index)
            return k;
        node_t* found = element_at(k, index, depth + 1U);
        if (found != NULL)
            return found;
    }
    return NULL;
}

/* "backgroundColor" -> "background-color" */
static void kebab(const char* name, char* out, size_t size)
{
    size_t n = 0;
    for (const char* s = name; *s != '\0' && n + 2U < size; s++)
    {
        if (*s >= 'A' && *s <= 'Z')
        {
            out[n++] = '-';
            out[n++] = (char)(*s - 'A' + 'a');
        }
        else
            out[n++] = *s;
    }
    out[n] = '\0';
}

static void apply_mod(conv_t* c, const mod_t* m)
{
    node_t* n = element_at(c->document, m->element, 0);
    if (n == NULL)
        return;
    if (m->kind == MOD_STYLE)
    {
        /* As element.style.<name> = value does: the style attribute, at its end */
        char property[64];
        kebab(m->name, property, sizeof(property));
        attr_t* style = NULL;
        for (attr_t* a = n->attrs; a != NULL; a = a->next)
        {
            if (a->name != NULL && strcmp(a->name, "style") == 0)
                style = a;
        }
        if (style == NULL)
        {
            style = arena_alloc(&c->arena, sizeof(*style));
            if (style == NULL)
                return;
            style->name = arena_strndup(&c->arena, "style", 5);
            style->value = arena_strndup(&c->arena, "", 0);
            style->next = n->attrs;
            n->attrs = style;
        }
        size_t length = strlen(style->value) + strlen(property) + strlen(m->value) + 4U;
        char* value = arena_alloc(&c->arena, length);
        if (value != NULL)
        {
            Dmod_SnPrintf(value, length, "%s;%s:%s", style->value, property, m->value);
            style->value = value;
        }
        return;
    }
    /* A class added or removed */
    uint32_t at = n->class_count;
    for (uint32_t i = 0; i < n->class_count; i++)
    {
        if (strcmp(n->classes[i], m->name) == 0)
            at = i;
    }
    if (m->kind == MOD_CLASS_REMOVE && at < n->class_count)
    {
        for (uint32_t i = at + 1U; i < n->class_count; i++)
            n->classes[i - 1U] = n->classes[i];
        n->class_count--;
    }
    else if (m->kind == MOD_CLASS_ADD && at == n->class_count)
    {
        char** classes = arena_alloc(&c->arena, (n->class_count + 1U) * sizeof(char*));
        if (classes == NULL)
            return;
        if (n->class_count > 0)
            memcpy(classes, n->classes, n->class_count * sizeof(char*));
        classes[n->class_count++] = (char*)m->name;
        n->classes = classes;
    }
}

int run_layout(conv_t* c)
{
    size_t size = 0;
    char* text = (c->path != NULL) ? read_resource(c, c->path, &size) : NULL;
    if (text == NULL)
        return c->arena.failed ? -ENOMEM : -ENOENT;
    if ((c->document = html_parse(c, text, size)) == NULL)
        return c->arena.failed ? -ENOMEM : -EBADMSG;
    for (uint32_t i = 0; i < c->mod_count; i++)
        apply_mod(c, &c->mods[i]);
    load_sheets(c, c->document, 0);
    if (c->tailwind)
        css_parse(c, tailwind_preflight, strlen(tailwind_preflight), c->path, LAYER_TAILWIND);
    style_compute(c, c->document);
    if (!c->arena.failed)
        layout_page(c, c->document);
    return c->arena.failed ? -ENOMEM : 0;
}

dmod_dmvsi_dif_api_declaration(1.0, dmvs_html, int, _convert, ( const char* path, const dmvsi_options_t* options, dmvsi_doc_t doc ))
{
    conv_t* c = Dmod_Malloc(sizeof(*c));
    if (c == NULL)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));
    c->options = options;
    c->doc = doc;
    c->vw = ((options->width != 0) ? options->width : DEFAULT_WIDTH) * U;
    c->vh = ((options->height != 0) ? options->height : DEFAULT_HEIGHT) * U;
    c->path = arena_strndup(&c->arena, path, strlen(path));

    int status = (c->path != NULL) ? run_layout(c) : -ENOMEM;
    if (status == 0)
        status = script_compile(c);     /* What its scripts do: variables, handlers */
    if (status == 0)
        status = paint_page(c, c->document);
    if (c->arena.failed)
        status = -ENOMEM;
    const char* dump_path = Dmod_GetEnv("DMVS_HTML_DUMP");
    if (status == 0 && dump_path != NULL && dump_path[0] != '\0')
        dump(c, dump_path);
    if (c->warnings > 0)
        DMOD_LOG_INFO("dmvs_html: %s: %u warnings\n", path, (unsigned)c->warnings);
    arena_free(&c->arena);
    Dmod_Free(c);
    return status;
}

/* ---- Module ---- */

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
