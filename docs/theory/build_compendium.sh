#!/usr/bin/env bash
# Builds the Mathematical Compendium PDF from docs/theory/Mathematical-Compendium.tex.
#
#   docs/theory/build_compendium.sh [out-dir]     (default: docs/theory/)
#
# Writes <out-dir>/Mathematical-Compendium-v<version>.pdf, where <version> is the project
# version in CMakeLists.txt. Fails (exit 1) if the .tex states a different version, so the
# document can't be published under a release it doesn't describe.
# Needs pdflatex with amsmath, mathptmx/helvet/courier, geometry, fancyhdr, titlesec, booktabs,
# enumitem, tcolorbox, lastpage, hyperref (Debian/Ubuntu: texlive-latex-recommended,
# texlive-latex-extra, texlive-fonts-recommended).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$here/Mathematical-Compendium.tex"
out_dir="${1:-$here}"

version="$(sed -n 's/^project(NeonSynth VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n 1)"
if [[ -z "$version" ]]; then
    echo "error: no project version found in $root/CMakeLists.txt" >&2
    exit 1
fi

tex_version="$(sed -n 's/^\\newcommand{\\NSVersion}{\([^}]*\)}.*/\1/p' "$src" | head -n 1)"
if [[ "$tex_version" != "$version" ]]; then
    echo "error: $src says version '${tex_version:-<none>}', CMakeLists.txt says '$version'" >&2
    exit 1
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Twice: the first pass writes the page and equation references, the second uses them.
for pass in 1 2; do
    if ! pdflatex -interaction=nonstopmode -halt-on-error -output-directory="$work" \
                  -jobname=compendium "$src" > "$work/pass$pass.out"; then
        tail -n 40 "$work/pass$pass.out" >&2
        echo "error: pdflatex failed (pass $pass)" >&2
        exit 1
    fi
done

if grep -q -E 'LaTeX Warning: (Reference|Label).*(undefined|multiply defined)|There were undefined references' "$work/compendium.log"; then
    grep -E 'LaTeX Warning' "$work/compendium.log" >&2
    echo "error: unresolved or duplicate references" >&2
    exit 1
fi
if grep -q -E '^Overfull \\[hv]box' "$work/compendium.log"; then
    grep -E -A1 '^Overfull \\[hv]box' "$work/compendium.log" >&2
    echo "warning: overfull boxes (see above)" >&2
fi

pdf="$out_dir/Mathematical-Compendium-v$version.pdf"
cp "$work/compendium.pdf" "$pdf"
echo "$pdf ($(sed -n 's/^Output written on .*(\([0-9]*\) pages\?.*/\1/p' "$work/pass2.out") pages)"
