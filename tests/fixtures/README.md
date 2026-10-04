# Test fixtures

## dmodos/

A UI design for a 272 x 480 screen, made in HTML with Tailwind CSS (its
Play CDN), Font Awesome and Inter from Google Fonts - the page as it is,
`dmodos.html`. The test maps its CDN resources to the files here, as
`todmvs -m URL=FILE` does:

| URL | File |
|-----|------|
| `https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css` | `fontawesome.css` - Font Awesome Free 6.4.0's CSS (MIT) |
| `.../font-awesome/6.4.0/webfonts/fa-solid-900.ttf`, `fa-brands-400.ttf` | `fonts/icons-solid.ttf`, `fonts/icons-brands.ttf` - its fonts cut down to the icons of the page, renamed as the SIL OFL asks of a modified font (`fonts/LICENSE-FontAwesome.txt`) |
| `https://fonts.googleapis.com/css2` | `inter.css` - `@font-face` of `fonts/Inter-*.otf`, Inter cut down to the characters of the page (SIL OFL, `fonts/LICENSE-Inter.txt`) |

The expected boxes in the test are the ones Chrome lays the page out in.
