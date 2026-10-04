# dmvs_html

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmvs_html/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmvs_html/actions/workflows/ci.yml)

HTML and CSS for [dmvsi](https://github.com/choco-technologies/dmvsi): a
converter plugin that lays a page out as a browser does and describes it as
a dmvsi document - [todmvs](https://github.com/choco-technologies/todmvs)
makes a dmview view of it.

It has its own HTML parser, CSS cascade and layout - no browser, no
dependencies - so it runs on a PC at build time and on a device. It is made
for **still UI pages**: the screens a designer (or an AI) draws in HTML with
Tailwind CSS. Scripts are not run: a page is converted as it is when it
loads.

## What it does

**HTML** - elements, attributes, text with character references, the end
tags a page may leave out, `<style>`, `<link rel="stylesheet">`, `style=""`,
`<img>`, `<br>`. `<script>` is not run; one that loads Tailwind's Play CDN
(`cdn.tailwindcss.com`) turns Tailwind on.

**CSS** - the cascade (origins, `!important`, specificity, order),
inheritance, custom properties and `var()`, `calc()` / `min()` / `max()` /
`clamp()`; selectors with every combinator, attributes, `:first-child`,
`:last-child`, `:only-child`, `:root`, `:empty`, `:not()`, `:is()`,
`:where()`, `::before` / `::after` with `content`; `@media` (against the
viewport), `@supports`, `@layer`, `@import`, `@font-face` (TrueType /
OpenType sources). Interaction (`:hover`, `:active`, `:focus`, ...) never
matches a still page.

**Layout** - block flow (with sibling margins collapsing, `margin: auto`),
inline text broken into lines (fonts, `line-height`, `letter-spacing`,
`text-align`, `text-transform`, `white-space`, `vertical-align`, inline
blocks), flexbox (direction, grow, shrink, basis, `gap`, `justify-content`,
`align-items` / `-self`, auto margins, `order`), grid (tracks of lengths,
`fr`, `auto`, `repeat()`, spans, `gap`), `position` relative / absolute /
fixed, `z-index` and stacking contexts, `overflow` (clipping, scrolling),
`translate()`.

**Painting** - colors (`#rgb`, `rgb()` / `rgba()`, `hsl()`, names),
`linear-gradient()` and `radial-gradient()` (premultiplied as in CSS),
`border` and `border-radius`, `opacity`, `box-shadow` (outer and inset),
`filter: blur()` (e.g. a glow) and `drop-shadow()`, `visibility`.

**Tailwind CSS** (v3, as its Play CDN generates it) - its preflight, the
utilities of layout, flexbox, grid, spacing, sizing, typography,
backgrounds and gradients, borders, effects, filters and transforms, the
palette, arbitrary values (`w-[454px]`, `text-[11px]`, `[color:red]`), the
opacity modifier (`bg-white/10`), `sm:` ... `2xl:` against the viewport,
`!important`, negative values. Their order decides between two utilities as
in Tailwind's style sheet.

**Fonts** - the faces of `@font-face` matched by family, weight and style as
CSS does; text is measured with them as the view will draw it. Icon fonts
(Font Awesome) work through their CSS: `::before { content: "\f1eb" }`.

## Resources

Style sheets, fonts and images are files next to the page; a URL is read
only where the options' maps say (`todmvs -m URL=FILE`). A style sheet
mapped from a URL resolves its relative URLs against the URL first (so a
whole CDN tree can be one directory map) and then next to its file.

```bash
todmvs ui.html -r screen \
    -m https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/=fontawesome/ \
    -m https://fonts.googleapis.com/css2=inter.css
```

The page is laid out in a viewport of the options' size (default 480 x 272);
the view is the viewport, or the element the options name (`-r`).

## What it does not do (yet)

- JavaScript - a page is still; what a script would show (an opened window)
  is converted when the HTML says it (e.g. a class added).
- Floats, tables (laid out as blocks), multi-line flex (`flex-wrap`),
  explicit grid placement (only spans), `position: sticky` (relative).
- Rounded clipping: `overflow: hidden` with `border-radius` clips to the
  rectangle; transforms other than `translate()`; `backdrop-filter`,
  filters other than `blur()` and `drop-shadow()`.
- Kerning (as dmview); text is a little narrower or wider than a browser's.

`DMVS_HTML_DUMP=<file>` writes every element's box (JSON) - to compare the
layout with a browser's.

## Building

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout.

## Testing

The tests convert a real page (tests/fixtures/dmodos - Tailwind CSS, Font
Awesome, Inter) and compare its boxes with the ones Chrome lays it out in,
and small pages of single features:

```bash
cd build
ctest --output-on-failure
```

## License

MIT - see [LICENSE](LICENSE). The test fixtures: see
[tests/fixtures/README.md](tests/fixtures/README.md).
