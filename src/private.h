#ifndef DMVS_HTML_PRIVATE_H
#define DMVS_HTML_PRIVATE_H

#include "dmvsi.h"

/*
 * dmvs_html - HTML and CSS into dmvsi documents. A page goes through:
 *
 *   html.c      the text -> a tree of nodes (elements, text)
 *   css.c       style sheets -> rules (selectors, declarations), @font-face
 *   tailwind.c  Tailwind CSS classes -> declarations, as its Play CDN makes them
 *   style.c     the cascade: every element's computed style
 *   layout.c    boxes: block, inline (lines of text), flex, grid, positioned
 *   paint.c     the boxes in painting order -> the document's groups and shapes
 *
 * Every length is in 1/64 pixel (DMVSI_UNIT), as a browser's layout units.
 * Everything a conversion allocates is in its arena, freed at once.
 */

#define U                   DMVSI_UNIT              /* 1 px */
#define AUTO_SIZE           INT32_MIN               /* No size given */

/* ---- Arena ---- */

typedef struct chunk chunk_t;
struct chunk
{
    chunk_t*    next;
    size_t      used;
    size_t      size;
};

typedef struct
{
    chunk_t*    chunks;
    bool        failed;
} arena_t;

void*   arena_alloc(arena_t* a, size_t size);      /* Zeroed */
char*   arena_strndup(arena_t* a, const char* s, size_t length);
void    arena_free(arena_t* a);

/* ---- Values ---- */

/* A length: px + pct % of what it is relative to, or auto / none */
#define LEN_SET             0u
#define LEN_AUTO            1u
#define LEN_NONE            2u                      /* max-width: none */
#define LEN_CONTENT         3u                      /* min-content, max-content, fit-content: as auto */

typedef struct
{
    int32_t     px;                                 /* 1/64 px */
    int32_t     pct;                                /* 1/100 % */
    uint8_t     kind;
} len_t;

static inline len_t len_px(int32_t px) { len_t l = { px, 0, LEN_SET }; return l; }
static inline len_t len_auto(void) { len_t l = { 0, 0, LEN_AUTO }; return l; }
static inline bool len_is_auto(len_t l) { return l.kind != LEN_SET; }
int32_t len_resolve(len_t l, int32_t base);         /* auto: 0 */

/* ---- Nodes ---- */

#define NODE_DOCUMENT       0u
#define NODE_ELEMENT        1u
#define NODE_TEXT           2u

#define PSEUDO_NONE         0u
#define PSEUDO_BEFORE       1u
#define PSEUDO_AFTER        2u
#define PSEUDO_ANONYMOUS    3u                      /* A block around text in a flex or grid container */

typedef struct attr attr_t;
struct attr
{
    attr_t*     next;
    char*       name;
    char*       value;
};

typedef struct style style_t;
typedef struct frag frag_t;
typedef struct node node_t;

/* Where layout put a box */
typedef struct
{
    int32_t     m[4];                               /* Margins: top, right, bottom, left */
    int32_t     b[4];                               /* Borders */
    int32_t     p[4];                               /* Paddings */
    int32_t     x, y;                               /* Border box, relative to `container`'s */
    int32_t     w, h;                               /* Border box */
    int32_t     ax, ay;                             /* Border box, absolute (on the page) */
    int32_t     baseline;                           /* First line's, from the border box top; AUTO_SIZE: none */
    int32_t     content_w, content_h;               /* What is inside (scrolling) */
    node_t*     container;                          /* The box `x, y` are relative to */
    frag_t*     frags;                              /* Lines of an inline formatting context it holds */
    frag_t*     last_frag;
    int32_t     min_content, max_content;           /* Intrinsic widths (border box), AUTO_SIZE: not yet */
    bool        laid_out;
    bool        out_of_flow;                        /* Absolutely positioned */
    bool        placed;                             /* Its absolute position is known */
} box_t;

/* One look of an element: the element as laid out in a state of the page, shown on its conditions */
#define MAX_VARIANTS        4u

typedef struct
{
    node_t*         node;                           /* The element in that state's tree; NULL: not shown then */
    dmvsi_var_t     var;                            /* Shown while var == value (0: always) ... */
    int32_t         value;
    int8_t          pressed;                        /* ... and its box is pressed (1), is not (0); -1: either */
} variant_t;

/* What a script does with an element (script.c): the variables its group is bound to, its click, its looks */
typedef struct
{
    dmvsi_var_t     bind[DMVSI_BIND_COUNT];
    dmvsi_handler_t click;
    variant_t       variants[MAX_VARIANTS];         /* None: it looks as it is */
    uint8_t         variant_count;
} dynamic_t;

struct node
{
    uint8_t     kind;
    uint32_t    index;                              /* Of the elements, in document order (as parsed) */
    dynamic_t*  dynamic;                            /* NULL: what it is it stays */
    bool        active;                             /* Pressed: :active holds (MOD_ACTIVE) */
    uint8_t     pseudo;
    char*       tag;                                /* Lowercase; NULL for text */
    attr_t*     attrs;
    char*       text;                               /* Text: entities decoded */
    size_t      length;
    const char* id;
    char**      classes;
    uint32_t    class_count;
    node_t*     parent;
    node_t*     first;
    node_t*     last;
    node_t*     next;
    node_t*     prev;
    style_t*    style;                              /* Elements (and text: its parent's) */
    box_t       box;
};

/* ---- Style sheets ---- */

#define COMB_NONE           0u
#define COMB_DESCENDANT     1u
#define COMB_CHILD          2u
#define COMB_NEXT           3u                      /* + */
#define COMB_SIBLING        4u                      /* ~ */

#define ATTR_EXISTS         0u
#define ATTR_EQUALS         1u
#define ATTR_INCLUDES       2u                      /* ~= */
#define ATTR_DASH           3u                      /* |= */
#define ATTR_PREFIX         4u                      /* ^= */
#define ATTR_SUFFIX         5u                      /* $= */
#define ATTR_CONTAINS       6u                      /* *= */

#define PC_FIRST_CHILD      0x0001u
#define PC_LAST_CHILD       0x0002u
#define PC_ONLY_CHILD       0x0004u
#define PC_ROOT             0x0008u
#define PC_EMPTY            0x0010u
#define PC_ACTIVE           0x0020u                 /* :active - a pressed element (MOD_ACTIVE) */
#define PC_NEVER            0x8000u                 /* :hover, :active, ... - never in a still page */

typedef struct compound compound_t;

typedef struct
{
    char*       name;
    char*       value;
    uint8_t     op;
    bool        icase;
} attr_sel_t;

struct compound
{
    char*           tag;                            /* NULL: any */
    char*           id;
    char**          classes;
    uint8_t         class_count;
    attr_sel_t*     attrs;
    uint8_t         attr_count;
    uint16_t        pseudo;                         /* PC_* */
    compound_t*     nots;                           /* :not(...) - each must not match */
    uint8_t         not_count;
    compound_t*     ises;                           /* :is(...), :where(...) - one must match */
    uint8_t         is_count;
    uint8_t         comb;                           /* How it relates to the compound before it */
};

typedef struct
{
    const char*     name;                           /* Lowercase */
    const char*     value;
    bool            important;
} decl_t;

typedef struct
{
    compound_t*     parts;                          /* Left to right */
    uint8_t         count;
    uint8_t         pseudo_element;                 /* PSEUDO_BEFORE, _AFTER, or none */
    uint32_t        specificity;                    /* ids << 16 | classes << 8 | types */
    decl_t*         decls;
    uint16_t        decl_count;
    uint32_t        order;                          /* Of the cascade: later wins */
    uint8_t         layer;                          /* LAYER_*: the user agent's rules lose to the author's */
    const char*     base;                           /* URLs in its values are relative to it */
} rule_t;

typedef struct font_face font_face_t;
struct font_face
{
    font_face_t*    next;
    char*           family;                         /* Lowercase */
    uint16_t        weight_min;
    uint16_t        weight_max;
    bool            italic;
    char*           path;                           /* The TrueType / OpenType file */
};

/* ---- Computed styles ---- */

#define DISPLAY_NONE        0u
#define DISPLAY_INLINE      1u
#define DISPLAY_BLOCK       2u
#define DISPLAY_INLINE_BLOCK 3u
#define DISPLAY_FLEX        4u
#define DISPLAY_INLINE_FLEX 5u
#define DISPLAY_GRID        6u
#define DISPLAY_INLINE_GRID 7u
#define DISPLAY_CONTENTS    8u

#define POSITION_STATIC     0u
#define POSITION_RELATIVE   1u
#define POSITION_ABSOLUTE   2u
#define POSITION_FIXED      3u

#define OVERFLOW_VISIBLE    0u
#define OVERFLOW_HIDDEN     1u
#define OVERFLOW_SCROLL     2u
#define OVERFLOW_AUTO       3u

#define FLEX_ROW            0u
#define FLEX_ROW_REVERSE    1u
#define FLEX_COLUMN         2u
#define FLEX_COLUMN_REVERSE 3u

#define ALIGN_AUTO          0u
#define ALIGN_NORMAL        1u
#define ALIGN_STRETCH       2u
#define ALIGN_START         3u
#define ALIGN_END           4u
#define ALIGN_CENTER        5u
#define ALIGN_BASELINE      6u
#define ALIGN_BETWEEN       7u
#define ALIGN_AROUND        8u
#define ALIGN_EVENLY        9u

#define TEXT_LEFT           0u
#define TEXT_RIGHT          1u
#define TEXT_CENTER         2u
#define TEXT_JUSTIFY        3u

#define CASE_NONE           0u
#define CASE_UPPER          1u
#define CASE_LOWER          2u
#define CASE_CAPITALIZE     3u

#define WS_NORMAL           0u
#define WS_NOWRAP           1u
#define WS_PRE              2u
#define WS_PRE_WRAP         3u
#define WS_PRE_LINE         4u

#define VALIGN_BASELINE     0u
#define VALIGN_MIDDLE       1u
#define VALIGN_TOP          2u
#define VALIGN_BOTTOM       3u
#define VALIGN_TEXT_TOP     4u
#define VALIGN_TEXT_BOTTOM  5u
#define VALIGN_LENGTH       6u                      /* Raised by `valign_by` */

#define LH_NORMAL           0u
#define LH_NUMBER           1u                      /* x 1/1000 of the font size */
#define LH_LENGTH           2u

#define TRACK_LENGTH        0u
#define TRACK_FR            1u
#define TRACK_AUTO          2u

#define MAX_SHADOWS         4u
#define MAX_TRACKS          12u
#define MAX_TRANSITIONS     12u

/* What a transition animates (of what dmview can: a box's position and opacity) */
#define TRANSITION_NONE     0u
#define TRANSITION_ALL      1u
#define TRANSITION_POSITION 2u                      /* top, left, right, bottom, inset, margin, transform, translate */
#define TRANSITION_OPACITY  3u
#define TRANSITION_OTHER    4u

typedef struct
{
    int32_t     x, y, blur, spread;                 /* 1/64 px */
    uint32_t    color;
    bool        inset;
} shadow_t;

typedef struct
{
    uint8_t     kind;
    len_t       length;
    int32_t     fr;                                 /* 1/100 */
} track_t;

/* A gradient as CSS gives it: placed on the box when it is painted */
#define CSS_LINEAR          1u
#define CSS_RADIAL          2u
#define TO_TOP              0x1u
#define TO_RIGHT            0x2u
#define TO_BOTTOM           0x4u
#define TO_LEFT             0x8u

typedef struct
{
    uint8_t     kind;
    uint8_t     to;                                 /* Linear: TO_* of "to <side or corner>", 0: `angle` */
    int16_t     angle;                              /* Degrees */
    bool        circle;                             /* Radial: a circle, else an ellipse */
    uint8_t     extent;                             /* Radial: 0 farthest-corner, 1 closest-side, 2 farthest-side, 3 closest-corner */
    len_t       cx, cy;                             /* Radial: center (default 50 %) */
    uint8_t     count;
    uint32_t    colors[DMVSI_MAX_STOPS];
    int32_t     positions[DMVSI_MAX_STOPS];         /* 1/100 %, -1: not given */
} gradient_t;

typedef struct var var_t;
struct var
{
    var_t*      next;
    const char* name;
    const char* value;
};

struct style
{
    uint8_t     display;
    uint8_t     position;
    uint8_t     overflow_x, overflow_y;
    bool        border_box;                         /* box-sizing */
    bool        hidden;                             /* visibility */
    len_t       inset[4];                           /* top, right, bottom, left */
    int32_t     z_index;
    bool        z_auto;
    len_t       width, height, min_w, min_h, max_w, max_h;
    len_t       margin[4];
    len_t       padding[4];
    int32_t     border_w[4];
    uint32_t    border_color[4];
    len_t       radius[4];                          /* Corners: top-left, top-right, bottom-right, bottom-left */

    uint8_t     flex_direction;
    bool        flex_wrap;
    uint8_t     justify_content, align_items, align_self, align_content, justify_items, justify_self;
    int32_t     grow, shrink;                       /* 1/100 */
    len_t       basis;
    int32_t     order;
    len_t       row_gap, column_gap;
    track_t     columns[MAX_TRACKS];
    uint8_t     column_count;
    uint8_t     column_span, row_span;

    uint8_t     opacity;
    uint32_t    color;
    uint32_t    background;
    gradient_t* background_image;

    const char* font_family;
    int32_t     font_size;
    uint16_t    font_weight;
    bool        italic;
    uint8_t     line_height_kind;
    int32_t     line_height;
    int32_t     letter_spacing;
    uint8_t     text_align, text_transform, white_space, valign;
    int32_t     valign_by;

    uint8_t     transition_count;                   /* transition-property, -duration, -timing-function */
    uint8_t     transition_props[MAX_TRANSITIONS];  /* TRANSITION_* */
    uint8_t     duration_count;
    uint16_t    transition_ms[MAX_TRANSITIONS];
    uint8_t     timing_count;
    int16_t     transition_easing[MAX_TRANSITIONS][4];

    shadow_t    shadows[MAX_SHADOWS];               /* box-shadow, the first on top */
    uint8_t     shadow_count;
    shadow_t    drops[MAX_SHADOWS];                 /* filter: drop-shadow() */
    uint8_t     drop_count;
    int32_t     blur;                               /* filter: blur(), its standard deviation */
    len_t       translate_x, translate_y;

    const char* content;                            /* ::before, ::after */
    size_t      content_length;
    bool        has_content;
    var_t*      vars;                               /* Custom properties, inherited */

    dmvsi_font_t font;                              /* Layout's: the font it chose (style_font()) */
    bool        font_chosen;
};

/* ---- Lines ---- */

#define FRAG_TEXT           0u                      /* A run of text: its pen starts at x, on the baseline y */
#define FRAG_BOX            1u                      /* An inline element's background and border on one line */

struct frag
{
    frag_t*         next;
    uint8_t         kind;
    const node_t*   node;                           /* The text, or the inline element */
    const style_t*  style;
    dmvsi_font_t    font;
    int32_t         x, y;                           /* Relative to its block's border box */
    int32_t         w, h;
    char*           text;
    size_t          length;
};

/* ---- A conversion ---- */

#define RULE_BUCKETS        512u

typedef struct rule_ref rule_ref_t;
struct rule_ref
{
    rule_ref_t*     next;
    rule_t*         rule;
};

/* What a script changes of an element, for laying the page out as it is then */
#define MOD_CLASS_ADD       0u
#define MOD_CLASS_REMOVE    1u
#define MOD_STYLE           2u
#define MOD_ACTIVE          3u                      /* Pressed: it and its ancestors are :active */

typedef struct
{
    uint32_t        element;                        /* Its index */
    uint8_t         kind;
    const char*     name;                           /* A class, a property */
    const char*     value;
} mod_t;

typedef struct
{
    arena_t                 arena;
    const char*             path;                   /* The page */
    const mod_t*            mods;                   /* Applied after parsing (run_layout()) */
    uint32_t                mod_count;
    const dmvsi_options_t*  options;
    dmvsi_doc_t             doc;
    node_t*                 document;
    rule_ref_t*             buckets[RULE_BUCKETS];  /* Rules by their last compound's id, class or tag */
    rule_ref_t*             universal;              /* ... or every element */
    uint32_t                rule_count;
    uint32_t                order;
    font_face_t*            faces;
    bool                    tailwind;               /* The page loads Tailwind CSS (its Play CDN) */
    bool                    has_active;             /* A rule of :active (a pressed look) */
    int32_t                 vw, vh;                 /* The viewport, 1/64 px */
    int                     status;
    uint32_t                warnings;
    void*                   style_work;             /* style.c's */
    const char*             font_warned;            /* The font file last reported as missing */
    void*                   states;                 /* script.c's: the page laid out in other states */
} conv_t;

/* Resources: a URL or a path relative to `base` made absolute (resource_url), and
 * that through the options' maps to a file (resource_path) - NULL when it is a
 * URL nothing maps; resolve_resource does both */
char*   resource_url(conv_t* c, const char* base, const char* url, size_t length);
char*   resource_path(conv_t* c, const char* url);
char*   resolve_resource(conv_t* c, const char* base, const char* url, size_t length);
char*   read_resource(conv_t* c, const char* path, size_t* size);

/* html.c */
node_t* html_parse(conv_t* c, const char* text, size_t length);
const char* node_attr(const node_t* n, const char* name);
bool    node_is(const node_t* n, const char* tag);

/* css.c */
void    css_parse(conv_t* c, const char* text, size_t length, const char* base, uint32_t layer);
bool    css_matches(const rule_t* rule, const node_t* n);
uint32_t css_unescape(const char** p, const char* end);
void    css_add_rule(conv_t* c, rule_t* rule);
uint16_t css_parse_decls(conv_t* c, const char* text, size_t length, decl_t** decls);
const rule_ref_t* css_bucket(const conv_t* c, const char* prefix, const char* name);

#define LAYER_UA            0u
#define LAYER_AUTHOR        1u
#define LAYER_TAILWIND      2u

/* tailwind.c */
extern const char tailwind_preflight[];
uint32_t tailwind_class(conv_t* c, const char* name, decl_t* decls, uint32_t max, uint32_t* rank, bool* active);

/* style.c */
void    style_compute(conv_t* c, node_t* root);

/* layout.c */
void    layout_page(conv_t* c, node_t* root);
dmvsi_font_t style_font(conv_t* c, style_t* st);

/* dmvs_html.c */
bool    image_size(const char* path, int32_t* width, int32_t* height);

/* paint.c */
int     paint_page(conv_t* c, node_t* root);
node_t* find_id(node_t* n, const char* id, uint32_t depth);
dmvsi_rect_t group_rect(const conv_t* c, const node_t* n, int32_t ox, int32_t oy);
void    view_origin(const conv_t* c, int32_t* ox, int32_t* oy);

/* script.c */
int     script_compile(conv_t* c);
void    script_free(conv_t* c);                     /* The states it laid out (after painting) */

/* style.c: the transition of a property (TRANSITION_POSITION, _OPACITY) - false when none */
bool    style_transition(const style_t* st, uint8_t what, uint16_t* ms, int16_t* easing);

/* dmvs_html.c: a page laid out as it is, or with what a script changed (state.c) */
int     run_layout(conv_t* c);

#define WARN(c, ...)    do { (c)->warnings++; DMOD_LOG_WARN("dmvs_html: " __VA_ARGS__); } while (0)

#endif /* DMVS_HTML_PRIVATE_H */
