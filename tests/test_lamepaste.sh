#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/lamepaste}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DB="$TMP/history.db"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

# Binary-safe input: NUL bytes, all byte values, and newlines survive file redirection.
python3 - "$TMP/input.bin" <<'PY'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(bytes(range(256)) * 16)
    f.write(b"\x00pipe-test\nend\xff")
PY
"$BIN" --no-clipboard --db "$DB" < "$TMP/input.bin" 2>"$TMP/file.err"
"$BIN" --show 1 --db "$DB" > "$TMP/file.out"
cmp "$TMP/input.bin" "$TMP/file.out" || fail 'file redirection did not preserve bytes'
pass 'file redirection preserves binary bytes'

# Pipeline uses the same stdin reader and must yield identical content.
cat "$TMP/input.bin" | "$BIN" --no-clipboard --db "$DB" 2>"$TMP/pipe.err"
"$BIN" --show 2 --db "$DB" > "$TMP/pipe.out"
cmp "$TMP/input.bin" "$TMP/pipe.out" || fail 'pipeline did not preserve bytes'
pass 'pipeline input preserves binary bytes'

# Empty input is a valid empty clipboard/history item.
: | "$BIN" --no-clipboard --db "$DB" 2>"$TMP/empty.err"
"$BIN" --show 3 --db "$DB" > "$TMP/empty.out"
[[ ! -s "$TMP/empty.out" ]] || fail 'empty input became nonempty'
pass 'empty input is handled safely'

# Restore is binary safe without requiring a desktop clipboard.
"$BIN" --restore 1 --no-clipboard --db "$DB" 2>"$TMP/restore.err"
"$BIN" --show 4 --db "$DB" > "$TMP/restore.out"
cmp "$TMP/input.bin" "$TMP/restore.out" || fail 'restore did not preserve bytes'
pass 'restore preserves binary bytes'

# A missing desktop display must be reported as a clipboard failure, while history remains saved.
set +e
printf 'clipboard-check' | env -u DISPLAY "$BIN" --db "$TMP/nodisplay.db" >"$TMP/nodisplay.out" 2>"$TMP/nodisplay.err"
rc=$?
set -e
[[ $rc -ne 0 ]] || fail 'clipboard copy without DISPLAY unexpectedly succeeded'
grep -Eq 'DISPLAY|display|libx11' "$TMP/nodisplay.err" || fail 'missing display error was not specific'
"$BIN" --show 1 --db "$TMP/nodisplay.db" > "$TMP/nodisplay.saved"
printf 'clipboard-check' > "$TMP/expected"
cmp "$TMP/expected" "$TMP/nodisplay.saved" || fail 'history was not retained after clipboard failure'
pass 'clipboard failures are explicit and saved history is retained'

# Exercise native X11 clipboard ownership directly. The large item forces INCR chunking.
if command -v xvfb-run >/dev/null 2>&1 && command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x11; then
    cc -O2 -Wall -Wextra -Wpedantic -std=c11 \
        $(pkg-config --cflags x11) "$ROOT/tests/x11_clipboard_reader.c" \
        -o "$TMP/x11_clipboard_reader" $(pkg-config --libs x11)
    python3 - "$TMP/clipboard-large.bin" <<'PY'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(bytes(range(256)) * 8192)
PY
    mkdir -p "$TMP/no-tools"
    export TEST_BIN="$BIN" TEST_READER="$TMP/x11_clipboard_reader" TEST_TMP="$TMP"
    xvfb-run -a bash -euo pipefail -c '
        BIN="$1"; READER="$2"; TMPDIR_TEST="$3"
        printf "small clipboard value" | env PATH="$TMPDIR_TEST/no-tools" "$BIN" --db "$TMPDIR_TEST/x11-small.db" 2>"$TMPDIR_TEST/x11-small.log"
        "$READER" "$TMPDIR_TEST/x11-small.out"
        printf "small clipboard value" > "$TMPDIR_TEST/x11-small.expected"
        cmp "$TMPDIR_TEST/x11-small.expected" "$TMPDIR_TEST/x11-small.out"
        : | env PATH="$TMPDIR_TEST/no-tools" "$BIN" --db "$TMPDIR_TEST/x11-empty.db" 2>"$TMPDIR_TEST/x11-empty.log"
        "$READER" "$TMPDIR_TEST/x11-empty.out"
        test ! -s "$TMPDIR_TEST/x11-empty.out"
        cat "$TMPDIR_TEST/clipboard-large.bin" | env PATH="$TMPDIR_TEST/no-tools" "$BIN" --db "$TMPDIR_TEST/x11-large.db" 2>"$TMPDIR_TEST/x11-large.log"
        "$READER" "$TMPDIR_TEST/x11-large.out"
        cmp "$TMPDIR_TEST/clipboard-large.bin" "$TMPDIR_TEST/x11-large.out"
    ' _ "$BIN" "$TMP/x11_clipboard_reader" "$TMP"
    pass 'native X11 clipboard ownership and large INCR transfers work without clipboard helper programs'
else
    printf 'SKIP: Xvfb unavailable; native clipboard integration test not run.\n'
fi

"$BIN" --help >/dev/null
"$BIN" --version | grep -q 'Buddy 01100010 01110101 01100100 01100100 01111001'
pass 'help and version commands work'

printf '\nAll LamePaste tests passed.\n'
