#!/bin/sh
# Smoke test for the musicbrainz CLI. Requires network access to
# musicbrainz.org. Usage: sh test/cli-smoke.sh [path-to-binary]
set -u
BIN="${1:-./build/musicbrainz}"
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

[ -x "$BIN" ] || { echo "binary not found: $BIN (build first)"; exit 1; }

# 1. human search output
OUT=$("$BIN" search artist --query "artist:radiohead" --limit 1) || bad "search exit"
echo "$OUT" | grep -q "Radiohead" && ok "search artist" || bad "search artist: $OUT"

# 2. human lookup with tracks
OUT=$("$BIN" lookup release 4b3d18cc-8937-36f4-8de0-481088be58e6) || bad "lookup exit"
echo "$OUT" | grep -q "Airbag" && ok "lookup release tracks" || bad "lookup tracks: $OUT"
echo "$OUT" | grep -q "EMI Music Canada" && ok "lookup label-info" || bad "label-info"

# 3. browse
OUT=$("$BIN" browse release --artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --limit 2) || bad "browse exit"
echo "$OUT" | grep -q "of " && ok "browse releases" || bad "browse: $OUT"

# 4. bad inc rejected, exit 2
if "$BIN" lookup artist a74b1b7f-71a5-4011-9441-d0b5e4122711 --inc bogus-thing >/dev/null 2>&1; then
  bad "bad inc accepted"
else
  [ $? -eq 2 ] && ok "bad inc rejected" || bad "bad inc wrong exit"
fi

# 5. unknown entity rejected
"$BIN" lookup frobnicate x >/dev/null 2>&1
[ $? -eq 2 ] && ok "unknown entity rejected" || bad "unknown entity exit"

# 6. unknown method-gated endpoint rejected (annotation has no lookup)
"$BIN" lookup annotation x >/dev/null 2>&1
[ $? -eq 2 ] && ok "ungated lookup rejected" || bad "ungated lookup exit"

# 7. --json emits parseable JSON
OUT=$("$BIN" search artist --query "artist:radiohead" --limit 1 --json) || bad "json exit"
echo "$OUT" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['artists'][0]['name']=='Radiohead'" \
  && ok "json output" || bad "json parse"

# 8. real discid round-trip
OUT=$("$BIN" lookup discid 1zOfzhFgYPD05pEMZa8j5o.dR.g-) || bad "discid exit"
echo "$OUT" | grep -q "OK Computer" && ok "discid lookup" || bad "discid: $OUT"

echo "--- $PASS passed, $FAIL failed ---"
[ "$FAIL" -eq 0 ]
