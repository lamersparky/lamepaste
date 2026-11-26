# Changelog

## 2

- Rewrote the application in C++20.
- Replaced full-input memory buffering with a private, unlinked temporary spool file.
- Added incremental SQLite BLOB storage, history recovery, and streaming output.
- Preserved existing SQLite history databases and the native X11/XWayland clipboard backend.
- Retained large X11 INCR clipboard transfers while serving data from the spool file.
- Stopped the progress thread before forking the clipboard owner to avoid unsafe multi-threaded fork behavior.
- Hardened argument parsing, integer validation, file handling, SQLite transactions, and error reporting.
- Made `--version` a strict two-line output: `LamePaste 2`, followed by the binary mark.
- Added large-BLOB restore, exact version output, and C++ clipboard-reader coverage.

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
