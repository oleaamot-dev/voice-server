#!/bin/sh
# End-to-end test: start a server, upload with gnome-voice-send, read back.
set -eu

# Non-ASCII command-line arguments need a UTF-8 locale.
export LC_ALL=C.UTF-8

: "${GVS_SERVER:=./gnome-voice-server}"
: "${GVS_SEND:=./gnome-voice-send}"
: "${GVS_SRCDIR:=.}"

work=$(mktemp -d)
pid=""
cleanup() {
  [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

fail() { echo "FAIL: $*" >&2; [ -f "$work/server.log" ] && cat "$work/server.log" >&2; exit 1; }

echo "s3cret-token" > "$work/token"
chmod 600 "$work/token"

: > "$work/empty.conf"
"$GVS_SERVER" --config "$work/empty.conf" --address 127.0.0.1 --port 0 \
  --port-file "$work/port" --storage-dir "$work/store" \
  --token-file "$work/token" --public-url http://voice.test \
  >"$work/server.log" 2>&1 &
pid=$!

i=0
while [ ! -s "$work/port" ]; do
  i=$((i + 1)); [ $i -gt 50 ] && fail "server did not start"
  sleep 0.1
done
port=$(cat "$work/port")
url="http://127.0.0.1:$port"
echo "server on $url"

# A fake but well-formed-enough Ogg payload: capture pattern + bytes.
music="$work/Music"
mkdir -p "$music"
{ printf 'OggS'; head -c 200000 /dev/urandom; } > "$music/GNOME.ogg"
cp "$GVS_SRCDIR/tests/data/GNOME.voice" "$music/GNOME.voice"

# 1. Upload without token is refused.
if GVS_TOKEN= "$GVS_SEND" -s "$url" -o "$music/GNOME.ogg" -q 2>"$work/err"; then
  fail "upload without token succeeded"
fi
grep -q 401 "$work/err" || fail "expected 401, got: $(cat "$work/err")"

# 2. Upload with token (audio + client's .voice).
id=$("$GVS_SEND" -s "$url" -t "$work/token" -o "$music/GNOME.ogg" -q) || fail "upload failed"
echo "stored $id"
[ -f "$work/store/voicegrams/$id.ogg" ]   || fail "audio not on disk"
[ -f "$work/store/voicegrams/$id.voice" ] || fail "metadata not on disk"
cmp -s "$music/GNOME.ogg" "$work/store/voicegrams/$id.ogg" || fail "audio corrupted"

grep -q 'name="Ole Aamot"' "$work/store/voicegrams/$id.voice" || fail "name not kept"
grep -q '>Oslo<'           "$work/store/voicegrams/$id.voice" || fail "location not kept"
grep -q "<stream>http://voice.test/voicegrams/$id.ogg</stream>" \
     "$work/store/voicegrams/$id.voice" || fail "stream not rewritten"

# 3. Listing contains it.
"$GVS_SEND" -s "$url" --list > "$work/list.xml" || fail "list failed"
grep -q "id=\"$id\"" "$work/list.xml" || fail "id missing from list"

# 4. Non-Ogg upload is refused.
echo "not audio at all" > "$work/fake.ogg"
if "$GVS_SEND" -s "$url" -t "$work/token" -o "$work/fake.ogg" -n x -q 2>"$work/err"; then
  fail "non-ogg accepted"
fi
grep -q 415 "$work/err" || fail "expected 415"

# 5. Metadata from options (no .voice file), with Norwegian letters.
rm -f "$work/solo.voice"
cp "$music/GNOME.ogg" "$work/solo.ogg"
id2=$("$GVS_SEND" -s "$url" -t "$work/token" -o "$work/solo.ogg" \
        -n "Bjørn" -l "Ålesund" --lat 62.4722 --lon 6.1495 -q) || fail "upload 2 failed"
grep -q 'name="Bjørn"' "$work/store/voicegrams/$id2.voice" || fail "utf-8 name lost"
grep -q '>Ålesund<'    "$work/store/voicegrams/$id2.voice" || fail "utf-8 location lost"

if command -v curl >/dev/null 2>&1; then
  # 6. Range request for seeking.
  code=$(curl -s -o "$work/part" -w '%{http_code}' -r 0-99 "$url/voicegrams/$id.ogg")
  [ "$code" = 206 ] || fail "range: got $code"
  [ "$(wc -c < "$work/part")" -eq 100 ] || fail "range length"
  # 7. Path traversal is not a voicegram.
  code=$(curl -s -o /dev/null -w '%{http_code}' --path-as-is "$url/voicegrams/../../etc/passwd")
  [ "$code" = 404 ] || fail "traversal: got $code"
  # 8. Invalid metadata is rejected.
  code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT -H 'Authorization: Bearer s3cret-token' \
         --data-binary '<radio/>' "$url/voicegrams/$id.voice")
  [ "$code" = 400 ] || fail "bad xml: got $code"
  # 9. Oversized upload: declared size is checked before reading.
  code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer s3cret-token' \
         -H 'Content-Type: audio/ogg' -H 'Content-Length: 999999999999' \
         -X POST "$url/voicegrams")
  [ "$code" = 413 ] || fail "oversize: got $code"
fi

# 10. Delete.
"$GVS_SEND" -s "$url" -t "$work/token" --delete "$id2" -q || fail "delete failed"
[ ! -f "$work/store/voicegrams/$id2.ogg" ] || fail "file still there after delete"

echo "PASS"
