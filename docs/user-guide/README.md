# User guide (printable tri-fold)

`DEVELOPER-USERGUIDE.md` is the source of truth for both GitHub and the
printed handout. `render_userguide.py` converts it to styled HTML and prints
`DEVELOPER-USERGUIDE.pdf` via headless Chrome — a 2-page Letter-landscape
sheet meant for duplex printing and a standard letter fold (three panels
per side, six total).

## Files

| File | Role |
| --- | --- |
| `DEVELOPER-USERGUIDE.md` | Source content — edit this, never the PDF |
| `render_userguide.py` | Markdown -> HTML -> PDF pipeline + all CSS |
| `DEVELOPER-USERGUIDE.pdf` | Generated output (2 pages expected) |
| `cyd-horizon-guide-logo.svg` | Front-cover logo source — derived from the 2.8" packaging cover (middle band only: title, slogan, divider, radar/emblem, bullets; no top/bottom bars or separators) |
| `cyd-horizon-guide-logo.png` | Generated raster used by the guide |
| `render_dash_mock.py` | Regenerates the simulated screenshots |
| `mock_data.json` | Cached data for the mock screens (`--refresh` re-pulls) |
| `fetch_backgrounds.py` | One-time generator for `bg-*.png` page art |
| `bg-blur.png` / `bg-leaf.png` / `bg-lines.png` | Saved page backgrounds |

## Regenerating

```sh
cyd-horizon/.venv/bin/python docs/user-guide/render_userguide.py
```

Run from the repo root. Requires the `markdown` package and Google Chrome
at `/Applications/Google Chrome.app/...`. It refuses to render if
`.release-please-manifest.json` is stale vs `origin/main`, and warns if the
PDF is not exactly 2 pages.

It also re-renders the dashboard screenshots first, **from the cached
`mock_data.json`. That is deliberate**: rendering from the cache keeps the
images byte-identical between PDF versions, so a docs change can't churn
them. Refreshing the mock data is a separate, explicit step:

```sh
cyd-horizon/.venv/bin/python docs/user-guide/render_dash_mock.py --refresh
```

`--refresh` belongs to `render_dash_mock.py` alone — `render_userguide.py`
does not forward it, and always renders from the cache. Run the mock
directly when you actually want new live data, and expect every screenshot
to change when you do.

After editing the cover logo SVG, re-render its PNG before rebuilding:

```sh
cyd-horizon/.venv/bin/python -c "import cairosvg; \
  cairosvg.svg2png(url='docs/user-guide/cyd-horizon-guide-logo.svg', \
  write_to='docs/user-guide/cyd-horizon-guide-logo.png', \
  output_width=2700, output_height=1220)"
```

## Markdown structure

- `<!-- SIDE:OUTSIDE -->` / `<!-- SIDE:INSIDE -->` split the two printed
  sides; `<!-- PANEL -->` splits each side's three panels, listed in printed
  left-to-right order. OUTSIDE = `[inside flap | back cover | front cover]`
  (front cover is last).
- `{{VERSION}}` on the cover is replaced with the firmware version (next
  patch of the manifest, e.g. `Firmware v3.5.1+`).
- Images use relative paths resolved against this folder (`.png`, `.svg`,
  `.jpg`).
- All `##` section headings use title case (major words capitalized —
  e.g. `Settings at a Glance`, `LED & Border Colors`).
- `.callout` — light-blue note card (navy text, `#7da8c8` border) for
  hardware notes that need emphasis, e.g. the external-speaker paragraph.
  Lighter than the dark `h2` bars so it can't be mistaken for a heading.
  `.warn` remains the red caution variant. Nested `<div>`s do NOT process
  markdown — write inner markup as explicit HTML (`<p>`, `<ul>`, `<li>`).

## Front-cover conventions (for agents)

- The logo is rendered at `width: 88%` to match the `.hero` screenshots —
  keep logo and screenshots the same width for symmetry.
- The logo SVG is a vertically-trimmed `cyd-2.8-cover-4.5x2.25.svg`
  (1350x610): dark banner, borders, radar+emblem, and the five feature
  bullets are all part of the artwork — the feature bullets live in the
  logo, not in the markdown. If the cover artwork changes, re-derive the
  guide logo the same way (drop the top bar, bottom bar, and both
  separators; re-wrap the borders around the middle band).
- The `.desc` card under the logo uses the same light-blue card style as
  `.callout` (`box-sizing: border-box` keeps it at 88% like the shots);
  text is left-aligned, boards listed as plain `<ul>` bullets.
- `.cover-bottom` groups the version pill and GitHub link as one flex item
  so they stay adjacent; `.cover` uses `justify-content: space-between`
  for even gaps between the remaining blocks.
- Both hero screenshots (`-dashboard.png`, `-wxgraph.png`) must fit the
  cover panel — the CSS keeps them single-line spaced; if the PDF grows
  past 2 pages, tighten `.cover` margins/font, not the panel order.
- Per AGENTS.md: user-facing guide changes ride along with `README.md` and
  `kHelpLines[]` in `cyd-horizon/settings.ino`.
