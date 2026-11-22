#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

#define APP_NAME "lamepaste"
#define APP_VERSION "1.2.1"
#define DEFAULT_HISTORY_LIMIT 20
#define MAX_HISTORY_LIMIT 100000
#define INPUT_CHUNK_SIZE 16384
#define PREVIEW_BYTES 72

static const char *g_db_path = NULL;

/* A small Rich-inspired transient progress renderer. It writes only to stderr,
 * activates only for an interactive terminal, and never estimates completion
 * for work whose total is unknown. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    int active;
    int running;
    int color;
    size_t bytes;
    size_t total;
    char phase[96];
} Progress;

static Progress g_progress = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, 0, 0, "" };
static const char *const progress_frames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

static void *progress_worker(void *unused)
{
    size_t frame = 0;
    (void)unused;
    for (;;) {
        char phase[sizeof(g_progress.phase)];
        size_t bytes, total;
        int running, color;
        struct timespec delay = {0, 120000000L};
        (void)pthread_mutex_lock(&g_progress.mutex);
        running = g_progress.running;
        color = g_progress.color;
        bytes = g_progress.bytes;
        total = g_progress.total;
        (void)snprintf(phase, sizeof(phase), "%s", g_progress.phase);
        (void)pthread_mutex_unlock(&g_progress.mutex);
        if (!running)
            break;
        if (g_progress.active) {
            if (total != 0) {
                size_t done = bytes < total ? bytes : total;
                size_t filled = (done * 18U) / total;
                size_t percent = (done * 100U) / total;
                size_t j;
                fprintf(stderr, "\r\033[2K%s%s%s  %-18s  ", color ? "\033[36m" : "", progress_frames[frame], color ? "\033[0m" : "", phase);
                for (j = 0; j < 18; ++j)
                    fputs(j < filled ? "━" : "─", stderr);
                fprintf(stderr, "  %3zu%%  %zu/%zu bytes", percent, done, total);
            } else if (bytes != 0) {
                fprintf(stderr, "\r\033[2K%s%s%s  %-22s  %zu bytes", color ? "\033[36m" : "", progress_frames[frame], color ? "\033[0m" : "", phase, bytes);
            } else {
                fprintf(stderr, "\r\033[2K%s%s%s  %s", color ? "\033[36m" : "", progress_frames[frame], color ? "\033[0m" : "", phase);
            }
            fflush(stderr);
            frame = (frame + 1) % (sizeof(progress_frames) / sizeof(progress_frames[0]));
        }
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

static void progress_start(const char *phase)
{
    if (!isatty(STDERR_FILENO))
        return;
    (void)pthread_mutex_lock(&g_progress.mutex);
    g_progress.active = 1;
    g_progress.running = 1;
    g_progress.color = getenv("NO_COLOR") == NULL;
    g_progress.bytes = 0;
    g_progress.total = 0;
    (void)snprintf(g_progress.phase, sizeof(g_progress.phase), "%s", phase);
    (void)pthread_mutex_unlock(&g_progress.mutex);
    if (pthread_create(&g_progress.thread, NULL, progress_worker, NULL) != 0) {
        (void)pthread_mutex_lock(&g_progress.mutex);
        g_progress.active = 0;
        g_progress.running = 0;
        (void)pthread_mutex_unlock(&g_progress.mutex);
    }
}

static void progress_phase(const char *phase)
{
    if (!g_progress.active)
        return;
    (void)pthread_mutex_lock(&g_progress.mutex);
    g_progress.bytes = 0;
    g_progress.total = 0;
    (void)snprintf(g_progress.phase, sizeof(g_progress.phase), "%s", phase);
    (void)pthread_mutex_unlock(&g_progress.mutex);
}

static void progress_total(size_t total)
{
    if (!g_progress.active)
        return;
    (void)pthread_mutex_lock(&g_progress.mutex);
    g_progress.total = total;
    (void)pthread_mutex_unlock(&g_progress.mutex);
}

static void progress_bytes(size_t bytes)
{
    if (!g_progress.active)
        return;
    (void)pthread_mutex_lock(&g_progress.mutex);
    g_progress.bytes = bytes;
    (void)pthread_mutex_unlock(&g_progress.mutex);
}

static void progress_stop(void)
{
    int active;
    (void)pthread_mutex_lock(&g_progress.mutex);
    active = g_progress.active;
    g_progress.running = 0;
    (void)pthread_mutex_unlock(&g_progress.mutex);
    if (active) {
        (void)pthread_join(g_progress.thread, NULL);
        fputs("\r\033[2K", stderr);
        fflush(stderr);
    }
    (void)pthread_mutex_lock(&g_progress.mutex);
    g_progress.active = 0;
    (void)pthread_mutex_unlock(&g_progress.mutex);
}

typedef enum {
    ACTION_PIPE = 0,
    ACTION_HISTORY,
    ACTION_SHOW,
    ACTION_RESTORE,
    ACTION_HELP,
    ACTION_VERSION
} Action;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buffer;

static void print_usage(FILE *out)
{
    fprintf(out,
        "01101100 01100001 01101101 01100101 01110010\n"
        "LAMERPASTE " APP_VERSION "\n"
        "Usage:\n"
        "  command | " APP_NAME " [--db PATH] [--no-clipboard]\n"
        "  " APP_NAME " [--db PATH] --history [N]\n"
        "  " APP_NAME " [--db PATH] --show ID\n"
        "  " APP_NAME " [--db PATH] --restore ID [--no-clipboard]\n"
        "\n"
        "With piped or redirected input, " APP_NAME " saves the exact bytes and local\n"
        "date/time to a private SQLite history database, then copies the input\n"
        "to the system clipboard.\n"
        "\n"
        "Options:\n"
        "  -d, --db PATH       Use a different SQLite database file\n"
        "      --no-clipboard  Save/restore history without using the clipboard\n"
        "  -H, --history [N]   List the newest N entries (default: %d)\n"
        "      --show ID       Write an entry's original bytes to stdout\n"
        "      --restore ID    Copy an entry back to the clipboard and log it\n"
        "  -h, --help          Show this help\n"
        "      --version       Show version\n"
        "\n"
        "Database default: ~/.local/share/lamepaste/lamepaste.db\n"
        "Override the default with LAMEPASTE_DB or --db PATH.\n",
        DEFAULT_HISTORY_LIMIT);
}

static int parse_positive_id(const char *s, sqlite3_int64 *out)
{
    char *end = NULL;
    long long value;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    value = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value <= 0)
        return -1;
    *out = (sqlite3_int64)value;
    return 0;
}

static int parse_history_limit(const char *s, int *out)
{
    char *end = NULL;
    long value;

    if (s == NULL || *s == '\0' || *s == '-')
        return -1;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < 1 || value > MAX_HISTORY_LIMIT)
        return -1;
    *out = (int)value;
    return 0;
}

static int buffer_append(Buffer *b, const unsigned char *data, size_t len)
{
    size_t needed;
    size_t new_cap;
    unsigned char *new_data;

    if (len == 0)
        return 0;
    if (b->len > SIZE_MAX - len) {
        errno = EOVERFLOW;
        return -1;
    }
    needed = b->len + len;
    if (needed > b->cap) {
        new_cap = b->cap ? b->cap : INPUT_CHUNK_SIZE;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = needed;
                break;
            }
            new_cap *= 2;
        }
        new_data = realloc(b->data, new_cap);
        if (new_data == NULL)
            return -1;
        b->data = new_data;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static int read_all_stdin(Buffer *b)
{
    unsigned char chunk[INPUT_CHUNK_SIZE];

    for (;;) {
        ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (buffer_append(b, chunk, (size_t)n) != 0)
            return -1;
        progress_bytes(b->len);
    }
}

static int mkdir_p(const char *path, mode_t mode)
{
    char *copy;
    char *p;
    size_t len;
    int rc = 0;

    if (path == NULL || *path == '\0') {
        errno = EINVAL;
        return -1;
    }
    copy = strdup(path);
    if (copy == NULL)
        return -1;
    len = strlen(copy);

    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';

    for (p = copy + (copy[0] == '/' ? 1 : 0); ; ++p) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (*copy != '\0' && mkdir(copy, mode) != 0 && errno != EEXIST) {
                rc = -1;
                *p = saved;
                break;
            }
            if (*copy != '\0') {
                struct stat st;
                if (stat(copy, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    if (errno == 0)
                        errno = ENOTDIR;
                    rc = -1;
                    *p = saved;
                    break;
                }
            }
            *p = saved;
            if (saved == '\0')
                break;
        }
    }
    free(copy);
    return rc;
}

static char *default_database_path(void)
{
    const char *home = getenv("HOME");
    struct passwd *pw = NULL;
    size_t needed;
    char *path;

    if (home == NULL || *home == '\0') {
        pw = getpwuid(getuid());
        if (pw != NULL)
            home = pw->pw_dir;
    }
    if (home == NULL || *home == '\0') {
        fprintf(stderr, APP_NAME ": cannot determine your home directory; set HOME or use --db PATH\n");
        return NULL;
    }

    needed = strlen(home) + sizeof("/.local/share/lamepaste/lamepaste.db");
    path = malloc(needed);
    if (path == NULL)
        return NULL;
    (void)snprintf(path, needed, "%s/.local/share/lamepaste/lamepaste.db", home);
    return path;
}

static int ensure_database_parent(const char *db_path)
{
    char *parent;
    char *slash;
    int rc;

    if (strcmp(db_path, ":memory:") == 0)
        return 0;
    parent = strdup(db_path);
    if (parent == NULL)
        return -1;

    slash = strrchr(parent, '/');
    if (slash == NULL) {
        free(parent);
        return 0;
    }
    if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';

    rc = mkdir_p(parent, 0700);
    free(parent);
    return rc;
}

static int execute_sql(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": SQLite error: %s\n", errmsg ? errmsg : sqlite3_errmsg(db));
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int open_database(const char *db_path, sqlite3 **out_db)
{
    sqlite3 *db = NULL;
    int rc;

    if (ensure_database_parent(db_path) != 0) {
        fprintf(stderr, APP_NAME ": cannot create database directory for '%s': %s\n",
                db_path, strerror(errno));
        return -1;
    }

    rc = sqlite3_open_v2(db_path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot open database '%s': %s\n",
                db_path, db ? sqlite3_errmsg(db) : "out of memory");
        if (db != NULL)
            sqlite3_close(db);
        return -1;
    }

    (void)sqlite3_busy_timeout(db, 5000);
    if (strcmp(db_path, ":memory:") != 0 && chmod(db_path, S_IRUSR | S_IWUSR) != 0) {
        fprintf(stderr, APP_NAME ": cannot restrict permissions on database '%s': %s\n",
                db_path, strerror(errno));
        sqlite3_close(db);
        return -1;
    }

    if (execute_sql(db,
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  copied_at TEXT NOT NULL,"
        "  byte_count INTEGER NOT NULL CHECK(byte_count >= 0),"
        "  payload BLOB NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS clips_copied_at_idx ON clips(copied_at);"
        "PRAGMA user_version = 1;" ) != 0) {
        sqlite3_close(db);
        return -1;
    }

    *out_db = db;
    return 0;
}

static int make_timestamp(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm local_tm;
    size_t n;

    if (now == (time_t)-1 || localtime_r(&now, &local_tm) == NULL)
        return -1;
    n = strftime(out, out_size, "%Y-%m-%d %H:%M:%S %z", &local_tm);
    return n == 0 ? -1 : 0;
}

static int insert_clip(sqlite3 *db, const unsigned char *data, size_t len, sqlite3_int64 *out_id)
{
    static const char sql[] =
        "INSERT INTO clips(copied_at, byte_count, payload) VALUES(?1, ?2, ?3);";
    sqlite3_stmt *stmt = NULL;
    char timestamp[64];
    int rc;

    if (len > (size_t)INT64_MAX) {
        fprintf(stderr, APP_NAME ": input is too large for SQLite\n");
        return -1;
    }
    if (make_timestamp(timestamp, sizeof(timestamp)) != 0) {
        fprintf(stderr, APP_NAME ": could not generate a date/time stamp\n");
        return -1;
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)len);
    if (rc == SQLITE_OK) {
        if (len == 0)
            rc = sqlite3_bind_zeroblob(stmt, 3, 0);
        else
            rc = sqlite3_bind_blob64(stmt, 3, data, (sqlite3_uint64)len, SQLITE_TRANSIENT);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": SQLite bind failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, APP_NAME ": cannot save clipboard entry: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    if (out_id != NULL)
        *out_id = sqlite3_last_insert_rowid(db);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": SQLite finalize failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

#ifdef HAVE_X11
static int read_all_fd(int fd, unsigned char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = read(fd, data + offset, len - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}
#endif

static int wait_for_child(pid_t pid, int *status)
{
    while (waitpid(pid, status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

#ifdef HAVE_X11
#define X11_TRANSFER_CHUNK (64U * 1024U)
#define X11_MAX_TRANSFERS 128U

typedef struct X11Transfer {
    Window requestor;
    Atom property;
    Atom target;
    size_t offset;
    size_t length;
    struct X11Transfer *next;
} X11Transfer;

typedef struct {
    int status;
    int error_number;
    char message[160];
} ClipboardStartup;

static int x11_error_seen;

static int x11_error_handler(Display *display, XErrorEvent *event)
{
    (void)display;
    x11_error_seen = event->error_code;
    return 0;
}

static void x11_send_notify(Display *display, XSelectionRequestEvent *request,
                            Atom property)
{
    XEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = request->display;
    reply.xselection.requestor = request->requestor;
    reply.xselection.selection = request->selection;
    reply.xselection.target = request->target;
    reply.xselection.property = property;
    reply.xselection.time = request->time;
    (void)XSendEvent(display, request->requestor, False, 0, &reply);
    (void)XFlush(display);
}

static size_t x11_max_property_bytes(Display *display)
{
    long units = XMaxRequestSize(display);
    size_t bytes;
    if (units <= 0)
        return X11_TRANSFER_CHUNK;
    bytes = (size_t)units * 4U;
    return bytes > 1024U ? bytes - 1024U : X11_TRANSFER_CHUNK;
}

static int x11_start_incr(Display *display, XSelectionRequestEvent *request,
                          Atom property, Atom incr_atom, Atom target,
                          size_t len, X11Transfer **transfers)
{
    X11Transfer *transfer;
    X11Transfer *scan;
    unsigned long total;
    size_t active = 0;

    for (scan = *transfers; scan != NULL; scan = scan->next)
        ++active;
    if (active >= X11_MAX_TRANSFERS) {
        x11_send_notify(display, request, None);
        return -1;
    }
    if (len > UINT32_MAX) {
        x11_send_notify(display, request, None);
        return -1;
    }
    transfer = calloc(1, sizeof(*transfer));
    if (transfer == NULL) {
        x11_send_notify(display, request, None);
        return -1;
    }
    transfer->requestor = request->requestor;
    transfer->property = property;
    transfer->target = target;
    transfer->length = len;
    transfer->next = *transfers;
    *transfers = transfer;

    total = (unsigned long)len;
    x11_error_seen = 0;
    XSelectInput(display, request->requestor, PropertyChangeMask | StructureNotifyMask);
    XChangeProperty(display, request->requestor, property, incr_atom, 32,
                    PropModeReplace, (unsigned char *)&total, 1);
    XSync(display, False);
    if (x11_error_seen != 0) {
        *transfers = transfer->next;
        free(transfer);
        x11_send_notify(display, request, None);
        return -1;
    }
    x11_send_notify(display, request, property);
    return 0;
}

static void x11_handle_selection_request(Display *display,
                                         XSelectionRequestEvent *request,
                                         Atom clipboard_atom, Atom targets_atom,
                                         Atom utf8_atom, Atom text_atom,
                                         Atom octet_atom, Atom incr_atom,
                                         const unsigned char *data, size_t len,
                                         X11Transfer **transfers)
{
    Atom property = request->property != None ? request->property : request->target;
    Atom offered[5];
    size_t max_bytes = x11_max_property_bytes(display);

    if (request->selection != clipboard_atom) {
        x11_send_notify(display, request, None);
        return;
    }

    if (request->target == targets_atom) {
        offered[0] = targets_atom;
        offered[1] = utf8_atom;
        offered[2] = XA_STRING;
        offered[3] = text_atom;
        offered[4] = octet_atom;
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)offered, 5);
        x11_send_notify(display, request, property);
        return;
    }

    if (request->target != utf8_atom && request->target != XA_STRING &&
        request->target != text_atom && request->target != octet_atom) {
        x11_send_notify(display, request, None);
        return;
    }

    if (len <= max_bytes && len <= (size_t)INT_MAX) {
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (const unsigned char *)data, (int)len);
        x11_send_notify(display, request, property);
        return;
    }

    (void)x11_start_incr(display, request, property, incr_atom, request->target,
                         len, transfers);
}

static void x11_handle_property_delete(Display *display, XPropertyEvent *event,
                                       const unsigned char *data,
                                       X11Transfer **transfers)
{
    X11Transfer **cursor = transfers;
    size_t chunk;

    while (*cursor != NULL) {
        X11Transfer *transfer = *cursor;
        if (transfer->requestor != event->window || transfer->property != event->atom) {
            cursor = &transfer->next;
            continue;
        }

        if (transfer->offset < transfer->length) {
            chunk = transfer->length - transfer->offset;
            size_t max_chunk = x11_max_property_bytes(display);
            if (max_chunk > X11_TRANSFER_CHUNK)
                max_chunk = X11_TRANSFER_CHUNK;
            if (chunk > max_chunk)
                chunk = max_chunk;
            XChangeProperty(display, transfer->requestor, transfer->property,
                            transfer->target, 8, PropModeReplace,
                            data + transfer->offset, (int)chunk);
            transfer->offset += chunk;
        } else {
            XChangeProperty(display, transfer->requestor, transfer->property,
                            transfer->target, 8, PropModeReplace,
                            (const unsigned char *)"", 0);
            *cursor = transfer->next;
            free(transfer);
        }
        XFlush(display);
        return;
    }
}

static void x11_clipboard_owner(const unsigned char *data, size_t len, int status_fd)
{
    Display *display;
    Window owner;
    Atom clipboard_atom, targets_atom, utf8_atom, text_atom, octet_atom, incr_atom;
    X11Transfer *transfers = NULL;
    ClipboardStartup startup;
    XEvent event;

    memset(&startup, 0, sizeof(startup));
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        startup.status = -1;
        startup.error_number = errno;
        (void)snprintf(startup.message, sizeof(startup.message),
                       "cannot connect to DISPLAY (X11/XWayland)");
        (void)write_all(status_fd, (const unsigned char *)&startup, sizeof(startup));
        _exit(1);
    }

    XSetErrorHandler(x11_error_handler);
    x11_error_seen = 0;
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    text_atom = XInternAtom(display, "TEXT", False);
    octet_atom = XInternAtom(display, "application/octet-stream", False);
    incr_atom = XInternAtom(display, "INCR", False);
    owner = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1,
                                0, 0, 0);
    x11_error_seen = 0;
    XSetSelectionOwner(display, clipboard_atom, owner, CurrentTime);
    XSync(display, False);
    if (x11_error_seen != 0 || XGetSelectionOwner(display, clipboard_atom) != owner) {
        startup.status = -1;
        (void)snprintf(startup.message, sizeof(startup.message),
                       "X server refused clipboard ownership");
        (void)write_all(status_fd, (const unsigned char *)&startup, sizeof(startup));
        XDestroyWindow(display, owner);
        XCloseDisplay(display);
        _exit(1);
    }

    startup.status = 0;
    (void)write_all(status_fd, (const unsigned char *)&startup, sizeof(startup));
    close(status_fd);

    for (;;) {
        XNextEvent(display, &event);
        if (event.type == SelectionRequest) {
            x11_handle_selection_request(display, &event.xselectionrequest,
                clipboard_atom, targets_atom, utf8_atom, text_atom, octet_atom,
                incr_atom, data, len, &transfers);
        } else if (event.type == PropertyNotify &&
                   event.xproperty.state == PropertyDelete) {
            x11_handle_property_delete(display, &event.xproperty, data, &transfers);
        } else if (event.type == SelectionClear &&
                   event.xselectionclear.selection == clipboard_atom) {
            break;
        } else if (event.type == DestroyNotify) {
            X11Transfer **cursor = &transfers;
            while (*cursor != NULL) {
                if ((*cursor)->requestor == event.xdestroywindow.window) {
                    X11Transfer *dead = *cursor;
                    *cursor = dead->next;
                    free(dead);
                } else {
                    cursor = &(*cursor)->next;
                }
            }
        }
    }

    while (transfers != NULL) {
        X11Transfer *next = transfers->next;
        free(transfers);
        transfers = next;
    }
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    _exit(0);
}

static void close_fds_except(int keep_fd)
{
    long max_fd = sysconf(_SC_OPEN_MAX);
    int fd;
    if (max_fd < 0 || max_fd > 65536)
        max_fd = 65536;
    for (fd = 0; fd < max_fd; ++fd) {
        if (fd != keep_fd)
            close(fd);
    }
}

static int copy_to_clipboard(const unsigned char *data, size_t len,
                             char *why, size_t why_size)
{
    int status_pipe[2] = {-1, -1};
    pid_t first_child;
    ClipboardStartup startup;

    if (getenv("DISPLAY") == NULL || *getenv("DISPLAY") == '\0') {
        if (getenv("WAYLAND_DISPLAY") != NULL && *getenv("WAYLAND_DISPLAY") != '\0') {
            (void)snprintf(why, why_size,
                "Wayland session detected, but DISPLAY is unavailable; this build uses native X11/XWayland access and needs XWayland enabled");
        } else {
            (void)snprintf(why, why_size,
                "no X11 display is available; this build uses native X11/XWayland clipboard access");
        }
        return -1;
    }
    if (len > UINT32_MAX) {
        (void)snprintf(why, why_size,
            "clipboard payload exceeds the X11 INCR protocol size limit");
        return -1;
    }
    if (pipe(status_pipe) != 0) {
        (void)snprintf(why, why_size, "clipboard status pipe failed: %s", strerror(errno));
        return -1;
    }
    first_child = fork();
    if (first_child < 0) {
        int saved = errno;
        close(status_pipe[0]); close(status_pipe[1]);
        (void)snprintf(why, why_size, "clipboard owner fork failed: %s", strerror(saved));
        return -1;
    }
    if (first_child == 0) {
        pid_t daemon_child;
        int null_fd;
        close(status_pipe[0]);
        if (setsid() < 0) {
            memset(&startup, 0, sizeof(startup));
            startup.status = -1;
            startup.error_number = errno;
            (void)snprintf(startup.message, sizeof(startup.message), "setsid failed: %s", strerror(errno));
            (void)write_all(status_pipe[1], (const unsigned char *)&startup, sizeof(startup));
            _exit(1);
        }
        daemon_child = fork();
        if (daemon_child < 0) {
            memset(&startup, 0, sizeof(startup));
            startup.status = -1;
            startup.error_number = errno;
            (void)snprintf(startup.message, sizeof(startup.message), "clipboard daemon fork failed: %s", strerror(errno));
            (void)write_all(status_pipe[1], (const unsigned char *)&startup, sizeof(startup));
            _exit(1);
        }
        if (daemon_child > 0) {
            close(status_pipe[1]);
            _exit(0);
        }
        close_fds_except(status_pipe[1]);
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        x11_clipboard_owner(data, len, status_pipe[1]);
        _exit(1);
    }

    close(status_pipe[1]);
    if (read_all_fd(status_pipe[0], (unsigned char *)&startup, sizeof(startup)) != 0) {
        int saved = errno;
        int child_status = 0;
        close(status_pipe[0]);
        (void)wait_for_child(first_child, &child_status);
        (void)snprintf(why, why_size,
            "clipboard owner exited before confirming startup: %s", strerror(saved));
        return -1;
    }
    close(status_pipe[0]);
    (void)wait_for_child(first_child, &(int){0});
    if (startup.status != 0) {
        (void)snprintf(why, why_size, "%s", startup.message[0] ? startup.message :
                       "native clipboard owner failed to start");
        return -1;
    }
    return 0;
}
#else
static int copy_to_clipboard(const unsigned char *data, size_t len,
                             char *why, size_t why_size)
{
    (void)data;
    (void)len;
    (void)snprintf(why, why_size,
        "lamepaste was built without native X11 support; install libx11-dev and rebuild");
    return -1;
}
#endif

static void print_preview(const unsigned char *data, size_t len)
{
    size_t limit = len < PREVIEW_BYTES ? len : PREVIEW_BYTES;
    size_t i;

    for (i = 0; i < limit; ++i) {
        unsigned char c = data[i];
        if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\0') fputs("\\0", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c >= 0x20 && c <= 0x7e) fputc(c, stdout);
        else printf("\\x%02x", (unsigned int)c);
    }
    if (len > limit)
        fputs("...", stdout);
}

static int show_history(sqlite3 *db, int limit)
{
    static const char sql[] =
        "SELECT id, copied_at, byte_count, payload FROM clips ORDER BY id DESC LIMIT ?1;";
    sqlite3_stmt *stmt = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot query history: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int(stmt, 1, limit);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot bind history limit: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    puts("ID\tDATE/TIME\tBYTES\tPREVIEW");
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        const unsigned char *when = sqlite3_column_text(stmt, 1);
        sqlite3_int64 byte_count = sqlite3_column_int64(stmt, 2);
        const unsigned char *payload = sqlite3_column_blob(stmt, 3);
        int payload_len = sqlite3_column_bytes(stmt, 3);

        printf("%lld\t%s\t%lld\t", (long long)id,
               when ? (const char *)when : "(unknown)", (long long)byte_count);
        print_preview(payload, payload_len < 0 ? 0 : (size_t)payload_len);
        fputc('\n', stdout);
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, APP_NAME ": history query failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": history query finalize failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return ferror(stdout) ? -1 : 0;
}

static int show_clip(sqlite3 *db, sqlite3_int64 id)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db, "SELECT payload FROM clips WHERE id = ?1;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot prepare clip lookup: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot bind history ID: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE)
            fprintf(stderr, APP_NAME ": no history entry with ID %lld\n", (long long)id);
        else
            fprintf(stderr, APP_NAME ": cannot read history entry: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        int failed = 0;
        if (bytes > 0 && fwrite(blob, 1, (size_t)bytes, stdout) != (size_t)bytes)
            failed = 1;
        rc = sqlite3_finalize(stmt);
        if (failed || ferror(stdout)) {
            fprintf(stderr, APP_NAME ": writing entry to stdout failed\n");
            return -1;
        }
        if (rc != SQLITE_OK) {
            fprintf(stderr, APP_NAME ": SQLite finalize failed: %s\n", sqlite3_errmsg(db));
            return -1;
        }
    }
    return 0;
}

static int load_clip(sqlite3 *db, sqlite3_int64 id, Buffer *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db, "SELECT payload FROM clips WHERE id = ?1;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot prepare clip lookup: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": cannot bind history ID: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE)
            fprintf(stderr, APP_NAME ": no history entry with ID %lld\n", (long long)id);
        else
            fprintf(stderr, APP_NAME ": cannot read history entry: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }
    {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        unsigned char *copy = NULL;
        if (bytes < 0) {
            fprintf(stderr, APP_NAME ": invalid payload length in database\n");
            sqlite3_finalize(stmt);
            return -1;
        }
        if (bytes > 0) {
            if (blob == NULL) {
                fprintf(stderr, APP_NAME ": database returned a NULL payload with a nonzero size\n");
                sqlite3_finalize(stmt);
                return -1;
            }
            copy = malloc((size_t)bytes);
            if (copy == NULL) {
                fprintf(stderr, APP_NAME ": not enough memory to restore entry\n");
                sqlite3_finalize(stmt);
                return -1;
            }
            memcpy(copy, blob, (size_t)bytes);
        }
        out->data = copy;
        out->len = (size_t)bytes;
        out->cap = (size_t)bytes;
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, APP_NAME ": SQLite finalize failed: %s\n", sqlite3_errmsg(db));
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Action action = ACTION_PIPE;
    const char *db_arg = NULL;
    int no_clipboard = 0;
    int history_limit = DEFAULT_HISTORY_LIMIT;
    sqlite3_int64 clip_id = 0;
    int action_seen = 0;
    int i;
    char *owned_default_path = NULL;
    const char *env_db;
    sqlite3 *db = NULL;
    Buffer input = {0};
    int result = 1;
    sqlite3_int64 saved_id = 0;

    /* Protect newly created database files and directories by default. */
    (void)umask(0077);

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            action = ACTION_HELP;
            action_seen = 1;
        } else if (strcmp(arg, "--version") == 0) {
            action = ACTION_VERSION;
            action_seen = 1;
        } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--db") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, APP_NAME ": --db requires a path\n");
                return 2;
            }
            db_arg = argv[++i];
        } else if (strcmp(arg, "--no-clipboard") == 0) {
            no_clipboard = 1;
        } else if (strcmp(arg, "-H") == 0 || strcmp(arg, "--history") == 0) {
            if (action_seen) {
                fprintf(stderr, APP_NAME ": specify only one action\n");
                return 2;
            }
            action = ACTION_HISTORY;
            action_seen = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (parse_history_limit(argv[i + 1], &history_limit) != 0) {
                    fprintf(stderr, APP_NAME ": history limit must be between 1 and %d\n", MAX_HISTORY_LIMIT);
                    return 2;
                }
                ++i;
            }
        } else if (strcmp(arg, "--show") == 0 || strcmp(arg, "--restore") == 0) {
            if (action_seen || i + 1 >= argc) {
                fprintf(stderr, APP_NAME ": %s requires exactly one ID and only one action may be used\n", arg);
                return 2;
            }
            action = strcmp(arg, "--show") == 0 ? ACTION_SHOW : ACTION_RESTORE;
            action_seen = 1;
            if (parse_positive_id(argv[++i], &clip_id) != 0) {
                fprintf(stderr, APP_NAME ": ID must be a positive integer\n");
                return 2;
            }
        } else {
            fprintf(stderr, APP_NAME ": unknown option or argument: %s\n", arg);
            print_usage(stderr);
            return 2;
        }
    }

    if (action == ACTION_HELP) {
        print_usage(stdout);
        return 0;
    }
    if (action == ACTION_VERSION) {
        puts(APP_NAME " " APP_VERSION " ---- Lamer 01101100 01100001 01101101 01100101 01110010 | Buddy 01100010 01110101 01100100 01100100 01111001");
        return 0;
    }

    env_db = getenv("LAMEPASTE_DB");
    if (db_arg != NULL) {
        g_db_path = db_arg;
    } else if (env_db != NULL && *env_db != '\0') {
        g_db_path = env_db;
    } else {
        owned_default_path = default_database_path();
        if (owned_default_path == NULL) {
            fprintf(stderr, APP_NAME ": failed to determine database path\n");
            return 1;
        }
        g_db_path = owned_default_path;
    }

    if (open_database(g_db_path, &db) != 0)
        goto cleanup;

    if (action == ACTION_HISTORY) {
        result = show_history(db, history_limit) == 0 ? 0 : 1;
        goto cleanup;
    }
    if (action == ACTION_SHOW) {
        result = show_clip(db, clip_id) == 0 ? 0 : 1;
        goto cleanup;
    }

    progress_start(action == ACTION_PIPE ? "Reading input" : "Restoring history entry");
    if (action == ACTION_PIPE) {
        struct stat input_stat;
        if (fstat(STDIN_FILENO, &input_stat) == 0 && S_ISREG(input_stat.st_mode) &&
            input_stat.st_size > 0 && (uintmax_t)input_stat.st_size <= SIZE_MAX)
            progress_total((size_t)input_stat.st_size);
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, APP_NAME ": expected piped or redirected input; example: printf 'hello' | " APP_NAME "\n");
            result = 2;
            goto cleanup;
        }
        if (read_all_stdin(&input) != 0) {
            fprintf(stderr, APP_NAME ": reading stdin failed: %s\n", strerror(errno));
            goto cleanup;
        }
    } else if (action == ACTION_RESTORE) {
        if (load_clip(db, clip_id, &input) != 0)
            goto cleanup;
    }

    progress_phase("Saving to SQLite");
    if (insert_clip(db, input.data, input.len, &saved_id) != 0)
        goto cleanup;

    if (!no_clipboard) {
        char why[512];
        progress_phase("Copying to clipboard");
        if (copy_to_clipboard(input.data, input.len, why, sizeof(why)) != 0) {
            fprintf(stderr,
                APP_NAME ": saved entry #%lld (%zu bytes) to history, but native clipboard copy failed: %s\n",
                (long long)saved_id, input.len, why);
            goto cleanup;
        }
    }

    progress_stop();
    if (no_clipboard) {
        fprintf(stderr, APP_NAME ": saved entry #%lld (%zu bytes) to %s (clipboard skipped)\n",
                (long long)saved_id, input.len, g_db_path);
    } else {
        fprintf(stderr, APP_NAME ": saved entry #%lld (%zu bytes) to history and native clipboard\n",
                (long long)saved_id, input.len);
    }
    result = 0;

cleanup:
    progress_stop();
    free(input.data);
    if (db != NULL) {
        int close_rc = sqlite3_close(db);
        if (close_rc != SQLITE_OK) {
            fprintf(stderr, APP_NAME ": closing SQLite database failed: %s\n",
                    sqlite3_errstr(close_rc));
            if (result == 0)
                result = 1;
        }
    }
    free(owned_default_path);
    return result;
}
