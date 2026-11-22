# Changelog

## 1.2.1

- Removed the retired external transfer integration completely, including implementation, option parsing, progress stage, tests, and documentation.
- Preserved clipboard capture, SQLite history, restore, and terminal progress behavior.

## 1.2.0

- Added a compact animated terminal progress indicator for interactive operations.
- Shows live input byte counts and current processing phase without fabricating percentages.
- Keeps progress on stderr and suppresses it for non-interactive output.

## 1.1.0

- Replaced `wl-copy`, `xclip`, and `xsel` subprocess clipboard handling with direct Xlib selection ownership.
- Added background clipboard-owner lifetime management and X11 INCR streaming for large payloads.
- Hardened SQLite binding checks, process startup handling, database close diagnostics, and stdout errors.
- Added regression and Xvfb clipboard integration tests.

## 1.0.0

- Original SQLite-backed stdin capture, clipboard helper dispatch, history, show, and restore functionality.
