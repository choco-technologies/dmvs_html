#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmvsi.h"
#include <errno.h>
#include <string.h>

/*
 * dmvs_html through dmvsi: a real page (fixtures/dmodos - Tailwind CSS,
 * Font Awesome, Inter; see fixtures/README.md) whose boxes are compared
 * with the ones Chrome lays it out in, and small pages for single features.
 */

#ifndef DMVS_HTML_FIXTURES_DIR
#define DMVS_HTML_FIXTURES_DIR  "fixtures"
#endif
#ifndef DMVS_HTML_TEST_DIR
#define DMVS_HTML_TEST_DIR      "."
#endif
#define FIXTURE(name)           DMVS_HTML_FIXTURES_DIR "/dmodos/" name
#define TEST_FILE(name)         DMVS_HTML_TEST_DIR "/" name
#define FA                      "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/"

#define TOLERANCE               (2 * DMVSI_UNIT)        /* Text is measured a little differently than in a browser */

static int32_t dist(int32_t a, int32_t b) { return (a > b) ? a - b : b - a; }

/* The first node of `kind` matching: text, or a fill of the rectangle (at TOLERANCE) */
static const dmvsi_node_t* find(const dmvsi_node_t* n, uint8_t kind, const char* text, const dmvsi_rect_t* r)
{
    for (; n != NULL; n = n->next)
    {
        if (n->kind == kind)
        {
            if (kind == DMVSI_NODE_TEXT && text != NULL && strcmp(n->u.text.text, text) == 0)
                return n;
            if (kind == DMVSI_NODE_RECT && r != NULL && dist(n->u.fill.rect.x, r->x) <= TOLERANCE &&
                dist(n->u.fill.rect.y, r->y) <= TOLERANCE && dist(n->u.fill.rect.w, r->w) <= TOLERANCE &&
                dist(n->u.fill.rect.h, r->h) <= TOLERANCE)
                return n;
        }
        if (n->kind == DMVSI_NODE_GROUP)
        {
            const dmvsi_node_t* f = find(n->first, kind, text, r);
            if (f != NULL)
                return f;
        }
    }
    return NULL;
}

/* x, y, w, h in 1/100 pixel (as Chrome gives them, to two decimals) */
static dmvsi_rect_t px100(int32_t x, int32_t y, int32_t w, int32_t h)
{
    dmvsi_rect_t r = { x * DMVSI_UNIT / 100, y * DMVSI_UNIT / 100, w * DMVSI_UNIT / 100, h * DMVSI_UNIT / 100 };
    return r;
}

static dmvsi_doc_t convert(const char* path, const char* root, uint16_t w, uint16_t h, int* status)
{
    /* Text, not pointers, in a module's data (only its GOT is relocated): filled in here */
    dmvsi_map_t maps[4] = {
        { FA "css/all.min.css", FIXTURE("fontawesome.css") },
        { FA "webfonts/fa-solid-900.ttf", FIXTURE("fonts/icons-solid.ttf") },
        { FA "webfonts/fa-brands-400.ttf", FIXTURE("fonts/icons-brands.ttf") },
        { "https://fonts.googleapis.com/css2", FIXTURE("inter.css") },
    };
    dmvsi_options_t o;
    memset(&o, 0, sizeof(o));
    o.root = root;
    o.width = w;
    o.height = h;
    o.maps = maps;
    o.map_count = sizeof(maps) / sizeof(maps[0]);
    return dmvsi_convert_file(path, &o, status);
}

DMOD_TEST_STEP(dmvs_html_lays_a_page_out_as_a_browser)
{
    int status = -1;
    dmvsi_doc_t doc = convert(FIXTURE("dmodos.html"), "os-screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    DMOD_TEST_EXPECT_TRUE(doc != NULL);
    if (doc == NULL)
        return;
    DMOD_TEST_EXPECT_TRUE(strcmp(dmvsi_converter_name(doc), "dmvs_html") == 0);
    uint16_t w = 0, h = 0;
    DMOD_TEST_EXPECT_EQ(dmvsi_view_size(doc, &w, &h), 0);
    DMOD_TEST_EXPECT_EQ(w, 272);                         /* The root's size: .os-screen */
    DMOD_TEST_EXPECT_EQ(h, 480);
    const dmvsi_node_t* root = dmvsi_root(doc);

    /* Boxes where Chrome has them (relative to .os-screen) */
    dmvsi_rect_t status_bar = px100(0, 0, 27200, 2400);           /* h-6 bg-black/30 */
    dmvsi_rect_t clock = px100(1600, 4000, 24000, 11200);        /* glass-panel h-28 */
    dmvsi_rect_t weather = px100(1600, 16400, 24000, 5600);      /* glass-panel h-14 */
    dmvsi_rect_t tile = px100(11198, 32900, 4800, 4800);         /* the store's tile */
    const dmvsi_node_t* n = find(root->first, DMVSI_NODE_RECT, NULL, &status_bar);
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.paint.color == 0x4D000000u);
    n = find(root->first, DMVSI_NODE_RECT, NULL, &clock);
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.radius == DMVSI_PX(12) && n->u.fill.paint.color == 0x1AFFFFFFu);
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_RECT, NULL, &weather) != NULL);
    n = find(root->first, DMVSI_NODE_RECT, NULL, &tile);
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.paint.kind == DMVSI_PAINT_LINEAR);   /* bg-gradient-to-br */
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.paint.angle == 135 && n->u.fill.paint.stops[0].color == 0xFF6366F1u);

    /* Text: its pen where Chrome's text starts, in Inter of its size */
    struct { char text[24]; int32_t x; int32_t top; int32_t height; uint16_t size; } texts[] = {
        { "dmodOS", 5091, 375, 1650, 11 },
        { "Ustawienia", 2691, 29800, 1500, 10 },
        { "Sklep", 12267, 38300, 1500, 10 },
        { "22\xC2\xB0" "C", 7500, 17450, 2000, 14 },
        { "Poniedzia\xC5\x82" "ek, 17 Lip", 8005, 11400, 1600, 12 },
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
    {
        n = find(root->first, DMVSI_NODE_TEXT, texts[i].text, NULL);
        DMOD_TEST_EXPECT_TRUE(n != NULL);
        if (n == NULL)
            continue;
        dmvsi_font_info_t info;
        DMOD_TEST_EXPECT_EQ(dmvsi_font_info(n->u.text.font, &info), 0);
        DMOD_TEST_EXPECT_EQ(info.size, texts[i].size);
        DMOD_TEST_EXPECT_TRUE(info.file != NULL);
        DMOD_TEST_EXPECT_TRUE(dist(n->u.text.x, texts[i].x * DMVSI_UNIT / 100) <= TOLERANCE);
        /* The baseline within the line box */
        int32_t top = texts[i].top * DMVSI_UNIT / 100, height = texts[i].height * DMVSI_UNIT / 100;
        DMOD_TEST_EXPECT_TRUE(n->u.text.baseline > top && n->u.text.baseline < top + height);
    }

    /* An icon: Font Awesome's ::before, in its font */
    n = find(root->first, DMVSI_NODE_TEXT, "\xEF\x87\xAB", NULL);           /* wifi, U+F1EB */
    dmvsi_font_info_t icons;
    DMOD_TEST_EXPECT_TRUE(n != NULL && dmvsi_font_info(n->u.text.font, &icons) == 0 && icons.size == 10);
    dmvsi_free(doc);
}

static bool write_file(const char* path, const char* text)
{
    void* f = Dmod_FileOpen(path, "wb");
    if (f == NULL)
        return false;
    bool ok = Dmod_FileWrite(text, 1, strlen(text), f) == strlen(text);
    Dmod_FileClose(f);
    return ok;
}

DMOD_TEST_STEP(dmvs_html_computes_styles)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("styles.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0 }"
        ".a { position: absolute; left: 10px; top: 20px; width: calc(50% - 20px); height: 3em; font-size: 10px;"
        "     background: rgba(255, 0, 0, 0.5); border: 2px solid rgb(0 0 255 / 50%); border-radius: 4px }"
        ".b { --size: 30px; position: absolute; left: 0; top: 100px; width: var(--size); height: var(--missing, 15px);"
        "     background-color: #0f08 }"
        ".c:hover { background: red } .hidden { display: none }"
        "@media (max-width: 100px) { .b { width: 99px } }"
        "</style></head><body>"
        "<div class=\"a\"></div><div class=\"b\"></div><div class=\"c hidden\">x</div>"
        "</body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("styles.html"), NULL, 200, 150, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    DMOD_TEST_EXPECT_TRUE(doc != NULL);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);
    /* calc(50% - 20px) of 200 = 80, + the borders: 84 x 34 (3em of 10px + 4) */
    dmvsi_rect_t a = { DMVSI_PX(10), DMVSI_PX(20), DMVSI_PX(84), DMVSI_PX(34) };
    const dmvsi_node_t* n = find(root->first, DMVSI_NODE_RECT, NULL, &a);
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.paint.color == 0x80FF0000u && n->u.fill.radius == DMVSI_PX(4));
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->next != NULL && n->next->kind == DMVSI_NODE_FRAME &&
                          n->next->u.frame.paint.color == 0x800000FFu && n->next->u.frame.width == DMVSI_PX(2));
    /* var() and its fallback; the media query does not hold at 200 px */
    dmvsi_rect_t b = { 0, DMVSI_PX(100), DMVSI_PX(30), DMVSI_PX(15) };
    n = find(root->first, DMVSI_NODE_RECT, NULL, &b);
    DMOD_TEST_EXPECT_TRUE(n != NULL && n->u.fill.paint.color == 0x8800FF00u);
    dmvsi_free(doc);
}

DMOD_TEST_STEP(dmvs_html_lays_flex_out_and_stacks)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("flex.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0 } .row { display: flex; justify-content: space-between; width: 200px; height: 40px }"
        ".row div { width: 50px; background: #111 } .col { display: flex; flex-direction: column; height: 100px }"
        ".grow { flex: 1; background: #222 } .fixed { height: 30px; background: #333 }"
        ".z { display: flex } .z1 { position: absolute; inset: 0; background: #444 }"
        ".z2 { z-index: 1; background: #555; width: 10px; height: 10px }"
        "</style></head><body>"
        "<div class=\"row\"><div></div><div></div><div></div></div>"
        "<div class=\"col\"><div class=\"fixed\"></div><div class=\"grow\"></div></div>"
        "<div class=\"z\"><div class=\"z2\"></div><div class=\"z1\"></div></div>"
        "</body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("flex.html"), NULL, 200, 200, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);
    dmvsi_rect_t r1 = { DMVSI_PX(75), 0, DMVSI_PX(50), DMVSI_PX(40) };       /* the middle one, space-between */
    dmvsi_rect_t r2 = { DMVSI_PX(150), 0, DMVSI_PX(50), DMVSI_PX(40) };
    dmvsi_rect_t grow = { 0, DMVSI_PX(70), DMVSI_PX(200), DMVSI_PX(70) };    /* 100 - 30 */
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_RECT, NULL, &r1) != NULL);
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_RECT, NULL, &r2) != NULL);
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_RECT, NULL, &grow) != NULL);
    /* A flex item with a z-index is painted over a positioned box of z-index auto */
    dmvsi_rect_t z1 = { 0, 0, DMVSI_PX(200), DMVSI_PX(200) }, z2 = { 0, DMVSI_PX(140), DMVSI_PX(10), DMVSI_PX(10) };
    const dmvsi_node_t* a = find(root->first, DMVSI_NODE_RECT, NULL, &z1);
    const dmvsi_node_t* b = find(root->first, DMVSI_NODE_RECT, NULL, &z2);
    bool b_after_a = false;
    for (const dmvsi_node_t* k = a; k != NULL; k = k->next)
        b_after_a = b_after_a || k == b;
    DMOD_TEST_EXPECT_TRUE(a != NULL && b != NULL && b_after_a);
    dmvsi_free(doc);
}

DMOD_TEST_STEP(dmvs_html_reports_what_it_cannot_convert)
{
    int status = 0;
    DMOD_TEST_EXPECT_TRUE(convert(TEST_FILE("missing.html"), NULL, 0, 0, &status) == NULL);
    DMOD_TEST_EXPECT_EQ(status, -ENOENT);
    DMOD_TEST_EXPECT_TRUE(convert(FIXTURE("dmodos.html"), "no-such-id", 0, 0, &status) == NULL);
    DMOD_TEST_EXPECT_EQ(status, -EINVAL);
}

/* The first group (in painting order) named `name` */
static const dmvsi_node_t* group_named(const dmvsi_node_t* n, const char* name)
{
    for (; n != NULL; n = n->next)
    {
        if (n->kind != DMVSI_NODE_GROUP)
            continue;
        if (n->u.group.name != NULL && strcmp(n->u.group.name, name) == 0)
            return n;
        const dmvsi_node_t* f = group_named(n->first, name);
        if (f != NULL)
            return f;
    }
    return NULL;
}

static int32_t var_initial(dmvsi_doc_t doc, dmvsi_var_t var)
{
    int32_t v = -1;
    (void)dmvsi_var_at(doc, var - 1U, NULL, &v);
    return v;
}


/* A handler's actions with the handlers it calls in their place (scripts are compiled into calls) */
static dmvsi_action_t g_flat[256];

static uint32_t flatten_into(dmvsi_doc_t doc, dmvsi_handler_t h, uint32_t n, uint32_t depth)
{
    const dmvsi_action_t* a = NULL;
    uint32_t count = dmvsi_handler_actions(doc, h, &a);
    for (uint32_t i = 0; i < count && n < 256u; i++)
    {
        if (a[i].kind == DMVSI_ACT_CALL && depth < 8u)
            n = flatten_into(doc, a[i].handler, n, depth + 1U);
        else
            g_flat[n++] = a[i];
    }
    return n;
}

static uint32_t flat_actions(dmvsi_doc_t doc, dmvsi_handler_t h, const dmvsi_action_t** actions)
{
    *actions = g_flat;
    return flatten_into(doc, h, 0, 0);
}

DMOD_TEST_STEP(dmvs_html_converts_what_scripts_do)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("script.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0 } #screen { position: relative; width: 200px; height: 100px; overflow: hidden }"
        "#home { position: absolute; inset: 0; transition: opacity 200ms linear }"
        ".open { width: 50px; height: 20px }"
        "#win { position: absolute; left: 0; top: 100%; width: 200px; height: 100px; background: #222;"
        "       transition: top 0.3s ease-out } #win.active { top: 0 }"
        "#light { width: 20px; height: 20px; background: #555 } #light.on { margin-left: 30px }"
        "</style></head><body><div id=\"screen\">"
        "<div id=\"home\"><div class=\"open\" onclick=\"show('win')\"></div>"
        "<div id=\"light\" onclick=\"this.classList.toggle('on')\"></div></div>"
        "<div id=\"win\"><div class=\"open\" onclick=\"hide()\"></div></div>"
        "</div><script>"
        "const home = document.getElementById('home');\n"
        "let current = null;\n"
        "function show(id) { const w = document.getElementById(id); w.classList.add('active');"
        " home.style.opacity = '0.5'; current = w; }\n"
        "function hide() { if (current) { current.classList.remove('active'); home.style.opacity = '1'; current = null; } }\n"
        "setInterval(tick, 1000);\n"
        "</script></body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("script.html"), "screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);

    /* The window: its y a variable - off the screen (100 px down) until it is shown */
    const dmvsi_node_t* win = group_named(root->first, "win");
    DMOD_TEST_EXPECT_TRUE(win != NULL && win->bind[DMVSI_BIND_Y] != 0 && win->bind[DMVSI_BIND_X] == 0);
    DMOD_TEST_EXPECT_TRUE(win != NULL && var_initial(doc, win->bind[DMVSI_BIND_Y]) == DMVSI_PX(100));
    const dmvsi_node_t* home = group_named(root->first, "home");
    DMOD_TEST_EXPECT_TRUE(home != NULL && home->bind[DMVSI_BIND_OPACITY] != 0 && var_initial(doc, home->bind[DMVSI_BIND_OPACITY]) == 255);

    /* show('win'): the window up (its transition: 300 ms, ease-out), home at half, current = the window */
    const dmvsi_node_t* open = (home != NULL) ? home->first : NULL;
    while (open != NULL && open->click == 0)
        open = open->next;
    DMOD_TEST_EXPECT_TRUE(open != NULL);
    if (open != NULL && win != NULL && home != NULL)
    {
        const dmvsi_action_t* a = NULL;
        uint32_t n = flat_actions(doc, open->click, &a);
        DMOD_TEST_EXPECT_EQ(n, 3u);
        DMOD_TEST_EXPECT_TRUE(n >= 3 && a[0].kind == DMVSI_ACT_ANIMATE && a[0].var == win->bind[DMVSI_BIND_Y] && a[0].value == 0 &&
                              a[0].duration == 300 && a[0].easing[2] == 580);
        DMOD_TEST_EXPECT_TRUE(n >= 3 && a[1].kind == DMVSI_ACT_ANIMATE && a[1].var == home->bind[DMVSI_BIND_OPACITY] &&
                              a[1].value == 128 && a[1].duration == 200);
        DMOD_TEST_EXPECT_TRUE(n >= 3 && a[2].kind == DMVSI_ACT_SET && a[2].value != 0);
    }

    /* hide(): only when something is shown - IF current != 0, back, current = 0 */
    const dmvsi_node_t* close = (win != NULL) ? win->first : NULL;
    while (close != NULL && close->kind == DMVSI_NODE_GROUP && close->click == 0 && close->first != NULL && close->first->kind == DMVSI_NODE_GROUP)
        close = close->first;
    while (close != NULL && close->click == 0)
        close = close->next;
    DMOD_TEST_EXPECT_TRUE(close != NULL);
    if (close != NULL)
    {
        const dmvsi_action_t* a = NULL;
        uint32_t n = flat_actions(doc, close->click, &a);
        DMOD_TEST_EXPECT_TRUE(n >= 6 && a[0].kind == DMVSI_ACT_IF_NE && a[0].value == 0);
        DMOD_TEST_EXPECT_TRUE(n >= 6 && a[1].kind == DMVSI_ACT_IF_EQ);                     /* current is the window */
        DMOD_TEST_EXPECT_TRUE(n >= 6 && a[2].kind == DMVSI_ACT_ANIMATE && a[2].value == DMVSI_PX(100));
        DMOD_TEST_EXPECT_TRUE(n >= 6 && a[n - 1U].kind == DMVSI_ACT_END);
    }

    /* this.classList.toggle('on'): a variable of the class, the light moved by it */
    const dmvsi_node_t* light = group_named(root->first, "light");
    DMOD_TEST_EXPECT_TRUE(light != NULL && light->click != 0 && light->bind[DMVSI_BIND_X] != 0);
    if (light != NULL)
    {
        const dmvsi_action_t* a = NULL;
        uint32_t n = flat_actions(doc, light->click, &a);
        DMOD_TEST_EXPECT_TRUE(n == 8 && a[0].kind == DMVSI_ACT_TOGGLE && a[1].kind == DMVSI_ACT_IF_NE);
        DMOD_TEST_EXPECT_TRUE(n == 8 && a[3].kind == DMVSI_ACT_SET && a[3].value == DMVSI_PX(30));    /* no transition: at once */
        DMOD_TEST_EXPECT_TRUE(n == 8 && a[4].kind == DMVSI_ACT_ELSE && a[6].value == 0 && a[7].kind == DMVSI_ACT_END);
    }
    dmvsi_free(doc);
}

DMOD_TEST_STEP(dmvs_html_switches_the_screens_of_a_page)
{
    int status = -1;
    dmvsi_doc_t doc = convert(FIXTURE("dmodos.html"), "os-screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);
    static const char windows[6][16] = { "app-settings", "app-system", "app-store", "app-music", "app-home", "app-weather" };
    for (uint32_t i = 0; i < 6U; i++)
    {
        const dmvsi_node_t* w = group_named(root->first, windows[i]);
        DMOD_TEST_EXPECT_TRUE(w != NULL && w->bind[DMVSI_BIND_Y] != 0);
        DMOD_TEST_EXPECT_TRUE(w != NULL && var_initial(doc, w->bind[DMVSI_BIND_Y]) == DMVSI_PX(480));    /* top: 100% */
    }
    const dmvsi_node_t* home = group_named(root->first, "home-screen");
    DMOD_TEST_EXPECT_TRUE(home != NULL && home->bind[DMVSI_BIND_OPACITY] != 0);
    dmvsi_free(doc);
}

/* The n-th image node of the tree (in painting order) */
static const dmvsi_node_t* image_at(const dmvsi_node_t* n, uint32_t* index)
{
    for (; n != NULL; n = n->next)
    {
        if (n->kind == DMVSI_NODE_IMAGE && (*index)-- == 0)
            return n;
        if (n->kind == DMVSI_NODE_GROUP)
        {
            const dmvsi_node_t* f = image_at(n->first, index);
            if (f != NULL)
                return f;
        }
    }
    return NULL;
}

static const dmvsi_node_t* nth_image(dmvsi_doc_t doc, uint32_t index)
{
    return image_at(dmvsi_root(doc), &index);
}

static bool file_has(const char* path, const char* text)
{
    static char data[2048];
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return false;
    size_t n = Dmod_FileRead(data, 1, sizeof(data) - 1U, f);
    Dmod_FileClose(f);
    data[n] = '\0';
    size_t k = strlen(text);
    for (size_t i = 0; i + k <= n; i++)
        if (strncmp(data + i, text, k) == 0)
            return true;
    return false;
}

DMOD_TEST_STEP(dmvs_html_draws_svg_and_images)
{
    /* A PNG's signature and IHDR: 100 x 50 (dmvs_html reads only the size of an image) */
    static const uint8_t png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                     0, 0, 0, 100, 0, 0, 0, 50, 8, 6, 0, 0, 0, 0, 0, 0, 0 };
    void* f = Dmod_FileOpen(TEST_FILE("wide.png"), "wb");
    DMOD_TEST_EXPECT_TRUE(f != NULL);
    if (f == NULL)
        return;
    Dmod_FileWrite(png, 1, sizeof(png), f);
    Dmod_FileClose(f);

    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("images.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0 } #screen { width: 200px; height: 100px }"
        "svg { display: block; width: 40px; height: 40px }"
        "img { display: block; width: 40px; height: 40px; object-fit: cover }"
        "#bg { width: 40px; height: 40px; background: url('wide.png') right bottom / contain; filter: blur(4px) }"
        "</style></head><body><div id=\"screen\">"
        "<svg viewBox=\"0 0 10 10\"><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"red\"/></linearGradient>"
        "<circle cx=\"5\" cy=\"5\" r=\"4\" fill=\"currentColor\"/></svg>"
        "<img src=\"wide.png\"><div id=\"bg\"></div>"
        "</div></body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("images.html"), "screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;

    /* The <svg>: a file of its own, sized as its box, its viewBox and camelCase names kept */
    const dmvsi_node_t* svg = nth_image(doc, 0);
    DMOD_TEST_EXPECT_TRUE(svg != NULL && svg->u.image.rect.w == DMVSI_PX(40) && svg->u.image.rect.h == DMVSI_PX(40));
    if (svg != NULL)
    {
        size_t n = strlen(svg->u.image.path);
        DMOD_TEST_EXPECT_TRUE(n > 4 && strcmp(svg->u.image.path + n - 4, ".svg") == 0);
        DMOD_TEST_EXPECT_TRUE(file_has(svg->u.image.path, "width=\"40\" height=\"40\" viewBox=\"0 0 10 10\""));
        DMOD_TEST_EXPECT_TRUE(file_has(svg->u.image.path, "<linearGradient id=\"g\">"));
        DMOD_TEST_EXPECT_TRUE(file_has(svg->u.image.path, "fill=\"#000000\""));     /* currentColor */
    }

    /* object-fit: cover - 100 x 50 into 40 x 40: 80 x 40, in the middle */
    const dmvsi_node_t* img = nth_image(doc, 1);
    DMOD_TEST_EXPECT_TRUE(img != NULL && img->u.image.width == DMVSI_PX(80) && img->u.image.height == DMVSI_PX(40));
    DMOD_TEST_EXPECT_TRUE(img != NULL && img->u.image.flags == (DMVSI_IMAGE_CENTER | DMVSI_IMAGE_MIDDLE));

    /* background: url() right bottom / contain, blurred - 40 x 20 at the bottom right, blur 4 px */
    const dmvsi_node_t* bg = nth_image(doc, 2);
    DMOD_TEST_EXPECT_TRUE(bg != NULL && bg->u.image.width == DMVSI_PX(40) && bg->u.image.height == DMVSI_PX(20));
    DMOD_TEST_EXPECT_TRUE(bg != NULL && bg->u.image.flags == (DMVSI_IMAGE_RIGHT | DMVSI_IMAGE_BOTTOM));
    DMOD_TEST_EXPECT_TRUE(bg != NULL && bg->u.image.blur == DMVSI_PX(4));
    dmvsi_free(doc);
}

DMOD_TEST_STEP(dmvs_html_converts_listeners)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("listeners.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0 } #screen { width: 200px; height: 100px }"
        ".tab { width: 40px; height: 20px; background: #333 } .tab.active { background: #36f }"
        "</style></head><body><div id=\"screen\">"
        "<div class=\"tab active\" id=\"a\"></div><div class=\"tab\" id=\"b\"></div>"
        "</div><script>"
        "const tabs = document.querySelectorAll('.tab');\n"
        "tabs.forEach(tab => {\n"
        "  tab.addEventListener('click', () => {\n"
        "    tabs.forEach(t => t.classList.remove('active'));\n"
        "    tab.classList.add('active');\n"
        "  });\n"
        "});\n"
        "</script></body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("listeners.html"), "screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);

    /* forEach is unrolled: each tab is clicked with a handler of its own, and looks active by a variable */
    const dmvsi_node_t* a = group_named(root->first, "a");
    const dmvsi_node_t* b = group_named(root->first, "b");
    DMOD_TEST_EXPECT_TRUE(a != NULL && b != NULL && a->click != 0 && b->click != 0 && a->click != b->click);
    if (a != NULL && b != NULL)
    {
        const dmvsi_action_t* acts = NULL;
        uint32_t n = flat_actions(doc, b->click, &acts);
        bool sets = false;
        for (uint32_t i = 0; i < n; i++)
            sets = sets || acts[i].kind == DMVSI_ACT_SET;
        DMOD_TEST_EXPECT_TRUE(sets);
    }
    dmvsi_free(doc);
}

/* The first text node shown by a variable */
static const dmvsi_node_t* text_of_var(const dmvsi_node_t* n, dmvsi_var_t var)
{
    for (; n != NULL; n = n->next)
    {
        if (n->kind == DMVSI_NODE_TEXT && n->u.text.var == var && var != 0)
            return n;
        if (n->kind == DMVSI_NODE_GROUP)
        {
            const dmvsi_node_t* f = text_of_var(n->first, var);
            if (f != NULL)
                return f;
        }
    }
    return NULL;
}

/* A variable by its name */
static dmvsi_var_t var_named(dmvsi_doc_t doc, const char* name)
{
    dmvsi_var_info_t info;
    for (dmvsi_var_t v = 1; dmvsi_var_info(doc, v, &info) == 0; v++)
        if (strcmp(info.name, name) == 0)
            return v;
    return 0;
}

DMOD_TEST_STEP(dmvs_html_converts_texts_and_timers)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("texts.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0; font-family: sans-serif } #screen { width: 200px; height: 100px }"
        "#speed { width: 100px; text-align: center; font-size: 20px } #temp { width: 80px }"
        "</style></head><body><div id=\"screen\">"
        "<div id=\"speed\"></div><div id=\"temp\">21.5\xC2\xB0" "C</div>"
        "<div id=\"up\" style=\"width: 20px; height: 20px\"></div>"
        "</div><script>"
        "const speedText = document.getElementById('speed');\n"
        "let speed = 0;\n"
        "const interval = setInterval(() => {\n"
        "  speed += 2;\n"
        "  if (speed >= 68) { speed = 68; clearInterval(interval); }\n"
        "  speedText.innerText = speed;\n"
        "}, 35);\n"
        "const t = document.getElementById('temp');\n"
        "document.getElementById('up').addEventListener('click', () => {\n"
        "  t.innerText = (parseFloat(t.innerText) + 0.5).toFixed(1) + '\xC2\xB0" "C';\n"
        "});\n"
        "</script></body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("texts.html"), "screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);

    /* #speed: empty at first - a text of its variable, centred in its box (the scripts' characters: in its font) */
    dmvsi_var_t speed = var_named(doc, "speed_text");
    const dmvsi_node_t* st = text_of_var(root, speed);
    DMOD_TEST_EXPECT_TRUE(speed != 0 && st != NULL);
    if (st != NULL)
    {
        DMOD_TEST_EXPECT_TRUE(st->u.text.align == DMVSI_TEXT_CENTER && st->u.text.width == DMVSI_PX(100));
    }
    /* #temp: its text and its number (21.5) - the click sets both */
    dmvsi_var_t temp = var_named(doc, "temp_text");
    dmvsi_var_t number = var_named(doc, "temp_number");
    DMOD_TEST_EXPECT_TRUE(text_of_var(root, temp) != NULL && var_initial(doc, number) == 21500);
    dmvsi_var_info_t info;
    DMOD_TEST_EXPECT_TRUE(dmvsi_var_info(doc, temp, &info) == 0 && strcmp(info.text, "21.5\xC2\xB0" "C") == 0);
    const dmvsi_node_t* up = group_named(root->first, "up");
    DMOD_TEST_EXPECT_TRUE(up != NULL && up->click != 0);
    if (up != NULL)
    {
        const dmvsi_action_t* a = NULL;
        uint32_t n = flat_actions(doc, up->click, &a);
        bool sets_text = false, sets_number = false;
        for (uint32_t i = 0; i < n; i++)
        {
            sets_text = sets_text || (a[i].kind == DMVSI_ACT_SET && a[i].var == temp);
            sets_number = sets_number || (a[i].kind == DMVSI_ACT_SET && a[i].var == number);
        }
        DMOD_TEST_EXPECT_TRUE(sets_text && sets_number);
    }
    /* The interval: a timer of the view */
    uint16_t ms = 0;
    dmvsi_handler_t h = 0;
    DMOD_TEST_EXPECT_TRUE(dmvsi_timer_at(doc, 0, &ms, &h) && ms >= 10 && ms <= 50 && h != 0);
    dmvsi_free(doc);
}

static uint32_t count_texts(const dmvsi_node_t* n, const char* text)
{
    uint32_t count = 0;
    for (; n != NULL; n = n->next)
    {
        if (n->kind == DMVSI_NODE_TEXT && n->u.text.var == 0 && strcmp(n->u.text.text, text) == 0)
            count++;
        if (n->kind == DMVSI_NODE_GROUP)
            count += count_texts(n->first, text);
    }
    return count;
}

/* The groups clicked whose handler sets a variable */
static uint32_t clicks_setting(dmvsi_doc_t doc, const dmvsi_node_t* n, dmvsi_var_t var)
{
    uint32_t count = 0;
    for (; n != NULL; n = n->next)
    {
        if (n->kind != DMVSI_NODE_GROUP)
            continue;
        if (n->click != 0)
        {
            const dmvsi_action_t* a = NULL;
            uint32_t k = flat_actions(doc, n->click, &a);
            bool sets = false;
            for (uint32_t i = 0; i < k && !sets; i++)
                sets = a[i].kind == DMVSI_ACT_SET && a[i].var == var;
            count += sets ? 1U : 0U;
        }
        count += clicks_setting(doc, n->first, var);
    }
    return count;
}

DMOD_TEST_STEP(dmvs_html_converts_what_scripts_build)
{
    int status = -1;
    DMOD_TEST_EXPECT_TRUE(write_file(TEST_FILE("build.html"),
        "<!DOCTYPE html><html><head><style>"
        "body { margin: 0; font-family: sans-serif } #screen { width: 200px; height: 120px }"
        ".row { height: 20px } .on { background: #36f } #title { height: 20px }"
        ".view { visibility: hidden; opacity: 0; transition: opacity 0.3s } .view.active { visibility: visible; opacity: 1 }"
        "</style></head><body><div id=\"screen\"><div id=\"title\" onclick=\"document.getElementById('v').classList.add('active')\">-</div>"
        "<div class=\"view\" id=\"v\"><div id=\"list\"><!-- the script's --></div></div></div><script>"
        "const songs = [{ t: 'One', s: 65 }, { t: 'Two', s: 130 }, { t: 'Three', s: 7 }];\n"
        "let current = 1;\n"
        "const list = document.getElementById('list');\n"
        "function time(s) { return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; }\n"
        "function render() {\n"
        "  list.innerHTML = '';\n"
        "  songs.forEach((song, i) => {\n"
        "    const div = document.createElement('div');\n"
        "    div.className = `row ${i === current ? 'on' : ''}`;\n"
        "    div.innerHTML = `<span>${song.t}</span> <b>${time(song.s)}</b>`;\n"
        "    div.onclick = () => { current = i; document.getElementById('title').innerText = song.t; render(); };\n"
        "    list.appendChild(div);\n"
        "  });\n"
        "}\n"
        "render();\n"
        "</script></body></html>"));
    dmvsi_doc_t doc = convert(TEST_FILE("build.html"), "screen", 0, 0, &status);
    DMOD_TEST_EXPECT_EQ(status, 0);
    if (doc == NULL)
        return;
    const dmvsi_node_t* root = dmvsi_root(doc);

    /* The rows as the page loads: their texts, the current one's background, each clicked */
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_TEXT, "One", NULL) != NULL);
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_TEXT, "2:10", NULL) != NULL);
    DMOD_TEST_EXPECT_TRUE(find(root->first, DMVSI_NODE_TEXT, "0:07", NULL) != NULL);
    dmvsi_rect_t on = { 0, DMVSI_PX(40), DMVSI_PX(200), DMVSI_PX(20) };
    const dmvsi_node_t* fill = find(root->first, DMVSI_NODE_RECT, NULL, &on);
    DMOD_TEST_EXPECT_TRUE(fill != NULL && fill->u.fill.paint.color == 0xFF3366FFu);
    dmvsi_var_t title = var_named(doc, "title_text");
    uint32_t clicked = clicks_setting(doc, root->first, title);
    DMOD_TEST_EXPECT_EQ(clicked, 3u);
    /* Rendered anew on a click: each row's class list one of two - its look's variable set */
    dmvsi_var_t look = var_named(doc, "div_look");
    DMOD_TEST_EXPECT_TRUE(look != 0 && clicks_setting(doc, root->first, look) == 3u);
    /* The list is in a screen hidden as the page loads: a row's other look is seen when it is shown too */
    DMOD_TEST_EXPECT_TRUE(count_texts(root->first, "Two") >= 2u);
    dmvsi_free(doc);
}
