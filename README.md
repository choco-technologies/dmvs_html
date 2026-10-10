# dmvs_html

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmvs_html/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmvs_html/actions/workflows/ci.yml)

HTML and CSS for [dmvsi](https://github.com/choco-technologies/dmvsi): a
converter plugin that lays a page out as a browser does and describes it as
a dmvsi document - [todmvs](https://github.com/choco-technologies/todmvs)
makes a dmview view of it.

It has its own HTML parser, CSS cascade and layout - no browser, no
dependencies - so it runs on a PC at build time and on a device. It is made
for **UI pages**: the screens a designer (or an AI) draws in HTML with
Tailwind CSS - and their JavaScript, which
[dmvs_js](https://github.com/choco-technologies/dmvs_js) compiles into the
view's variables, handlers and timers.

## What it does

**HTML** - elements, attributes, text with character references, the end
tags a page may leave out, `<style>`, `<link rel="stylesheet">`, `style=""`,
`<img>`, inline `<svg>`, `<br>`. `<script>` is not run; one that loads
Tailwind's Play CDN (`cdn.tailwindcss.com`) turns Tailwind on, and its
`tailwind.config` (`theme.extend.fontFamily`) is read.

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

**Images** - `<img>` with `object-fit` (`cover`, `contain`, `fill` - its
aspect kept -, `none`, `scale-down`) and `object-position`;
`background-image: url()` (under its gradient layers) with
`background-size` (`cover`, `contain`, `auto`) and `background-position`;
`filter: blur()` blurs the image. Each size and blur an image is drawn at
is converted once (todmvs: a section of the image's `.ini`). An inline
`<svg>` is an image of its own: its subtree is written into an SVG file
of its box's size - `$TMPDIR` (`/tmp`), `<page>-svg<index>.svg`, the
`viewBox` and SVG's camelCase names kept, `currentColor` as its color -
which dmod's dmimg_svg rasterizes.

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

## Scripts

A script isn't run on the device. The page's `<script>`s and `onclick`
code are compiled by [dmvs_js](https://github.com/choco-technologies/dmvs_js)
into the view's code:

- what the scripts do when they load becomes the init handler;
- listeners become click handlers;
- `setTimeout` / `setInterval` become the view's timers;
- variables become the view's variables.

Anything known at conversion is computed then. See dmvs_js's
`docs/compiler.md` for the language it compiles. dmvs_html is its host, the
DOM:

```js
const home = document.getElementById('home');   // an element (querySelector / querySelectorAll too)
let current = null;                              // a variable holding elements
function open(id) {                              // onclick="open('settings')"
    const w = document.getElementById(id);
    w.classList.add('active');                   // classList.add / remove / toggle / replace / contains
    home.style.opacity = '0.3';                  // style.<property> = a text known at conversion
    current = w;
}
const speed = document.getElementById('speed');
let kmh = 0;
const timer = setInterval(() => {                // a timer of the view
    kmh += 2;
    if (kmh >= 68) clearInterval(timer);
    speed.innerText = kmh + ' km/h';             // a text variable
}, 35);
up.addEventListener('click', () =>               // parseFloat(innerText): the number it shows
    temp.innerText = (parseFloat(temp.innerText) + 0.5).toFixed(1) + '°C');
```

- **Changes are laid out.** A change of a class or a style gives the page
  with that change. How the element **moves and fades** becomes its
  group's variables, set at once or **animated by its CSS `transition`**.
  When it **looks different** it is painted in each state, shown on the
  class's variable. Classes an element changes together are one look.
  `:active` is its **pressed look**. The code calls a handler of each
  change, made once every change is known.
- **Element variables.** A variable holding elements
  (`current.classList.remove(...)`) changes whichever element it holds, an
  `IF` for each element it may hold.
- **Texts.** A text a script sets (`innerText`, `textContent`) is a text
  variable of 64 bytes. It is drawn on its element's first line, placed by
  `text-align`, with every character the scripts contain. Its number
  shadows it, so `parseFloat(el.innerText)` works.
- **Clicks.** `addEventListener('click', ...)`, `el.onclick = ...` and
  `onclick="..."` are a click handler. `this` is the element, and
  `el.click()` runs its handlers.

What's reported and left out:
- elements made or replaced (`createElement`, `innerHTML`);
- `src`, `setAttribute`;
- a style known only at runtime;
- a style that changes how an element looks (other than its opacity);
- events other than clicks;
- `Date`.

A class changes only the look of its element; what it moves around it
stays.

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

- JavaScript beyond the above (see [Scripts](#scripts)).
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
