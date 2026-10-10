# lamepaste

A terminal utility that captures piped input, saves it to a
SQLite history database with a local date/time stamp, and copies
the original input bytes to the system clipboard.

## Usage

Pipe command output into `lamepaste`:

    uname -a | lamepaste
    printf 'Hello, clipboard!\n' | lamepaste
    cat file.txt | lamepaste
    command 2>&1 | lamepaste

By default, standard output is captured. To include standard
error, redirect it with `2>&1`.

## Options

    lamepaste --help
    lamepaste --version
    lamepaste --history
    lamepaste --history 50
    lamepaste --show ID
    lamepaste --restore ID
    lamepaste --no-clipboard
    lamepaste --db /path/to/history.db

The default database is:

    ~/.local/share/lamepaste/lamepaste.db

The `LAMEPASTE_DB` environment variable or `--db PATH` can
select a different database.

## Building

### Dependencies

- A C compiler (such as GCC)
- SQLite3 development headers and library
- A clipboard utility: `xclip` or `xsel` for X11, or `wl-copy` for Wayland

On Ubuntu/Debian, install the build dependencies with:

    sudo apt install build-essential libsqlite3-dev

Install a clipboard utility if your desktop does not already provide one.

### Build

    make

Run the program:

    ./lamepaste --help
    ./lamepaste --version

Install system-wide (optional):

    sudo make install

Remove the installed executable:

    sudo make uninstall

The default installation prefix is `/usr/local`. Override it with
`make PREFIX=/your/prefix install`.

## License

This project is distributed under the MIT License. See LICENSE.
