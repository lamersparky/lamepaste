# LamePaste 2

LamePaste is a binary-safe clipboard and history utility written in modern C++20. It reads standard input, stores the exact bytes and a local timestamp in a private SQLite history database, and places the content on the desktop clipboard.

## Version identification

`lamepaste --version` writes exactly these two lines, followed by a newline:

```text
LamePaste 2
01100001 01110101 01100100 01110010 01100101 01111001
```

No database is opened or created when requesting the version.

## Version 2 changes

- Replaced the C implementation with C++20, clearer ownership, RAII resource cleanup, and centralized error handling.
- Replaced whole-input RAM buffering with an unlinked, owner-only temporary spool file. Large pipelines and file inputs no longer require the entire payload to reside in memory.
- Added SQLite incremental BLOB reads and writes. Existing `clips` history databases from earlier releases remain compatible.
- Kept binary-safe input and output, history listing, exact entry recovery, restore, and native X11/XWayland clipboard ownership.
- Retained X11 INCR chunk transfer for payloads too large for one X11 property request.
- Stopped the progress-rendering thread before forking the clipboard owner, avoiding a fork while another thread may hold a C/C++ runtime lock.
- Improved CLI validation, integer parsing, temporary-file handling, write/read loops, and diagnostics.
- Reduced history-listing work by fetching only the preview bytes instead of loading each full BLOB.

## Clipboard backend and Wayland note

LamePaste talks directly to the X11 clipboard selection using **Xlib**; it does not launch `wl-copy`, `xclip`, or `xsel`. In a typical KDE Plasma Wayland session, XWayland bridges this X11 clipboard selection to the Wayland desktop. This requires `DISPLAY` to be set and XWayland to be enabled.

This release does **not** implement the Wayland data-control protocol directly. A Wayland-only session with no XWayland `DISPLAY` reports a clear error instead of silently falling back to an external command. The clipboard owner is a small detached process that retains the private temporary spool descriptor and serves clipboard requests until another application takes ownership or the display session ends.

## Size and resource limits

Input is streamed to a private, unlinked temporary file, so input size does not translate into an equally large RAM allocation. The maximum stored BLOB is still bounded by the SQLite library's configured `SQLITE_LIMIT_LENGTH` and the incremental-BLOB API's integer offset range. The X11 clipboard INCR protocol also has a 32-bit transfer-size field. LamePaste checks these limits and reports a useful error rather than silently truncating input. Available temporary storage and database filesystem capacity remain practical limits.

The SQLite database is not encrypted. Treat it as sensitive data. Temporary spool files are created with owner-only permissions and unlinked immediately after creation.

## Build on Ubuntu

```sh
sudo apt install build-essential pkg-config libsqlite3-dev libx11-dev xvfb
make
make test
```

C++20, SQLite, and pthread support are required. X11 support is enabled automatically when `pkg-config` finds `x11`; without it, the program still builds and saves history, but clipboard operations report that native X11 support is unavailable.

The native clipboard integration test uses Xvfb when available. If `xvfb-run` is not installed, the test suite reports that the GUI integration test was skipped while still running the non-GUI tests.

Install for the current user:

```sh
install -Dm755 lamepaste "$HOME/.local/bin/lamepaste"
```

Ensure `~/.local/bin` is on `PATH`.

## Usage

```sh
printf 'Hello from LamePaste\n' | lamepaste
cat session.log | lamepaste
lamepaste < session.log
ip addr | lamepaste
journalctl -b -p warning | lamepaste

# Save without touching the desktop clipboard
journalctl -b | lamepaste --no-clipboard

# History and recovery
lamepaste --history
lamepaste --history 50
lamepaste --show 3 > recovered.bin
lamepaste --restore 3

# Version identity
lamepaste --version
```

## Storage and data handling

- Default database: `~/.local/share/lamepaste/lamepaste.db`.
- Override the default with `LAMEPASTE_DB=/path/to/history.db` or `--db /path/to/history.db`.
- SQLite payloads are stored as BLOBs, preserving NUL bytes, arbitrary byte values, and newlines.
- Database files are restricted to owner read/write permissions (`0600`); newly created directories use the process's restrictive `umask` (`0077`).
- The database is **not encrypted**.
- If clipboard placement fails, the history entry remains saved and LamePaste returns a nonzero status with the failing stage in its diagnostic.
- The background X11 clipboard owner retains the anonymous spool file descriptor after the main process exits, allowing delayed clipboard requests without keeping a second full copy in RAM.

## Exit status

- `0`: operation completed successfully.
- `1`: runtime, database, or clipboard failure.
- `2`: invalid arguments or interactive terminal input without a pipe/redirection.
