#!/usr/bin/env python3
"""Render DEVELOPER-USERGUIDE.md to the printable tri-fold user guide PDF.

Converts the markdown source to styled HTML and prints it to
DEVELOPER-USERGUIDE.pdf via headless Chrome, so the .md stays the single
source of truth for both GitHub and the printed handout. The guide is meant
to be duplex-printed on one Letter sheet in landscape and folded twice
(letter fold): three panels per side, six total.

Layout (standard letter-fold panel order):
    OUTSIDE = [ inside flap | back cover | front cover ]   (right = cover)
    INSIDE  = [ panel 1      | panel 2    | panel 3     ]  (reads L -> R)

The markdown marks the split: `<!-- SIDE:OUTSIDE -->` / `<!-- SIDE:INSIDE -->`
start each side and `<!-- PANEL -->` splits its three panels, listed in
printed left-to-right order. `{{VERSION}}` on the cover is replaced with the
firmware version from .release-please-manifest.json.

Page backgrounds are the saved bg-*.png images generated once by
fetch_backgrounds.py — renders never hit the network. Dashed lines between
panels mark the folds; the flap (outside-left / inside-right) panel is
~1/16" narrower so it tucks without buckling.

Usage:
    cyd-horizon/.venv/bin/python docs/user-guide/render_userguide.py
Requires: the `markdown` package in the venv (`python -m pip install markdown`)
and Google Chrome.
"""

import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent   # docs/user-guide/
REPO = ROOT.parents[1]                            # repo root
SRC = ROOT / "DEVELOPER-USERGUIDE.md"
OUT = ROOT / "DEVELOPER-USERGUIDE.pdf"
MANIFEST = REPO / ".release-please-manifest.json"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# Letter landscape 11 x 8.5in, full bleed. Each side is a .sheet flex row of
# three .panel columns; __ROOT__ is replaced with the absolute guide dir.
CSS = """
@page { size: Letter landscape; margin: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial,
       sans-serif; font-size: 8.4pt; line-height: 1.3; color: #222; margin: 0; }
.sheet { display: flex; width: 11in; height: 8.5in;
         background-size: cover; background-position: center; }
.sheet.out { background-image: url("__ROOT__/bg-lines.png"); }
.sheet.in  { background-image: url("__ROOT__/bg-leaf.png");
             break-before: page; page-break-before: always; }
.panel { box-sizing: border-box; height: 8.5in; overflow: hidden;
         padding: 0.28in 0.26in; position: relative; }
.panel + .panel { border-left: 1.5px dashed #9db3c8; }   /* fold guides */
/* Standard letter-fold widths: the tuck-in flap is ~1/16" narrower. The
   flap is the left panel outside / right panel inside. */
.out .panel:nth-child(1) { width: 3.625in; }
.out .panel:nth-child(2),
.out .panel:nth-child(3) { width: 3.6875in; }
.in  .panel:nth-child(1),
.in  .panel:nth-child(2) { width: 3.6875in; }
.in  .panel:nth-child(3) { width: 3.625in; }
h1 { font-size: 22pt; margin: 0 0 2pt; color: #0b3d66; }
/* Section headers: rounded blocks in a darkened version of the sky palette,
   light text — reads as a designed card against the pale backgrounds. */
h2 { font-size: 10pt; margin: 7pt 0 3.5pt; color: #fff;
     background: linear-gradient(100deg, #1d5279, #0e3854);
     border-radius: 4px; padding: 2.5pt 7pt;
     display: flex; align-items: center; gap: 0.35em; }
/* Emoji glyphs sit higher on the baseline than Latin text — as flex items
   the .ic span and the text are both centered on the bar's midline, but the
   emoji ink still rides high in its box, so nudge it down ~2px. */
h2 .ic { line-height: 1; position: relative; top: 2px; }
.panel > h2:first-child { margin-top: 0; }
p { margin: 2.5pt 0; }
ul, ol { margin: 2pt 0 3pt; padding-left: 13pt; }
li { margin: 0 0 1pt; }
hr { border: none; border-top: 1px solid #999; margin: 5pt 0 3pt; }
.footer { font-size: 7pt; color: #555; }
.warn { border: 1.5px solid #b3392e; border-left-width: 6px;
        background: rgba(253, 240, 240, 0.85); color: #7a1f16;
        padding: 5pt 7pt; margin: 9pt 0; font-size: 8.2pt;
        border-radius: 4px; }
/* Info card for hardware notes (e.g. the external-speaker requirement).
   Light-blue fill + navy text — clearly a note, not a section heading. */
.callout { color: #103a5c; background: linear-gradient(100deg, #e4eff8, #d0e2f0);
           border: 1px solid #7da8c8; border-radius: 5px;
           padding: 5pt 8pt; margin: 6pt 0; }
/* Wind tier labels: the device's green/yellow/orange/red, darkened so they stay
   legible on the pale page backgrounds (a bright yellow washes out). */
.wc { font-weight: 700; }
.wc-g { color: #1a7f37; }
.wc-y { color: #9a7b00; }
.wc-o { color: #c05621; }
.wc-r { color: #b3392e; }
/* Wind tier table — the guide's only table. No left padding so the first
   column lines up with the paragraph text; light rules against the page art. */
table { border-collapse: collapse; width: 100%; margin: 2pt 0 1pt;
        font-size: 7.5pt; }
th, td { padding: 0.3pt 4pt 0.3pt 0; text-align: left; vertical-align: top; }
th { color: #0b3d66; font-weight: 700; border-bottom: 1.2px solid #7da8c8; }
td { border-bottom: 0.6px solid #d3e2ef; }
tr:last-child td { border-bottom: none; }
.shot { margin: 8pt 0 0; }   /* a line of air between text and images */
.shot img, img.hero { width: 100%; border: 1px solid #999;
                      border-radius: 4px; }
/* Front cover: title block at the top, version+source footer pinned to the
   bottom, hero shots filling the middle — flexbox spreads the panel height. */
.cover { text-align: center; height: 100%;
         display: flex; flex-direction: column; justify-content: space-between;
         background-image: url("__ROOT__/bg-blur.png");
         background-size: cover; background-position: center;
         margin: -0.28in -0.26in; padding: 0.22in 0.24in 0.20in; }
/* space-between distributes the gaps; every cover child needs zero vertical
   margin so the spacing stays uniform. */
.cover p { font-size: 8pt; margin: 0; }
.cover img.logo { width: 88%; margin: 0 auto; display: block; }
.cover h1 { border: none; margin-top: 0; }
.cover .desc { width: 88%; margin: 0 auto; text-align: left;
               line-height: 1.3; font-size: 8.4pt; box-sizing: border-box;
               color: #103a5c; background: linear-gradient(100deg, #e4eff8, #d0e2f0);
               border: 1px solid #7da8c8; border-radius: 5px; padding: 5pt 8pt; }
.cover img.hero { width: 88%; margin: 0 auto; display: block;
                  border: 1px solid #999; border-radius: 4px; }
/* Version pill + repo link travel together as the last flex item so the
   pill sits right above the link instead of taking its own spaced slot. */
.cover-bottom { text-align: center; }
.cover-bottom .ver { margin-bottom: 4pt; }
.ver { display: inline-block; padding: 2pt 10pt;
       font-size: 8pt; color: #0b3d66; border: 1px solid #0b3d66;
       border-radius: 10px; }
.cover .footer { margin-bottom: 0; }
"""


def ensure_latest_manifest():
    """Refuse to render when the worktree manifest is stale.

    The manifest only changes via release-please merges on origin/main, so a
    local manifest that differs from the remote's means the checkout is behind
    (or the file was hand-edited) and the printed version would be wrong.
    """
    try:
        subprocess.run(["git", "fetch", "origin", "-q"], cwd=REPO,
                       timeout=30, check=True)
    except Exception:
        print("warning: could not fetch origin; rendering with the local "
              "manifest", file=sys.stderr)
        return
    remote = subprocess.run(
        ["git", "show", "origin/main:.release-please-manifest.json"],
        cwd=REPO, capture_output=True, text=True)
    if remote.returncode != 0:
        return   # no manifest on the remote; nothing to compare against
    try:
        remote_ver = json.loads(remote.stdout)["."]
        local_ver = json.loads(MANIFEST.read_text())["."]
    except Exception:
        sys.exit("error: could not parse .release-please-manifest.json")
    if remote_ver != local_ver:
        sys.exit("error: .release-please-manifest.json is stale - the local "
                 "git commits are behind origin/main and must be corrected "
                 "first (e.g. `git pull`), then re-run this script.")


def firmware_version():
    # The guide documents the NEXT release, not the last one: bump the
    # manifest's patch number and append "+" (that version or newer).
    try:
        ver = json.loads(MANIFEST.read_text())["."]
        major, minor, patch = (int(p) for p in ver.split("."))
        return "v%d.%d.%d+" % (major, minor, patch + 1)
    except Exception:
        return ""


def page_count(pdf_path):
    try:
        import pymupdf
        return len(pymupdf.open(str(pdf_path)))
    except Exception:
        pass
    try:
        data = pdf_path.read_bytes()
        return len(re.findall(rb"/Type\s*/Page[^s]", data))
    except Exception:
        return -1


def main():
    try:
        import markdown
    except ImportError:
        sys.exit("markdown package required: "
                 "cyd-horizon/.venv/bin/python -m pip install markdown")
    if not pathlib.Path(CHROME).exists():
        sys.exit(f"Chrome not found at {CHROME}")
    ensure_latest_manifest()

    # Regenerate the simulated dashboard screens first (cached data from
    # mock_data.json unless --refresh; non-fatal if it fails — a stale/missing
    # image just shows alt text in the PDF).
    mock = ROOT / "render_dash_mock.py"
    if mock.exists():
        r = subprocess.run([sys.executable, str(mock)],
                           capture_output=True, text=True, timeout=120)
        print(r.stdout.strip() or r.stderr.strip()[:300])

    md = SRC.read_text()
    ver = firmware_version()
    if ver:
        md = md.replace("{{VERSION}}", f'<div class="ver">Firmware {ver}</div>')

    # Split into the two printed sides, then each side into its three panels
    # (listed left-to-right as printed).
    sides = {}
    chunks = re.split(r"<!--\s*SIDE:(OUTSIDE|INSIDE)\s*-->", md)
    it = iter(chunks[1:])
    for name, body in zip(it, it):
        sides[name.lower()] = re.split(r"<!--\s*PANEL\s*-->", body)
    if set(sides) != {"outside", "inside"}:
        sys.exit("expected <!-- SIDE:OUTSIDE --> and <!-- SIDE:INSIDE --> "
                 "markers in the markdown")

    def sheet(cls, panels):
        cells = "".join(f'<div class="panel" markdown="1">\n{p}\n</div>\n'
                        for p in panels)
        return f'<div class="sheet {cls}" markdown="1">\n{cells}</div>\n'

    wrapped = (sheet("out", sides["outside"])
               + sheet("in", sides["inside"]))
    body = markdown.markdown(wrapped,
                             extensions=["sane_lists", "md_in_html", "tables"])
    # Wrap a leading emoji in <h2> so CSS can center it on the header text.
    body = re.sub(r"<h2>(\S+)\s+", r'<h2><span class="ic">\1</span> ', body)
    html = (f"<!doctype html><meta charset='utf-8'>"
            f"<style>{CSS}</style>{body}").replace("__ROOT__", str(ROOT))

    # Write into the repo's git-ignored build/; image srcs sit next to the
    # markdown, so point them at the real files.
    def resolve_src(m):
        src = m.group(1)
        if src.startswith("http") or src.startswith("/"):
            return m.group(0)
        return f'src="{(ROOT / src).resolve()}"'

    html = re.sub(r'src="([^"]+\.(?:png|svg|jpg))"', resolve_src, html)
    build = REPO / "build"
    build.mkdir(exist_ok=True)
    html_path = build / "userguide.html"
    html_path.write_text(html)

    subprocess.run(
        [CHROME, "--headless=new", "--disable-gpu", "--disable-extensions",
         "--no-pdf-header-footer", f"--print-to-pdf={OUT}",
         f"file://{html_path}"],
        check=True, capture_output=True, timeout=60)

    pages = page_count(OUT)
    print(f"wrote {OUT.name} ({pages} page(s))"
          + ("" if pages == 2 else " — expected 2, adjust content or CSS"))


if __name__ == "__main__":
    main()
