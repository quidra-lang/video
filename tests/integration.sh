#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGE_ROOT="$(dirname "$ROOT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg CLI not available; skipping"; exit 0; }
VIDEO="$TMP/input.mp4"
ffmpeg -loglevel error -y \
  -f lavfi -i "color=c=red:s=8x6:r=2:d=1.5" \
  -frames:v 3 -c:v mpeg4 -pix_fmt yuv420p "$VIDEO"

cat > "$TMP/test.qui" <<QUI
import qvideo = video

int | error run()
    qvideo.Reader reader = try qvideo.open("$VIDEO")
    print(reader.width())
    print(NL)
    print(reader.height())
    print(NL)
    print(reader.position())
    print(NL)
    tensor<nat8> | none | error first = reader.read<nat8>()
    match first
        tensor<nat8> pixels
            print(pixels.shape()[0])
            print(NL)
            print(pixels.shape()[1])
            print(NL)
            print(pixels.shape()[2])
            print(NL)
        none
            print("unexpected-eof")
            print(NL)
        error problem
            print(problem)
            print(NL)
    print(reader.position())
    print(NL)
    try reader.seek(0)
    print(reader.position())
    print(NL)
    tensor<real32> | none | error gray = reader.read<real32>(channel = 1)
    match gray
        tensor<real32> pixels
            print(pixels.shape()[0])
            print(NL)
            print(pixels.shape()[1])
            print(NL)
            print(pixels.shape()[2])
            print(NL)
        none
            print("unexpected-eof")
            print(NL)
        error problem
            print(problem)
            print(NL)
    qvideo.Reader copied = reader
    tensor<nat8> | none | error copied_frame = copied.read<nat8>()
    match copied_frame
        tensor<nat8>
            print(copied.position())
            print(NL)
        none
            print("unexpected-eof")
            print(NL)
        error problem
            print(problem)
            print(NL)
    print(reader.position())
    print(NL)
    return 0

auto | error result = run()
match result
    int
        int ignored = result
    error problem
        print(problem)
        print(NL)
QUI

OUT="$(QUIDRA_PACKAGE_PATH="$PACKAGE_ROOT" "$QUIDRA" run "$TMP/test.qui")"
EXPECTED="$(printf '8\n6\n0\n3\n6\n8\n1\n0\n1\n6\n8\n2\n1')"
if [[ "$OUT" != "$EXPECTED" ]]; then
    printf 'video output mismatch\nexpected:\n%s\nactual:\n%s\n' "$EXPECTED" "$OUT" >&2
    exit 1
fi

AOT="$TMP/video-aot"
QUIDRA_PACKAGE_PATH="$PACKAGE_ROOT" "$QUIDRA" build "$TMP/test.qui" -o "$AOT"
AOT_OUT="$("$AOT")"
if [[ "$AOT_OUT" != "$EXPECTED" ]]; then
    printf 'video AOT output mismatch\nexpected:\n%s\nactual:\n%s\n' "$EXPECTED" "$AOT_OUT" >&2
    exit 1
fi

REPL_OUT="$(
    QUIDRA_PACKAGE_PATH="$PACKAGE_ROOT" "$QUIDRA" repl < "$TMP/test.qui"
)"
for expected_line in 8 6 0 3 1 2; do
    if ! grep -Fxq "$expected_line" <<< "$REPL_OUT"; then
        printf 'video REPL/JIT output missing expected line %s\nactual:\n%s\n' "$expected_line" "$REPL_OUT" >&2
        exit 1
    fi
done

ORIGINAL="$TMP/resource.mp4"
REPLACEMENT="$TMP/replacement.mp4"
cp "$VIDEO" "$ORIGINAL"
ffmpeg -loglevel error -y \
  -f lavfi -i "color=c=blue:s=10x4:r=2:d=1.5" \
  -frames:v 3 -c:v mpeg4 -pix_fmt yuv420p "$REPLACEMENT"

cat > "$TMP/identity.qui" <<QUI
import qvideo = video
int | error run()
    qvideo.Reader reader = try qvideo.open("$ORIGINAL")
    try file.move("$ORIGINAL", "$TMP/opened.mp4")
    try file.move("$REPLACEMENT", "$ORIGINAL")
    qvideo.Reader copied = reader
    tensor<nat8> | none | error frame = copied.read<nat8>()
    match frame
        tensor<nat8> pixels
            print(pixels.shape()[1] == 6 and pixels.shape()[2] == 8)
            print(NL)
        none
            print(false)
            print(NL)
        error
            print(false)
            print(NL)
    return 0
auto | error result = run()
match result
    int
        int ignored = result
    error problem
        print(problem)
        print(NL)
QUI
[[ "$(QUIDRA_PACKAGE_PATH="$PACKAGE_ROOT" "$QUIDRA" run "$TMP/identity.qui")" == "true" ]]

echo "video integration: ok"
