#!/bin/sh
# Builds/obtains the speech runtime dependencies of Wesnoth Access for Windows x64.
#
# Runs inside the upstream MinGW image (wesnoth/wesnoth:mingw-master).
#   $1 = output directory
#
# 1. Universal Speech (https://github.com/qtnc/UniversalSpeech, MIT licence) is
#    compiled from source at a pinned commit with the x86_64 MinGW compiler.
# 2. The NVDA controller client (x64 build, LGPL-2.1, distributed by NV Access)
#    is downloaded from NV Access. Universal Speech loads it from its own
#    directory under the name nvdaControllerClient.dll.
#
# Pinning: US_COMMIT is a fixed commit hash. For NVDA, NVDA_CC_SHA256 can pin
# the archive checksum; the checksum of every downloaded archive is printed so
# it can be recorded.
set -eu

OUT="$1"
US_REPO="${US_REPO:-https://github.com/qtnc/UniversalSpeech.git}"
US_COMMIT="${US_COMMIT:-25e8b49c2f9df67f8baa0fbcb25fb1a0f1a3a558}"
NVDA_CC_VERSIONS="${NVDA_CC_VERSIONS:-2024.4.2 2024.4.1 2024.1}"
NVDA_CC_SHA256="${NVDA_CC_SHA256:-}"
WORK="$(mktemp -d)"

mkdir -p "$OUT"

echo "== Universal Speech $US_COMMIT"
git clone --quiet "$US_REPO" "$WORK/us"
git -C "$WORK/us" checkout --quiet "$US_COMMIT"
cd "$WORK/us"
# src/java is part of main.def's export list, so it must be compiled too.
x86_64-w64-mingw32-gcc -std=gnu99 -O2 -s -DRELEASE -mthreads -w -shared -static-libgcc \
	-o "$OUT/UniversalSpeech.dll" \
	src/UniversalSpeech.c src/java/*.c src/windows/*.c src/windows/main.def \
	-Wl,--add-stdcall-alias -Wl,--enable-stdcall-fixup \
	-lole32 -loleaut32 -luuid -lpsapi -lversion
cp LICENSE.txt "$OUT/UniversalSpeech-LICENSE.txt"
cd - >/dev/null
x86_64-w64-mingw32-objdump -p "$OUT/UniversalSpeech.dll" | grep -q 'PE32+' || { echo "UniversalSpeech.dll is not PE32+" >&2; exit 1; }
echo "UniversalSpeech.dll sha256: $(sha256sum "$OUT/UniversalSpeech.dll" | cut -d' ' -f1)"

echo "== NVDA controller client"
got=""
for v in $NVDA_CC_VERSIONS; do
	for url in "https://download.nvaccess.org/releases/$v/nvda_${v}_controllerClient.zip" \
	           "https://www.nvaccess.org/files/nvda/releases/$v/nvda_${v}_controllerClient.zip"; do
		echo "trying $url"
		if curl -fsSL --retry 3 -o "$WORK/cc.zip" "$url"; then
			got="$v"; break 2
		fi
	done
done
if [ -z "$got" ]; then
	echo "ERROR: could not download the NVDA controller client" >&2
	exit 1
fi
sum="$(sha256sum "$WORK/cc.zip" | cut -d' ' -f1)"
echo "NVDA controller client $got archive sha256: $sum"
if [ -n "$NVDA_CC_SHA256" ] && [ "$NVDA_CC_SHA256" != "$sum" ]; then
	echo "ERROR: checksum mismatch (expected $NVDA_CC_SHA256)" >&2
	exit 1
fi
mkdir -p "$WORK/cc"
python3 - "$WORK/cc.zip" "$WORK/cc" <<'EOF'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
EOF
find "$WORK/cc" -type f | sed 's/^/  /'
dll=""
for cand in $(find "$WORK/cc" -iname 'nvdaControllerClient*.dll'); do
	if x86_64-w64-mingw32-objdump -f "$cand" | grep -q 'x86-64'; then dll="$cand"; break; fi
done
if [ -z "$dll" ]; then
	echo "ERROR: no x64 nvdaControllerClient DLL in archive" >&2
	exit 1
fi
cp "$dll" "$OUT/nvdaControllerClient.dll"
lic="$(find "$WORK/cc" -iname 'license*' | head -n1)"
if [ -n "$lic" ]; then cp "$lic" "$OUT/nvdaControllerClient-LICENSE.txt"; else
	printf 'NVDA Controller Client (c) NV Access Limited.\r\nDistributed under the GNU Lesser General Public License version 2.1.\r\nSource: https://github.com/nvaccess/nvda/tree/master/extras/controllerClient\r\n' > "$OUT/nvdaControllerClient-LICENSE.txt"
fi

cat > "$OUT/SOURCES.txt" <<EOF
Third-party speech components bundled with Wesnoth Access

UniversalSpeech.dll
  Source: $US_REPO
  Commit: $US_COMMIT
  Licence: MIT (see UniversalSpeech-LICENSE.txt)
  Built with x86_64-w64-mingw32-gcc inside wesnoth/wesnoth:mingw-master.

nvdaControllerClient.dll (x64)
  Obtained from NV Access, NVDA $got controller client archive
  Archive sha256: $sum
  Licence: LGPL-2.1 (see nvdaControllerClient-LICENSE.txt)
  Source: https://github.com/nvaccess/nvda/tree/master/extras/controllerClient
EOF
rm -rf "$WORK"
echo "speech dependencies ready in $OUT"
