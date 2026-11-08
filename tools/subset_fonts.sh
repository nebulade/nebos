#!/bin/sh
# Regenerate the checked-in UI font subsets.  Requires pyftsubset (fonttools)
# and the Croscore/Liberation source fonts; normal builds use the checked-in
# files and do not run this script.
set -e
cd "$(dirname "$0")/.."
U='U+0020-00FF,U+20AC,U+2013-2014,U+2018-2019,U+201C-201D,U+2022,U+2026,U+00A0'
SUB=${PYFTSUBSET:-pyftsubset}

"$SUB" /usr/share/fonts/croscore/Arimo-Regular.ttf --unicodes="$U" \
	--layout-features='' --output-file=fonts/sans.ttf
"$SUB" /usr/share/fonts/croscore/Arimo-Bold.ttf --unicodes="$U" \
	--layout-features='' --output-file=fonts/sans-b.ttf
"$SUB" /usr/share/fonts/croscore/Arimo-Italic.ttf --unicodes="$U" \
	--layout-features='' --output-file=fonts/sans-i.ttf
"$SUB" /usr/share/fonts/croscore/Arimo-BoldItalic.ttf --unicodes="$U" \
	--layout-features='' --output-file=fonts/sans-bi.ttf
"$SUB" /usr/share/fonts/liberation/LiberationMono-Regular.ttf --unicodes="$U" \
	--layout-features='' --output-file=fonts/mono.ttf
