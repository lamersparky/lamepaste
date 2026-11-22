# LamePaste 1.2.1

LamePaste is a binary-safe clipboard and history utility. It reads standard input, stores the exact bytes in a private SQLite history database, and places the content on the desktop clipboard.

## What changed in 1.2.1

- Removed the retired external transfer integration and its command-line option, implementation, tests, and documentation.
- Retained the compact terminal progress indicator and native clipboard/history behavior.

## What changed in 1.2.0

- Added a compact, transient Rich-inspired terminal activity indicator for interactive terminals.
- Reports the actual number of bytes read while receiving input; it deliberately does not invent a percentage when the input size is unknown. For regular-file input, it can calculate a real byte-based percentage and render a compact Unicode bar.
- Progress is written only to stderr, is suppressed for redirected/non-interactive stderr, and is cleared when the operation finishes. `NO_COLOR` disables the colored display.

## Clipboard backend and Wayland note

LamePaste talks directly to the X11 clipboard selection using **Xlib**; it does not launch any clipboard helper executable. In a typical KDE Plasma Wayland session, XWayland bridges this X11 clipboard selection to the Wayland desktop. This requires `DISPLAY` to be set and XWayland to be enabled.

This release does **not** yet implement the Wayland data-control protocol directly. A Wayland-only session with no XWayland `DISPLAY` will report a clear error instead of silently falling back to an external command. The build-time X11 library dependency is `libX11`; it is not a clipboard helper process.

## Build on Ubuntu

```sh
sudo apt install build-essential pkg-config libsqlite3-dev libx11-dev xvfb
make
make test
```

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
```

## Storage and data handling

- Default database: `~/.local/share/lamepaste/lamepaste.db`.
- Override with `LAMEPASTE_DB=/path/to/history.db` or `--db /path/to/history.db`.
- SQLite payloads are stored as BLOBs, preserving NUL bytes and newlines.
- Database files are restricted to owner read/write permissions (`0600`); newly created directories use restrictive permissions.
- The database is **not encrypted**. Treat it as sensitive data.
- If clipboard placement fails, the history entry remains saved and LamePaste returns a nonzero status with the failing stage in its diagnostic.
- X11 clipboard ownership requires a small background LamePaste owner process. It serves the clipboard data until another application takes ownership or the display session ends.

## Exit status

- `0`: operation completed successfully.
- `1`: runtime, database, or clipboard failure.
- `2`: invalid arguments or interactive terminal input without a pipe/redirection.

---

Buddy binary mark: `01100010 01110101 01100100 01100100 01111001`
