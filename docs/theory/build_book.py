# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Checks "The DSP Behind NeonSynth" against the source and prints it to PDF.

    python docs/theory/build_book.py --check             # checks only
    python docs/theory/build_book.py                     # checks, then writes the PDF to docs/theory/
    python docs/theory/build_book.py --out build/book    # ... or to another folder

The check fails (exit 1) when the book does not match the code it ships with:
  * "Target instrument" must name the version in CMakeLists.txt;
  * every link to the repository at a tag (github.com/kv244/NeonSynth/.../vX.Y.Z) must
    name that version too;
  * every relative link and image must point at a file that exists, and a link to
    a #section (in the book or another Markdown file) at a heading that exists.
The book in the repository is never changed; the PDF is built from it as it is.

It is printed the way docs/build_manual.py prints the manual (Markdown -> HTML ->
Chromium through Playwright), and borrows that script's version, heading-anchor and
slug functions; the page style is the book's own.

Display equations are ```equation blocks: plain text, printed in the body font, where
x^{...} and x_{...} (or x^2, f_c: one run of letters and digits) become superscripts
and subscripts. Diagrams and pseudocode are ```text blocks, printed in monospace.

Needs: pip install markdown playwright && playwright install chromium
"""
import argparse
import html
import os
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
DOCS = HERE.parent
BOOK = HERE / "DSP-Behind-NeonSynth.md"
TITLE = "The DSP Behind NeonSynth"

sys.dont_write_bytecode = True                  # no __pycache__ left in docs/
sys.path.insert(0, str(DOCS))
import build_manual  # noqa: E402  (docs/build_manual.py: project_version, heading_anchors, github_slug)

REPO_TAG_LINK = re.compile(r"github\.com/kv244/NeonSynth/(?:blob|tree)/v(\d+\.\d+\.\d+)")


def check(md, version):
    problems = []
    m = re.search(r"Target instrument:\s*NeonSynth v(\d+\.\d+\.\d+)", md)
    if not m:
        problems.append("the 'Target instrument: NeonSynth vX.Y.Z' line is missing")
    elif m.group(1) != version:
        problems.append(f"the book targets v{m.group(1)} but CMakeLists.txt says v{version}: "
                        f"update the edition line (and the text) for this release")
    stale = sorted({tag for tag in REPO_TAG_LINK.findall(md) if tag != version})
    if stale:
        problems.append(f"links to the repository at v{', v'.join(stale)}; this edition is for v{version}")
    anchors = build_manual.heading_anchors(md)
    for link in re.findall(r"\]\(([^)\s]+)\)", md):
        if re.match(r"[a-z]+:", link):
            continue
        target, _, fragment = link.partition("#")
        path = (BOOK.parent / target).resolve() if target else BOOK
        if target and not path.exists():
            problems.append(f"broken relative link: {link}")
            continue
        if fragment and path.suffix == ".md":
            found = anchors if path == BOOK else build_manual.heading_anchors(path.read_text(encoding="utf-8"))
            if fragment not in found:
                problems.append(f"link to a missing section: {link}")
    return problems


# ---------------------------------------------------------------------------------------------
# Equations

EQUATION = re.compile(r"^([ \t]*)```equation[ \t]*\n(.*?)\n[ \t]*```[ \t]*$", re.S | re.M)
SCRIPT = re.compile(r"([\^_])(?:\{([^{}]*)\}|([A-Za-z0-9]+))")
MARKDOWN_SPECIAL = {"*": "&#42;", "_": "&#95;", "[": "&#91;", "]": "&#93;", "`": "&#96;", "\\": "&#92;"}


def equation_html(text):
    """Plain-text equation -> HTML: escaped, ^{..} / _{..} as <sup> / <sub>, lines kept."""
    def script(m):
        tag = "sup" if m.group(1) == "^" else "sub"
        inner = m.group(2) if m.group(2) is not None else m.group(3)
        return f"<{tag}>{SCRIPT.sub(script, inner)}</{tag}>"     # e^{j2π f n / F_s}: F_s inside

    out = SCRIPT.sub(script, html.escape(text, quote=False))
    # Markdown must leave the result alone: hide what it would read as emphasis or links.
    out = "".join(MARKDOWN_SPECIAL.get(c, c) for c in out)
    return "<br>".join(line.rstrip() for line in out.split("\n"))


def equations_to_html(md):
    """Each ```equation block becomes one line of inline HTML at the block's indentation, so it
    works inside list items too (Python-Markdown only finds fenced blocks at the top level)."""
    def block(m):
        indent = m.group(1)
        lines = [line[len(indent):] if line.startswith(indent) else line.lstrip() for line in m.group(2).split("\n")]
        return f'{indent}<span class="eq">{equation_html(chr(10).join(lines))}</span>'
    return EQUATION.sub(block, md)


# ---------------------------------------------------------------------------------------------
# Page

CSS = """
@page { size: A4; margin: 17mm 18mm 20mm 18mm;
  @bottom-left  { content: "%(title)s (v%(version)s)"; vertical-align: top; padding-top: 2.5mm;
                  border-top: 0.5pt solid #d5dce5; font: 8pt Georgia, Lora, 'DejaVu Serif', serif; color: #4a5568; }
  @bottom-right { content: "Page " counter(page) " of " counter(pages); vertical-align: top; padding-top: 2.5mm;
                  border-top: 0.5pt solid #d5dce5; font: 8pt Georgia, Lora, 'DejaVu Serif', serif; color: #4a5568; } }
:root { --ink: #1f2933; --head: #142236; --rule: #d5dce5; --panel: #f6f8fb; --blue: #1b8bd0; --link: #0b79b8; }
html { font-family: Georgia, Lora, 'TeX Gyre Pagella', 'DejaVu Serif', serif; font-size: 10.2pt; line-height: 1.48;
       color: var(--ink); -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { margin: 0; }
p { margin: 0 0 3.2mm; orphans: 3; widows: 3; }
li { margin: 0 0 1.1mm; orphans: 2; widows: 2; }
ul, ol { margin: 0 0 3.2mm; padding-left: 6mm; }
a { color: var(--link); text-decoration: none; }
strong { color: var(--head); }
h1, h2, h3, h4 { color: var(--head); break-after: avoid; font-weight: 700; }
h2 { font-size: 15.5pt; margin: 6mm 0 2.4mm; padding-bottom: 1mm; border-bottom: 0.6pt solid var(--rule); }
h3 { font-size: 11.6pt; margin: 4.5mm 0 1.6mm; }
h2 + h3 { margin-top: 2mm; }

/* Title block (page 1) */
.titleblock { border-bottom: 1.6pt solid var(--head); margin-bottom: 4mm; padding-bottom: 1mm; }
.titleblock img { display: block; width: 100%%; height: auto; margin: 0 0 6mm; }
.titleblock h1 { font-size: 24pt; margin: 0 0 1mm; }
.titleblock p { margin: 0 0 1.5mm; }
.titleblock p:has(em) { font-size: 13pt; color: #3b4a5c; }
.titleblock p:last-child { font-size: 9.5pt; color: #4a5568; }

/* Parts: a shaded band with a dark rule on the left */
h1[id^="part-"] { font-size: 19pt; background: #eef2f7; border-left: 3.5pt solid var(--head);
                  padding: 2.6mm 5mm; margin: 4mm 0 5mm; }
#glossary, #part-i--architecture-and-dsp, #part-iii--supporting-material { break-before: page; }

/* Contents */
.contents { margin-bottom: 4mm; }
.contents p { margin: 0; padding: 0.9mm 1.5mm; background: #f2f5f9; border-bottom: 0.5pt dotted #b9c3cf; }
.contents ul { list-style: none; padding: 0; margin: 0 0 1mm; }
.contents li { margin: 0; padding: 0.45mm 1.5mm; border-bottom: 0.5pt dotted #c9d1db; font-size: 9.6pt; }
.contents a { color: var(--ink); }

/* Code, diagrams, equations */
code { font-family: 'Liberation Mono', Consolas, 'DejaVu Sans Mono', monospace; font-size: 8.6pt;
       background: #f3f5f8; border: 0.5pt solid #dbe1e8; border-radius: 1.5pt; padding: 0.2mm 0.8mm; }
pre { background: var(--panel); border: 0.5pt solid var(--rule); border-left: 3pt solid var(--blue);
      padding: 3mm 4mm; margin: 0 0 3.6mm; break-inside: avoid; white-space: pre; overflow: hidden; }
pre code { background: none; border: 0; padding: 0; font-size: 8.4pt; line-height: 1.45; color: #1a202c; }
.eq { display: block; white-space: pre-wrap; background: var(--panel); border: 0.5pt solid #e1e6ed;
      border-left: 3pt solid #26354a; padding: 2.4mm 4mm; margin: 0.5mm 0 0; break-inside: avoid; line-height: 1.6; }
p:has(> .eq) { margin-bottom: 3.6mm; break-inside: avoid; }
.eq sub, .eq sup, p sub, p sup, li sub, li sup, td sub, td sup { font-size: 70%%; line-height: 0; }

/* Tables */
table { border-collapse: collapse; width: 100%%; margin: 1mm 0 4mm; font-size: 8.9pt; line-height: 1.35; }
thead { display: table-header-group; }
tr { break-inside: avoid; }
th, td { border: 0.5pt solid #d3dbe5; padding: 1.3mm 2mm; vertical-align: top; text-align: left; }
th { background: #eef3f8; color: var(--head); font-weight: 700; }
td code { font-size: 8.2pt; }

/* "What was tried" and other asides */
blockquote { margin: 1mm 0 4mm; padding: 3mm 4.5mm 0.5mm; background: var(--panel); border: 0.5pt solid var(--rule);
             border-left: 3pt solid var(--blue); }
blockquote p:first-child strong { font-size: 10.8pt; }
"""


def to_html(md, version):
    import markdown
    body = markdown.markdown(
        equations_to_html(md), extensions=["tables", "fenced_code", "toc", "sane_lists", "md_in_html"],
        extension_configs={"toc": {"slugify": build_manual.github_slug}})
    css = CSS % {"title": TITLE, "version": version}
    return (f"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            f"<title>{TITLE} (v{version})</title><style>{css}</style></head><body>{body}</body></html>")


def print_pdf(html_text, pdf_path):
    from playwright.sync_api import sync_playwright
    page_html = BOOK.parent / ".book-print.html"       # next to the book so images/ resolves
    page_html.write_text(html_text, encoding="utf-8")
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            page.goto(page_html.as_uri())
            page.wait_for_load_state("networkidle")
            page.evaluate("Promise.all(Array.from(document.images).map(i => i.decode().catch(() => null)))")
            page.pdf(path=str(pdf_path), prefer_css_page_size=True, print_background=True)
            browser.close()
    finally:
        page_html.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="check only; write nothing")
    ap.add_argument("--out", type=pathlib.Path, default=HERE)
    a = ap.parse_args()

    version = build_manual.project_version()
    md = BOOK.read_text(encoding="utf-8")
    problems = check(md, version)
    for p in problems:
        print(f"::error file=docs/theory/{BOOK.name}::{p}" if os.environ.get("GITHUB_ACTIONS") else f"error: {p}")
    if problems:
        return 1
    print(f"book matches v{version}")
    if a.check:
        return 0

    a.out.mkdir(parents=True, exist_ok=True)
    pdf = a.out / f"The-DSP-Behind-NeonSynth-v{version}.pdf"
    print_pdf(to_html(md, version), pdf)
    print(f"wrote {pdf} ({pdf.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
