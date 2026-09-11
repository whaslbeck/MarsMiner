#!/bin/sh
# golden.sh — run the asset decoders over real ROMs and compare the output against known hashes.
#
# Why this exists: every decoder here was originally validated by diffing it against a Python
# reference implementation, file by file. That reference lives in the parent ReRfM project, so a
# standalone MarsMiner has no oracle — and the unit tests cover the DSP core and the play-set
# walk, not the image/font/table decoders. This is their regression net: whole-stage content
# hashes, pinned to one game at one ROM version.
#
#   tests/golden.sh [ROMS_DIR] [BASELINE]
#
# Skips (exit 0) when the ROMs are absent, like the other ROM-reading tests.
set -eu

ROMS=${1:-roms}
BASE=${2:-tests/baseline/rfm_asset_hashes.txt}
BIN=./marsminer

[ -x "$BIN" ] || { echo "[golden] $BIN not built — run make first"; exit 1; }
[ -f "$BASE" ] || { echo "[golden] baseline $BASE missing"; exit 1; }

GAME=$(ls "$ROMS"/update_0180/*_game.rom 2>/dev/null | head -1 || true)
if [ -z "$GAME" ] || [ ! -d "$ROMS/chips" ]; then
    echo "[skip] ROMs not found under $ROMS — pass your ROM directory as argv[1]"
    exit 0
fi

# md5 of a file, on Linux or macOS
md5of() {
    if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | cut -d' ' -f1
    else md5 -q "$1"; fi
}
# md5 of a stage directory: every file's bytes, in a stable order
md5dir() {
    if command -v md5sum >/dev/null 2>&1; then
        find "$1" -type f | LC_ALL=C sort | xargs cat | md5sum | cut -d' ' -f1
    else
        find "$1" -type f | LC_ALL=C sort | xargs cat | md5 -q
    fi
}
expect() { grep "^$1 " "$BASE" | awk '{print $2}'; }

# The baseline is tied to one ROM version — confirm this is it before comparing anything.
want_game=$(expect game.rom.md5)
have_game=$(md5of "$GAME")
if [ "$want_game" != "$have_game" ]; then
    echo "[golden] game.rom is not the one the baseline was built from"
    echo "         baseline $want_game"
    echo "         yours    $have_game  ($GAME)"
    echo "         Add a baseline for your ROM version rather than regenerating this one."
    exit 1
fi

OUT=$(mktemp -d)
WORK=$(mktemp -d)
trap 'rm -rf "$OUT" "$WORK"' EXIT INT TERM

echo "[golden] extracting (images take ~1-2 min) …"
"$BIN" --roms "$ROMS/chips" --bundle "$ROMS/update_0180" \
       --only images,fonts,messages,tables \
       --out "$OUT/assets" --work "$WORK" --loose --no-zip >/dev/null

fails=0
for stage in images fonts tables; do
    want=$(expect "$stage")
    if [ -z "$want" ]; then echo "[golden] no baseline entry for $stage"; fails=$((fails + 1)); continue; fi
    have=$(md5dir "$OUT/assets/$stage")
    n=$(find "$OUT/assets/$stage" -type f | wc -l | tr -d ' ')
    if [ "$want" = "$have" ]; then
        printf '%-10s %7s files  [OK]\n' "$stage" "$n"
    else
        printf '%-10s %7s files  [FAIL]  want %s  got %s\n' "$stage" "$n" "$want" "$have"
        fails=$((fails + 1))
    fi
done

if [ "$fails" -eq 0 ]; then
    echo "[golden] asset output matches the baseline"
else
    echo "[golden] $fails stage(s) differ — a decoder changed its output"
fi
exit $((fails > 0 ? 1 : 0))
