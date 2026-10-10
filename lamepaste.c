#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
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

#define APP_NAME "lamepaste"
#define DEFAULT_HISTORY_LIMIT 20
#define MAX_HISTORY_LIMIT 100000
#define INPUT_CHUNK_SIZE 16384
#define PREVIEW_BYTES 72

static const char *g_db_path = NULL;

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
        "Usage:\n"
        "  command | " APP_NAME " [--db PATH] [--no-clipboard]\n"
        "  " APP_NAME " [--db PATH] --history [N]\n"
        "  " APP_NAME " [--db PATH] --show ID\n"
        "  " APP_NAME " [--db PATH] --restore ID [--no-clipboard]\n"
        "\n"
        "With piped input, " APP_NAME " saves the exact input bytes and local\n"
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
    sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)len);
    if (len == 0)
        rc = sqlite3_bind_zeroblob(stmt, 3, 0);
    else
        rc = sqlite3_bind_blob64(stmt, 3, data, (sqlite3_uint64)len, SQLITE_TRANSIENT);
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

static int command_exists(const char *command)
{
    const char *path_env;
    const char *cursor;
    size_t command_len;

    if (strchr(command, '/') != NULL)
        return access(command, X_OK) == 0;

    path_env = getenv("PATH");
    if (path_env == NULL)
        return 0;
    command_len = strlen(command);
    cursor = path_env;
    while (1) {
        const char *end = strchr(cursor, ':');
        size_t dir_len = end ? (size_t)(end - cursor) : strlen(cursor);
        size_t alloc_len = (dir_len ? dir_len : 1) + 1 + command_len + 1;
        char *candidate = malloc(alloc_len);
        int found = 0;

        if (candidate == NULL)
            return 0;
        if (dir_len == 0)
            (void)snprintf(candidate, alloc_len, "./%s", command);
        else
            (void)snprintf(candidate, alloc_len, "%.*s/%s", (int)dir_len, cursor, command);
        found = access(candidate, X_OK) == 0;
        free(candidate);
        if (found)
            return 1;
        if (end == NULL)
            break;
        cursor = end + 1;
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

static int wait_for_child(pid_t pid, int *status)
{
    while (waitpid(pid, status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* Feed bytes directly to an external clipboard utility without invoking a shell. */
static int run_clipboard_command(char *const argv[], const unsigned char *data,
                                 size_t len, char *why, size_t why_size)
{
    int input_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    pid_t pid;
    int exec_errno = 0;
    ssize_t exec_read;
    int status = 0;
    int write_error = 0;

    if (pipe(input_pipe) != 0) {
        (void)snprintf(why, why_size, "pipe failed: %s", strerror(errno));
        return -1;
    }
    if (pipe(exec_pipe) != 0) {
        int saved = errno;
        close(input_pipe[0]);
        close(input_pipe[1]);
        (void)snprintf(why, why_size, "pipe failed: %s", strerror(saved));
        return -1;
    }
    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        int saved = errno;
        close(input_pipe[0]); close(input_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        (void)snprintf(why, why_size, "fcntl failed: %s", strerror(saved));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(input_pipe[0]); close(input_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        (void)snprintf(why, why_size, "fork failed: %s", strerror(saved));
        return -1;
    }

    if (pid == 0) {
        int saved;
        close(input_pipe[1]);
        close(exec_pipe[0]);
        (void)signal(SIGPIPE, SIG_DFL);
        if (dup2(input_pipe[0], STDIN_FILENO) < 0) {
            saved = errno;
            (void)write(exec_pipe[1], &saved, sizeof(saved));
            _exit(127);
        }
        close(input_pipe[0]);
        execvp(argv[0], argv);
        saved = errno;
        (void)write(exec_pipe[1], &saved, sizeof(saved));
        _exit(127);
    }

    close(input_pipe[0]);
    close(exec_pipe[1]);

    /* EOF here means exec succeeded (the descriptor is close-on-exec). */
    do {
        exec_read = read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
    } while (exec_read < 0 && errno == EINTR);
    close(exec_pipe[0]);

    if (exec_read > 0) {
        close(input_pipe[1]);
        (void)wait_for_child(pid, &status);
        (void)snprintf(why, why_size, "could not start %s: %s", argv[0], strerror(exec_errno));
        return -1;
    }
    if (exec_read < 0) {
        int saved = errno;
        close(input_pipe[1]);
        (void)wait_for_child(pid, &status);
        (void)snprintf(why, why_size, "could not verify launch of %s: %s", argv[0], strerror(saved));
        return -1;
    }

    if (len != 0 && write_all(input_pipe[1], data, len) != 0)
        write_error = errno;
    if (close(input_pipe[1]) != 0 && write_error == 0)
        write_error = errno;

    if (wait_for_child(pid, &status) != 0) {
        (void)snprintf(why, why_size, "waiting for %s failed: %s", argv[0], strerror(errno));
        return -1;
    }
    if (write_error != 0) {
        (void)snprintf(why, why_size, "writing clipboard data failed: %s", strerror(write_error));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status))
            (void)snprintf(why, why_size, "%s terminated by signal %d", argv[0], WTERMSIG(status));
        else
            (void)snprintf(why, why_size, "%s exited with status %d", argv[0],
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

static int copy_to_clipboard(const unsigned char *data, size_t len, char *why, size_t why_size)
{
    char *wl_argv[] = {(char *)"wl-copy", NULL};
    char *xclip_argv[] = {(char *)"xclip", (char *)"-selection", (char *)"clipboard", (char *)"-in", NULL};
    char *xsel_argv[] = {(char *)"xsel", (char *)"--clipboard", (char *)"--input", NULL};
    int wayland = getenv("WAYLAND_DISPLAY") != NULL && *getenv("WAYLAND_DISPLAY") != '\0';
    int x11 = getenv("DISPLAY") != NULL && *getenv("DISPLAY") != '\0';
    int tried = 0;
    char last_error[256] = "no supported clipboard program found";

    if (wayland && command_exists("wl-copy")) {
        tried = 1;
        if (run_clipboard_command(wl_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }
    if (x11 && command_exists("xclip")) {
        tried = 1;
        if (run_clipboard_command(xclip_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }
    if (x11 && command_exists("xsel")) {
        tried = 1;
        if (run_clipboard_command(xsel_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }
    /* Last-chance attempts also help in nested/mixed desktop sessions. */
    if (!wayland && command_exists("wl-copy")) {
        tried = 1;
        if (run_clipboard_command(wl_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }
    if (!x11 && command_exists("xclip")) {
        tried = 1;
        if (run_clipboard_command(xclip_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }
    if (!x11 && command_exists("xsel")) {
        tried = 1;
        if (run_clipboard_command(xsel_argv, data, len, last_error, sizeof(last_error)) == 0)
            return 0;
    }

    if (!tried) {
        (void)snprintf(why, why_size,
            "no clipboard utility found; install wl-clipboard (Wayland), xclip or xsel (X11)");
    } else {
        (void)snprintf(why, why_size, "%s", last_error);
    }
    return -1;
}

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
    sqlite3_bind_int(stmt, 1, limit);
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
    sqlite3_bind_int64(stmt, 1, id);
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
            fprintf(stderr, APP_NAME ": writing entry to stdout failed: %s\n", strerror(errno));
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
    sqlite3_bind_int64(stmt, 1, id);
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
        puts(APP_NAME " 1.0.0");
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

    if (action == ACTION_PIPE) {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, APP_NAME ": expected piped input; example: printf 'hello' | " APP_NAME "\n");
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

    if (insert_clip(db, input.data, input.len, &saved_id) != 0)
        goto cleanup;

    if (!no_clipboard) {
        char why[512];
        /* Prevent a closed clipboard pipe from terminating the whole program. */
        (void)signal(SIGPIPE, SIG_IGN);
        if (copy_to_clipboard(input.data, input.len, why, sizeof(why)) != 0) {
            fprintf(stderr,
                APP_NAME ": saved entry #%lld (%zu bytes) to %s, but clipboard copy failed: %s\n",
                (long long)saved_id, input.len, g_db_path, why);
            goto cleanup;
        }
    }

    if (no_clipboard) {
        fprintf(stderr, APP_NAME ": saved entry #%lld (%zu bytes) to %s (clipboard skipped)\n",
                (long long)saved_id, input.len, g_db_path);
    } else {
        fprintf(stderr, APP_NAME ": saved entry #%lld (%zu bytes) to history and clipboard\n",
                (long long)saved_id, input.len);
    }
    result = 0;

cleanup:
    free(input.data);
    if (db != NULL)
        sqlite3_close(db);
    free(owned_default_path);
    return result;
}
