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
