#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/lamepaste}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DB="$TMP/history.db"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

# Version output is a strict, stable two-line interface with no extra text.
printf 'LamePaste 2\n01100001 01110101 01100100 01110010 01100101 01111001\n' > "$TMP/version.expected"
"$BIN" --version > "$TMP/version.actual" 2>"$TMP/version.err"
cmp "$TMP/version.expected" "$TMP/version.actual" || fail '--version output is not exact'
[[ ! -s "$TMP/version.err" ]] || fail '--version wrote unexpected stderr output'
pass '--version prints only the version identity and binary mark'

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
    c++ -O2 -Wall -Wextra -Wpedantic -std=c++20 \
        $(pkg-config --cflags x11) "$ROOT/tests/x11_clipboard_reader.cpp" \
        -o "$TMP/x11_clipboard_reader" $(pkg-config --libs x11)
    python3 - "$TMP/clipboard-large.bin" <<'PY'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(bytes(range(256)) * 12288)
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
pass 'help command works'

set +e
"$BIN" --history 0 --db "$TMP/invalid.db" >/dev/null 2>"$TMP/invalid.err"
invalid_rc=$?
set -e
[[ $invalid_rc -eq 2 ]] || fail 'invalid history limits must return exit status 2'
pass 'invalid arguments are rejected cleanly'

# Opening an older 1.x database must preserve its rows and append new entries.
python3 - "$TMP/legacy.db" <<'PY'
import sqlite3
import sys
connection = sqlite3.connect(sys.argv[1])
connection.executescript("""
CREATE TABLE clips (id INTEGER PRIMARY KEY AUTOINCREMENT, copied_at TEXT NOT NULL,
                    byte_count INTEGER NOT NULL CHECK(byte_count >= 0), payload BLOB NOT NULL);
CREATE INDEX clips_copied_at_idx ON clips(copied_at);
PRAGMA user_version = 1;
""")
payload = b'legacy\x00payload\xff'
connection.execute('INSERT INTO clips(copied_at, byte_count, payload) VALUES(?, ?, ?)',
                   ('2026-01-02 03:04:05 +0000', len(payload), payload))
connection.commit()
connection.close()
PY
"$BIN" --show 1 --db "$TMP/legacy.db" > "$TMP/legacy.out"
printf 'legacy\0payload\377' > "$TMP/legacy.expected"
cmp "$TMP/legacy.expected" "$TMP/legacy.out" || fail 'existing history database was not preserved'
printf 'new-entry' | "$BIN" --no-clipboard --db "$TMP/legacy.db" 2>"$TMP/legacy-write.err"
"$BIN" --show 2 --db "$TMP/legacy.db" > "$TMP/legacy-new.out"
printf 'new-entry' > "$TMP/legacy-new.expected"
cmp "$TMP/legacy-new.expected" "$TMP/legacy-new.out" || fail 'could not append to an existing history database'
pass 'existing 1.x SQLite history databases remain compatible'

# Concurrent writers should serialize cleanly through SQLite's busy timeout.
for index in 1 2 3 4 5 6 7 8; do
    (printf 'parallel-%02d' "$index" | "$BIN" --no-clipboard --db "$TMP/concurrent.db" >/dev/null 2>>"$TMP/concurrent.err") &
done
wait
concurrent_count=$("$BIN" --history 20 --db "$TMP/concurrent.db" | tail -n +2 | wc -l)
[[ "$concurrent_count" -eq 8 ]] || fail 'concurrent writes lost one or more history entries'
pass 'concurrent writers serialize without losing history entries'

# Version inspection must not initialize a database, even with a custom DB path.
env LAMEPASTE_DB="$TMP/version-must-not-exist.db" "$BIN" --version >/dev/null
[[ ! -e "$TMP/version-must-not-exist.db" ]] || fail '--version unexpectedly created a database'
pass '--version does not touch SQLite or create files'

printf '\nAll LamePaste tests passed.\n'
