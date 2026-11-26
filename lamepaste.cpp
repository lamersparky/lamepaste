#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <new>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

namespace {

constexpr char APP_NAME[] = "lamepaste";
constexpr char APP_DISPLAY_NAME[] = "LamePaste";
constexpr char APP_VERSION[] = "2";
constexpr char BINARY_MARK[] =
    "01100001 01110101 01100100 01110010 01100101 01111001";
constexpr int DEFAULT_HISTORY_LIMIT = 20;
constexpr int MAX_HISTORY_LIMIT = 100000;
constexpr std::size_t IO_CHUNK_SIZE = 256U * 1024U;
constexpr std::size_t PREVIEW_BYTES = 72;

[[nodiscard]] std::string errno_message(int value = errno)
{
    return std::error_code(value, std::generic_category()).message();
}

void throw_system_error(std::string_view operation, int value = errno)
{
    throw std::runtime_error(std::string(operation) + ": " + errno_message(value));
}

[[nodiscard]] bool write_all(int fd, const unsigned char *data, std::size_t length)
{
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t written = ::write(fd, data + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

[[nodiscard]] bool pread_all(int fd, unsigned char *data,
                             std::size_t length, std::uint64_t offset)
{
    std::size_t completed = 0;
    while (completed < length) {
        const std::uint64_t absolute = offset + completed;
        if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            errno = EOVERFLOW;
            return false;
        }
        const ssize_t count = ::pread(fd, data + completed, length - completed,
                                      static_cast<off_t>(absolute));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0) {
            errno = EPIPE;
            return false;
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] bool pwrite_all(int fd, const unsigned char *data,
                              std::size_t length, std::uint64_t offset)
{
    std::size_t completed = 0;
    while (completed < length) {
        const std::uint64_t absolute = offset + completed;
        if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            errno = EOVERFLOW;
            return false;
        }
        const ssize_t count = ::pwrite(fd, data + completed, length - completed,
                                       static_cast<off_t>(absolute));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0) {
            errno = EIO;
            return false;
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

class TempSpool final {
public:
    TempSpool()
    {
        const char *environment_tmp = std::getenv("TMPDIR");
        std::string directory = (environment_tmp != nullptr && *environment_tmp != '\0')
            ? environment_tmp : "/tmp";
        if (directory.back() != '/')
            directory.push_back('/');
        std::string pattern = directory + "lamepaste.XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');

        fd_ = ::mkstemp(writable.data());
        if (fd_ < 0)
            throw_system_error("cannot create a private temporary spool file");

        const int unlink_result = ::unlink(writable.data());
        if (unlink_result != 0) {
            const int saved_errno = errno;
            ::close(fd_);
            fd_ = -1;
            throw_system_error("cannot unlink temporary spool file", saved_errno);
        }

        (void)::fchmod(fd_, S_IRUSR | S_IWUSR);
        const int flags = ::fcntl(fd_, F_GETFD);
        if (flags >= 0)
            (void)::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
    }

    ~TempSpool()
    {
        if (fd_ >= 0)
            (void)::close(fd_);
    }

    TempSpool(const TempSpool &) = delete;
    TempSpool &operator=(const TempSpool &) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

    void append(const unsigned char *data, std::size_t length)
    {
        if (length == 0)
            return;
        if (size_ > std::numeric_limits<std::uint64_t>::max() - length) {
            errno = EOVERFLOW;
            throw_system_error("input is too large");
        }
        if (!pwrite_all(fd_, data, length, size_))
            throw_system_error("cannot write temporary spool file");
        size_ += length;
    }

    void write_at(const unsigned char *data, std::size_t length, std::uint64_t offset)
    {
        if (!pwrite_all(fd_, data, length, offset))
            throw_system_error("cannot write temporary spool file");
        size_ = std::max(size_, offset + static_cast<std::uint64_t>(length));
    }

    [[nodiscard]] bool read_at(unsigned char *data, std::size_t length,
                               std::uint64_t offset) const
    {
        return pread_all(fd_, data, length, offset);
    }

private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

class Progress final {
public:
    Progress() = default;
    ~Progress() { stop(); }

    Progress(const Progress &) = delete;
    Progress &operator=(const Progress &) = delete;

    void start(std::string phase)
    {
        if (!::isatty(STDERR_FILENO))
            return;
        {
            std::lock_guard lock(mutex_);
            phase_ = std::move(phase);
            bytes_ = 0;
            total_ = 0;
            stopping_ = false;
            active_ = true;
            color_ = std::getenv("NO_COLOR") == nullptr;
            const char *term = std::getenv("TERM");
            if (term != nullptr && std::strcmp(term, "dumb") == 0)
                color_ = false;
        }
        try {
            worker_ = std::thread(&Progress::run, this);
        } catch (...) {
            std::lock_guard lock(mutex_);
            active_ = false;
            stopping_ = true;
        }
    }

    void phase(std::string value)
    {
        std::lock_guard lock(mutex_);
        phase_ = std::move(value);
        bytes_ = 0;
        total_ = 0;
        condition_.notify_all();
    }

    void total(std::uint64_t value)
    {
        std::lock_guard lock(mutex_);
        total_ = value;
        condition_.notify_all();
    }

    void bytes(std::uint64_t value)
    {
        std::lock_guard lock(mutex_);
        bytes_ = value;
        condition_.notify_all();
    }

    void stop() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (!active_)
                return;
            stopping_ = true;
            condition_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
        (void)std::fwrite("\r\033[2K", 1, 5, stderr);
        (void)std::fflush(stderr);
        std::lock_guard lock(mutex_);
        active_ = false;
    }

private:
    void run() noexcept
    {
        constexpr std::array<const char *, 10> frames = {
            "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
        };
        std::size_t frame = 0;
        for (;;) {
            std::string phase_copy;
            std::uint64_t bytes_copy = 0;
            std::uint64_t total_copy = 0;
            bool color_copy = false;
            {
                std::unique_lock lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(120),
                                    [this] { return stopping_; });
                if (stopping_)
                    break;
                phase_copy = phase_;
                bytes_copy = bytes_;
                total_copy = total_;
                color_copy = color_;
            }

            const char *color_on = color_copy ? "\033[36m" : "";
            const char *color_off = color_copy ? "\033[0m" : "";
            if (total_copy != 0) {
                const std::uint64_t done = std::min(bytes_copy, total_copy);
                const long double ratio = static_cast<long double>(done) /
                                          static_cast<long double>(total_copy);
                const int filled = static_cast<int>(ratio * 18.0L);
                const int percent = static_cast<int>(ratio * 100.0L);
                std::fprintf(stderr, "\r\033[2K%s%s%s  %-18s  ",
                             color_on, frames[frame], color_off, phase_copy.c_str());
                for (int index = 0; index < 18; ++index)
                    std::fputs(index < filled ? "━" : "─", stderr);
                std::fprintf(stderr, "  %3d%%  %llu/%llu bytes", percent,
                             static_cast<unsigned long long>(done),
                             static_cast<unsigned long long>(total_copy));
            } else if (bytes_copy != 0) {
                std::fprintf(stderr, "\r\033[2K%s%s%s  %-22s  %llu bytes",
                             color_on, frames[frame], color_off, phase_copy.c_str(),
                             static_cast<unsigned long long>(bytes_copy));
            } else {
                std::fprintf(stderr, "\r\033[2K%s%s%s  %s",
                             color_on, frames[frame], color_off, phase_copy.c_str());
            }
            (void)std::fflush(stderr);
            frame = (frame + 1U) % frames.size();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool active_ = false;
    bool stopping_ = true;
    bool color_ = false;
    std::uint64_t bytes_ = 0;
    std::uint64_t total_ = 0;
    std::string phase_;
};

enum class Action { Pipe, History, Show, Restore, Help, Version };

struct Options {
    Action action = Action::Pipe;
    bool action_seen = false;
    bool no_clipboard = false;
    std::string database_path;
    bool database_path_set = false;
    int history_limit = DEFAULT_HISTORY_LIMIT;
    sqlite3_int64 clip_id = 0;
};

void print_usage(std::ostream &output)
{
    output << APP_DISPLAY_NAME << ' ' << APP_VERSION << "\n"
        << "Usage:\n"
        << "  command | " << APP_NAME << " [--db PATH] [--no-clipboard]\n"
        << "  " << APP_NAME << " [--db PATH] --history [N]\n"
        << "  " << APP_NAME << " [--db PATH] --show ID\n"
        << "  " << APP_NAME << " [--db PATH] --restore ID [--no-clipboard]\n\n"
        << "With piped or redirected input, " << APP_NAME
        << " stores the exact bytes and local date/time in a private SQLite\n"
        << "history database, then copies the input to the desktop clipboard.\n\n"
        << "Options:\n"
        << "  -d, --db PATH       Use a different SQLite database file\n"
        << "      --no-clipboard  Save/restore history without using the clipboard\n"
        << "  -H, --history [N]   List the newest N entries (default: "
        << DEFAULT_HISTORY_LIMIT << ")\n"
        << "      --show ID       Write an entry's original bytes to stdout\n"
        << "      --restore ID    Copy an entry back to the clipboard and log it\n"
        << "  -h, --help          Show this help\n"
        << "      --version       Show the exact version identification\n\n"
        << "Database default: ~/.local/share/lamepaste/lamepaste.db\n"
        << "Override the default with LAMEPASTE_DB or --db PATH.\n";
}

[[nodiscard]] bool parse_positive_id(std::string_view text, sqlite3_int64 &value)
{
    if (text.empty())
        return false;
    sqlite3_int64 parsed = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0)
        return false;
    value = parsed;
    return true;
}

[[nodiscard]] bool parse_history_limit(std::string_view text, int &value)
{
    if (text.empty())
        return false;
    int parsed = 0;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end ||
        parsed < 1 || parsed > MAX_HISTORY_LIMIT)
        return false;
    value = parsed;
    return true;
}

[[nodiscard]] bool select_action(Options &options, Action action, std::string &error)
{
    if (options.action_seen) {
        error = "specify only one action";
        return false;
    }
    options.action = action;
    options.action_seen = true;
    return true;
}

[[nodiscard]] bool parse_arguments(int argc, char **argv, Options &options,
                                   std::string &error)
{
    bool options_enabled = true;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (options_enabled && argument == "--") {
            options_enabled = false;
            continue;
        }
        if (!options_enabled) {
            error = "unexpected positional argument: " + std::string(argument);
            return false;
        }
        if (argument == "-h" || argument == "--help") {
            if (!select_action(options, Action::Help, error))
                return false;
        } else if (argument == "--version") {
            if (!select_action(options, Action::Version, error))
                return false;
        } else if (argument == "-d" || argument == "--db") {
            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                error = "--db requires a path";
                return false;
            }
            options.database_path = argv[++index];
            options.database_path_set = true;
        } else if (argument == "--no-clipboard") {
            options.no_clipboard = true;
        } else if (argument == "-H" || argument == "--history") {
            if (!select_action(options, Action::History, error))
                return false;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                if (!parse_history_limit(argv[index + 1], options.history_limit)) {
                    error = "history limit must be between 1 and " +
                            std::to_string(MAX_HISTORY_LIMIT);
                    return false;
                }
                ++index;
            }
        } else if (argument == "--show" || argument == "--restore") {
            const std::string option(argument);
            if (options.action_seen || index + 1 >= argc) {
                error = option + " requires exactly one ID and only one action may be used";
                return false;
            }
            options.action = argument == "--show" ? Action::Show : Action::Restore;
            options.action_seen = true;
            if (!parse_positive_id(argv[++index], options.clip_id)) {
                error = "ID must be a positive integer";
                return false;
            }
        } else {
            error = "unknown option or argument: " + std::string(argument);
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string default_database_path()
{
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        const passwd *account = ::getpwuid(::getuid());
        if (account != nullptr)
            home = account->pw_dir;
    }
    if (home == nullptr || *home == '\0')
        throw std::runtime_error("cannot determine your home directory; set HOME or use --db PATH");
    return std::string(home) + "/.local/share/lamepaste/lamepaste.db";
}

void ensure_database_parent(const std::string &database_path)
{
    if (database_path == ":memory:")
        return;
    const std::filesystem::path parent = std::filesystem::path(database_path).parent_path();
    if (parent.empty())
        return;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error)
        throw std::runtime_error("cannot create database directory '" + parent.string() +
                                 "': " + error.message());
    if (!std::filesystem::is_directory(parent, error) || error)
        throw std::runtime_error("database parent path is not a directory: " + parent.string());
}

class Database final {
public:
    explicit Database(const std::string &path)
    {
        ensure_database_parent(path);
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        const int result = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (result != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory";
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("cannot open database '" + path + "': " + message);
        }
        try {
            sqlite3_extended_result_codes(db_, 1);
            sqlite3_busy_timeout(db_, 5000);
            if (path != ":memory:" && ::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
                throw_system_error("cannot restrict permissions on database '" + path + "'");

            execute("CREATE TABLE IF NOT EXISTS clips ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "copied_at TEXT NOT NULL,"
                    "byte_count INTEGER NOT NULL CHECK(byte_count >= 0),"
                    "payload BLOB NOT NULL"
                    ");"
                    "CREATE INDEX IF NOT EXISTS clips_copied_at_idx ON clips(copied_at);"
                    "PRAGMA user_version = 1;");
        } catch (...) {
            (void)sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    ~Database()
    {
        if (db_ != nullptr)
            (void)sqlite3_close(db_);
    }

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    [[nodiscard]] sqlite3 *get() const noexcept { return db_; }

    [[nodiscard]] std::uint64_t max_blob_size() const noexcept
    {
        const int limit = sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, -1);
        if (limit <= 256)
            return 0;
        return static_cast<std::uint64_t>(std::min(limit - 256, std::numeric_limits<int>::max()));
    }

    void execute(const char *sql) const
    {
        char *message = nullptr;
        const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (result != SQLITE_OK) {
            const std::string error = message != nullptr ? message : sqlite3_errmsg(db_);
            sqlite3_free(message);
            throw std::runtime_error("SQLite error: " + error);
        }
    }

private:
    sqlite3 *db_ = nullptr;
};

[[nodiscard]] std::string make_timestamp()
{
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1))
        throw std::runtime_error("could not read the system clock");
    std::tm local_time{};
    if (::localtime_r(&now, &local_time) == nullptr)
        throw std::runtime_error("could not convert the local date/time");
    std::array<char, 64> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S %z", &local_time) == 0)
        throw std::runtime_error("could not format the local date/time");
    return buffer.data();
}

class Statement final {
public:
    Statement(sqlite3 *db, const char *sql) : db_(db)
    {
        const int result = sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite prepare failed: ") + sqlite3_errmsg(db_));
    }

    ~Statement()
    {
        if (statement_ != nullptr)
            (void)sqlite3_finalize(statement_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    [[nodiscard]] sqlite3_stmt *get() const noexcept { return statement_; }

    void finish()
    {
        if (statement_ == nullptr)
            return;
        sqlite3_stmt *current = std::exchange(statement_, nullptr);
        const int result = sqlite3_finalize(current);
        if (result != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite finalize failed: ") + sqlite3_errmsg(db_));
    }

private:
    sqlite3 *db_;
    sqlite3_stmt *statement_ = nullptr;
};

void insert_clip(sqlite3 *db, const TempSpool &source, sqlite3_int64 &out_id,
                 Progress &progress)
{
    if (source.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("input exceeds SQLite's incremental BLOB interface limit");
    const std::string timestamp = make_timestamp();
    char *message = nullptr;
    int result = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        const std::string error = message != nullptr ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw std::runtime_error("cannot begin SQLite transaction: " + error);
    }
    bool transaction_open = true;
    try {
        {
            Statement statement(db,
                "INSERT INTO clips(copied_at, byte_count, payload) VALUES(?1, ?2, ?3);");
            sqlite3_stmt *raw = statement.get();
            result = sqlite3_bind_text(raw, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
            if (result == SQLITE_OK)
                result = sqlite3_bind_int64(raw, 2, static_cast<sqlite3_int64>(source.size()));
            if (result == SQLITE_OK)
                result = sqlite3_bind_zeroblob64(raw, 3, source.size());
            if (result != SQLITE_OK)
                throw std::runtime_error(std::string("SQLite bind failed: ") + sqlite3_errmsg(db));
            result = sqlite3_step(raw);
            if (result != SQLITE_DONE)
                throw std::runtime_error(std::string("cannot create history entry: ") + sqlite3_errmsg(db));
            out_id = sqlite3_last_insert_rowid(db);
            statement.finish();
        }

        sqlite3_blob *blob = nullptr;
        result = sqlite3_blob_open(db, "main", "clips", "payload", out_id, 1, &blob);
        if (result != SQLITE_OK)
            throw std::runtime_error(std::string("cannot open new history BLOB: ") + sqlite3_errmsg(db));

        std::array<unsigned char, IO_CHUNK_SIZE> buffer{};
        std::uint64_t offset = 0;
        while (offset < source.size()) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), source.size() - offset));
            if (!source.read_at(buffer.data(), amount, offset)) {
                const int saved_errno = errno;
                sqlite3_blob_close(blob);
                throw_system_error("cannot read temporary spool file", saved_errno);
            }
            result = sqlite3_blob_write(blob, buffer.data(), static_cast<int>(amount),
                                        static_cast<int>(offset));
            if (result != SQLITE_OK) {
                const std::string error = sqlite3_errmsg(db);
                sqlite3_blob_close(blob);
                throw std::runtime_error("cannot write history BLOB: " + error);
            }
            offset += amount;
            progress.bytes(offset);
        }
        result = sqlite3_blob_close(blob);
        if (result != SQLITE_OK)
            throw std::runtime_error(std::string("cannot close history BLOB: ") + sqlite3_errmsg(db));

        result = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &message);
        if (result != SQLITE_OK) {
            const std::string error = message != nullptr ? message : sqlite3_errmsg(db);
            sqlite3_free(message);
            message = nullptr;
            throw std::runtime_error("cannot commit history entry: " + error);
        }
        transaction_open = false;
    } catch (...) {
        if (transaction_open)
            (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
}

void copy_blob_to_fd(sqlite3 *db, sqlite3_int64 id, int output_fd,
                     Progress *progress = nullptr)
{
    Statement statement(db, "SELECT byte_count FROM clips WHERE id = ?1;");
    sqlite3_stmt *raw = statement.get();
    int result = sqlite3_bind_int64(raw, 1, id);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot bind history ID: ") + sqlite3_errmsg(db));
    result = sqlite3_step(raw);
    if (result == SQLITE_DONE)
        throw std::runtime_error("no history entry with ID " + std::to_string(id));
    if (result != SQLITE_ROW)
        throw std::runtime_error(std::string("cannot read history entry: ") + sqlite3_errmsg(db));
    const sqlite3_int64 expected_size = sqlite3_column_int64(raw, 0);
    if (expected_size < 0 || expected_size > std::numeric_limits<int>::max())
        throw std::runtime_error("history entry has an invalid or unsupported byte count");
    statement.finish();

    sqlite3_blob *blob = nullptr;
    result = sqlite3_blob_open(db, "main", "clips", "payload", id, 0, &blob);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot open history BLOB: ") + sqlite3_errmsg(db));
    const int blob_size = sqlite3_blob_bytes(blob);
    if (blob_size < 0 || static_cast<sqlite3_int64>(blob_size) != expected_size) {
        sqlite3_blob_close(blob);
        throw std::runtime_error("history entry byte count does not match its stored payload");
    }

    std::array<unsigned char, IO_CHUNK_SIZE> buffer{};
    int offset = 0;
    while (offset < blob_size) {
        const int amount = std::min(static_cast<int>(buffer.size()), blob_size - offset);
        result = sqlite3_blob_read(blob, buffer.data(), amount, offset);
        if (result != SQLITE_OK) {
            const std::string error = sqlite3_errmsg(db);
            sqlite3_blob_close(blob);
            throw std::runtime_error("cannot read history BLOB: " + error);
        }
        if (!write_all(output_fd, buffer.data(), static_cast<std::size_t>(amount))) {
            const int saved_errno = errno;
            sqlite3_blob_close(blob);
            throw_system_error("cannot write restored history entry", saved_errno);
        }
        offset += amount;
        if (progress != nullptr)
            progress->bytes(static_cast<std::uint64_t>(offset));
    }
    result = sqlite3_blob_close(blob);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot close history BLOB: ") + sqlite3_errmsg(db));
}

void load_clip(sqlite3 *db, sqlite3_int64 id, TempSpool &destination,
               Progress &progress)
{
    Statement statement(db, "SELECT byte_count FROM clips WHERE id = ?1;");
    sqlite3_stmt *raw = statement.get();
    int result = sqlite3_bind_int64(raw, 1, id);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot bind history ID: ") + sqlite3_errmsg(db));
    result = sqlite3_step(raw);
    if (result == SQLITE_DONE)
        throw std::runtime_error("no history entry with ID " + std::to_string(id));
    if (result != SQLITE_ROW)
        throw std::runtime_error(std::string("cannot read history entry: ") + sqlite3_errmsg(db));
    const sqlite3_int64 expected_size = sqlite3_column_int64(raw, 0);
    if (expected_size < 0 || expected_size > std::numeric_limits<int>::max())
        throw std::runtime_error("history entry has an invalid or unsupported byte count");
    statement.finish();

    sqlite3_blob *blob = nullptr;
    result = sqlite3_blob_open(db, "main", "clips", "payload", id, 0, &blob);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot open history BLOB: ") + sqlite3_errmsg(db));
    const int blob_size = sqlite3_blob_bytes(blob);
    if (blob_size < 0 || static_cast<sqlite3_int64>(blob_size) != expected_size) {
        sqlite3_blob_close(blob);
        throw std::runtime_error("history entry byte count does not match its stored payload");
    }

    std::array<unsigned char, IO_CHUNK_SIZE> buffer{};
    int offset = 0;
    while (offset < blob_size) {
        const int amount = std::min(static_cast<int>(buffer.size()), blob_size - offset);
        result = sqlite3_blob_read(blob, buffer.data(), amount, offset);
        if (result != SQLITE_OK) {
            const std::string error = sqlite3_errmsg(db);
            sqlite3_blob_close(blob);
            throw std::runtime_error("cannot read history BLOB: " + error);
        }
        try {
            destination.append(buffer.data(), static_cast<std::size_t>(amount));
        } catch (...) {
            sqlite3_blob_close(blob);
            throw;
        }
        offset += amount;
        progress.bytes(static_cast<std::uint64_t>(offset));
    }
    result = sqlite3_blob_close(blob);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot close history BLOB: ") + sqlite3_errmsg(db));
}

void print_preview(const unsigned char *data, std::size_t length)
{
    const std::size_t limit = std::min(length, PREVIEW_BYTES);
    for (std::size_t index = 0; index < limit; ++index) {
        const unsigned char value = data[index];
        if (value == '\n') std::fputs("\\n", stdout);
        else if (value == '\r') std::fputs("\\r", stdout);
        else if (value == '\t') std::fputs("\\t", stdout);
        else if (value == '\0') std::fputs("\\0", stdout);
        else if (value == '\\') std::fputs("\\\\", stdout);
        else if (value >= 0x20 && value <= 0x7e) std::fputc(value, stdout);
        else std::printf("\\x%02x", static_cast<unsigned int>(value));
    }
    if (length > limit)
        std::fputs("...", stdout);
}

void show_history(sqlite3 *db, int limit)
{
    Statement statement(db,
        "SELECT id, copied_at, byte_count, substr(payload, 1, 72) "
        "FROM clips ORDER BY id DESC LIMIT ?1;");
    sqlite3_stmt *raw = statement.get();
    int result = sqlite3_bind_int(raw, 1, limit);
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("cannot bind history limit: ") + sqlite3_errmsg(db));
    std::puts("ID\tDATE/TIME\tBYTES\tPREVIEW");
    while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
        const sqlite3_int64 id = sqlite3_column_int64(raw, 0);
        const unsigned char *when = sqlite3_column_text(raw, 1);
        const sqlite3_int64 byte_count = sqlite3_column_int64(raw, 2);
        const auto *payload = static_cast<const unsigned char *>(sqlite3_column_blob(raw, 3));
        const int payload_length = sqlite3_column_bytes(raw, 3);
        std::printf("%lld\t%s\t%lld\t", static_cast<long long>(id),
                    when != nullptr ? reinterpret_cast<const char *>(when) : "(unknown)",
                    static_cast<long long>(byte_count));
        print_preview(payload, payload_length > 0 ? static_cast<std::size_t>(payload_length) : 0U);
        std::fputc('\n', stdout);
    }
    if (result != SQLITE_DONE)
        throw std::runtime_error(std::string("history query failed: ") + sqlite3_errmsg(db));
    statement.finish();
    if (std::ferror(stdout) != 0)
        throw std::runtime_error("writing history to stdout failed");
}

void show_clip(sqlite3 *db, sqlite3_int64 id)
{
    copy_blob_to_fd(db, id, STDOUT_FILENO);
}

#ifdef HAVE_X11

constexpr std::size_t X11_TRANSFER_CHUNK = 64U * 1024U;
constexpr std::size_t X11_MAX_TRANSFERS = 128U;

struct X11Transfer {
    Window requestor = 0;
    Atom property = None;
    Atom target = None;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    X11Transfer *next = nullptr;
};

struct ClipboardStartup {
    int status = -1;
    int error_number = 0;
    char message[160]{};
};

int x11_error_seen = 0;

int x11_error_handler(Display *, XErrorEvent *event)
{
    x11_error_seen = event->error_code;
    return 0;
}

void x11_send_notify(Display *display, const XSelectionRequestEvent *request, Atom property)
{
    XEvent reply{};
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

[[nodiscard]] std::size_t x11_max_property_bytes(Display *display)
{
    long units = XExtendedMaxRequestSize(display);
    if (units <= 0)
        units = XMaxRequestSize(display);
    if (units <= 0)
        return X11_TRANSFER_CHUNK;
    const auto request_bytes = static_cast<std::uint64_t>(units) * 4U;
    const auto usable = request_bytes > 1024U ? request_bytes - 1024U : X11_TRANSFER_CHUNK;
    return static_cast<std::size_t>(std::min<std::uint64_t>(usable,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

[[nodiscard]] bool x11_start_incr(Display *display, XSelectionRequestEvent *request,
                                  Atom property, Atom incr_atom, Atom target,
                                  std::uint64_t length, X11Transfer **transfers)
{
    std::size_t active = 0;
    for (X11Transfer *item = *transfers; item != nullptr; item = item->next)
        ++active;
    if (active >= X11_MAX_TRANSFERS || length > UINT32_MAX) {
        x11_send_notify(display, request, None);
        return false;
    }

    auto *transfer = new (std::nothrow) X11Transfer{};
    if (transfer == nullptr) {
        x11_send_notify(display, request, None);
        return false;
    }
    transfer->requestor = request->requestor;
    transfer->property = property;
    transfer->target = target;
    transfer->length = length;
    transfer->next = *transfers;
    *transfers = transfer;

    unsigned long total = static_cast<unsigned long>(length);
    x11_error_seen = 0;
    XSelectInput(display, request->requestor, PropertyChangeMask | StructureNotifyMask);
    XChangeProperty(display, request->requestor, property, incr_atom, 32,
                    PropModeReplace, reinterpret_cast<unsigned char *>(&total), 1);
    XSync(display, False);
    if (x11_error_seen != 0) {
        *transfers = transfer->next;
        delete transfer;
        x11_send_notify(display, request, None);
        return false;
    }
    x11_send_notify(display, request, property);
    return true;
}

void x11_handle_selection_request(Display *display, XSelectionRequestEvent *request,
                                  Atom clipboard_atom, Atom targets_atom,
                                  Atom utf8_atom, Atom text_atom, Atom octet_atom,
                                  Atom incr_atom, int data_fd, std::uint64_t length,
                                  X11Transfer **transfers)
{
    const Atom property = request->property != None ? request->property : request->target;
    const Atom offered[] = {targets_atom, utf8_atom, XA_STRING, text_atom, octet_atom};
    const std::size_t max_bytes = x11_max_property_bytes(display);

    if (request->selection != clipboard_atom) {
        x11_send_notify(display, request, None);
        return;
    }
    if (request->target == targets_atom) {
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char *>(offered),
                        static_cast<int>(std::size(offered)));
        x11_send_notify(display, request, property);
        return;
    }
    if (request->target != utf8_atom && request->target != XA_STRING &&
        request->target != text_atom && request->target != octet_atom) {
        x11_send_notify(display, request, None);
        return;
    }

    if (length <= max_bytes && length <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        std::vector<unsigned char> data(static_cast<std::size_t>(length));
        static unsigned char empty = 0;
        if (length != 0 && !pread_all(data_fd, data.data(), data.size(), 0)) {
            x11_send_notify(display, request, None);
            return;
        }
        const unsigned char *payload = length == 0 ? &empty : data.data();
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, payload, static_cast<int>(length));
        x11_send_notify(display, request, property);
        return;
    }
    (void)x11_start_incr(display, request, property, incr_atom, request->target,
                         length, transfers);
}

void x11_handle_property_delete(Display *display, XPropertyEvent *event,
                                int data_fd, X11Transfer **transfers)
{
    X11Transfer **cursor = transfers;
    while (*cursor != nullptr) {
        X11Transfer *transfer = *cursor;
        if (transfer->requestor != event->window || transfer->property != event->atom) {
            cursor = &transfer->next;
            continue;
        }

        if (transfer->offset < transfer->length) {
            std::array<unsigned char, X11_TRANSFER_CHUNK> buffer{};
            const std::size_t max_chunk = std::min(X11_TRANSFER_CHUNK,
                                                   x11_max_property_bytes(display));
            const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
                transfer->length - transfer->offset, max_chunk));
            if (amount == 0 || !pread_all(data_fd, buffer.data(), amount, transfer->offset)) {
                // Terminate the INCR exchange cleanly so a requestor cannot wait forever.
                static unsigned char empty = 0;
                XChangeProperty(display, transfer->requestor, transfer->property,
                                transfer->target, 8, PropModeReplace, &empty, 0);
                *cursor = transfer->next;
                delete transfer;
                XFlush(display);
                return;
            }
            XChangeProperty(display, transfer->requestor, transfer->property,
                            transfer->target, 8, PropModeReplace, buffer.data(),
                            static_cast<int>(amount));
            transfer->offset += amount;
        } else {
            static unsigned char empty = 0;
            XChangeProperty(display, transfer->requestor, transfer->property,
                            transfer->target, 8, PropModeReplace, &empty, 0);
            *cursor = transfer->next;
            delete transfer;
        }
        XFlush(display);
        return;
    }
}

void close_fds_except(int first_keep, int second_keep)
{
    long maximum = ::sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 65536)
        maximum = 65536;
    for (int fd = 0; fd < maximum; ++fd) {
        if (fd != first_keep && fd != second_keep)
            (void)::close(fd);
    }
}

[[noreturn]] void x11_clipboard_owner(int data_fd, std::uint64_t length, int status_fd)
{
    Display *display = XOpenDisplay(nullptr);
    ClipboardStartup startup{};
    if (display == nullptr) {
        startup.status = -1;
        startup.error_number = errno;
        (void)std::snprintf(startup.message, sizeof(startup.message),
                            "cannot connect to DISPLAY (X11/XWayland)");
        (void)write_all(status_fd, reinterpret_cast<const unsigned char *>(&startup), sizeof(startup));
        _exit(1);
    }

    XSetErrorHandler(x11_error_handler);
    const Atom clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    const Atom targets_atom = XInternAtom(display, "TARGETS", False);
    const Atom utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    const Atom text_atom = XInternAtom(display, "TEXT", False);
    const Atom octet_atom = XInternAtom(display, "application/octet-stream", False);
    const Atom incr_atom = XInternAtom(display, "INCR", False);
    const Window owner = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                              0, 0, 1, 1, 0, 0, 0);
    x11_error_seen = 0;
    XSetSelectionOwner(display, clipboard_atom, owner, CurrentTime);
    XSync(display, False);
    if (x11_error_seen != 0 || XGetSelectionOwner(display, clipboard_atom) != owner) {
        startup.status = -1;
        (void)std::snprintf(startup.message, sizeof(startup.message),
                            "X server refused clipboard ownership");
        (void)write_all(status_fd, reinterpret_cast<const unsigned char *>(&startup), sizeof(startup));
        XDestroyWindow(display, owner);
        XCloseDisplay(display);
        _exit(1);
    }

    startup.status = 0;
    (void)write_all(status_fd, reinterpret_cast<const unsigned char *>(&startup), sizeof(startup));
    (void)::close(status_fd);

    X11Transfer *transfers = nullptr;
    XEvent event{};
    for (;;) {
        XNextEvent(display, &event);
        if (event.type == SelectionRequest) {
            x11_handle_selection_request(display, &event.xselectionrequest,
                clipboard_atom, targets_atom, utf8_atom, text_atom, octet_atom,
                incr_atom, data_fd, length, &transfers);
        } else if (event.type == PropertyNotify && event.xproperty.state == PropertyDelete) {
            x11_handle_property_delete(display, &event.xproperty, data_fd, &transfers);
        } else if (event.type == SelectionClear &&
                   event.xselectionclear.selection == clipboard_atom) {
            break;
        } else if (event.type == DestroyNotify) {
            X11Transfer **cursor = &transfers;
            while (*cursor != nullptr) {
                if ((*cursor)->requestor == event.xdestroywindow.window) {
                    X11Transfer *dead = *cursor;
                    *cursor = dead->next;
                    delete dead;
                } else {
                    cursor = &(*cursor)->next;
                }
            }
        }
    }

    while (transfers != nullptr) {
        X11Transfer *next = transfers->next;
        delete transfers;
        transfers = next;
    }
    (void)::close(data_fd);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    _exit(0);
}

[[nodiscard]] bool read_all_fd(int fd, unsigned char *data, std::size_t length)
{
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = ::read(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0) {
            errno = EPIPE;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] int wait_for_child(pid_t child, int *status)
{
    while (::waitpid(child, status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

[[nodiscard]] bool copy_to_clipboard(int data_fd, std::uint64_t length,
                                     std::string &reason)
{
    const char *display_environment = std::getenv("DISPLAY");
    if (display_environment == nullptr || *display_environment == '\0') {
        const char *wayland = std::getenv("WAYLAND_DISPLAY");
        reason = (wayland != nullptr && *wayland != '\0')
            ? "Wayland session detected, but DISPLAY is unavailable; enable XWayland for native X11 clipboard access"
            : "no X11 display is available; native X11/XWayland clipboard access requires DISPLAY";
        return false;
    }
    if (length > UINT32_MAX) {
        reason = "clipboard payload exceeds the X11 INCR protocol size limit";
        return false;
    }

    int status_pipe[2] = {-1, -1};
#ifdef O_CLOEXEC
    if (::pipe2(status_pipe, O_CLOEXEC) != 0) {
#else
    if (::pipe(status_pipe) != 0) {
#endif
        reason = "clipboard status pipe failed: " + errno_message();
        return false;
    }

    const pid_t first_child = ::fork();
    if (first_child < 0) {
        const int saved_errno = errno;
        ::close(status_pipe[0]);
        ::close(status_pipe[1]);
        reason = "clipboard owner fork failed: " + errno_message(saved_errno);
        return false;
    }
    if (first_child == 0) {
        ::close(status_pipe[0]);
        if (::setsid() < 0) {
            ClipboardStartup startup{};
            startup.status = -1;
            startup.error_number = errno;
            (void)std::snprintf(startup.message, sizeof(startup.message),
                                "setsid failed: %s", std::strerror(errno));
            (void)write_all(status_pipe[1], reinterpret_cast<const unsigned char *>(&startup), sizeof(startup));
            _exit(1);
        }
        const pid_t daemon_child = ::fork();
        if (daemon_child < 0) {
            ClipboardStartup startup{};
            startup.status = -1;
            startup.error_number = errno;
            (void)std::snprintf(startup.message, sizeof(startup.message),
                                "clipboard daemon fork failed: %s", std::strerror(errno));
            (void)write_all(status_pipe[1], reinterpret_cast<const unsigned char *>(&startup), sizeof(startup));
            _exit(1);
        }
        if (daemon_child > 0) {
            (void)::close(status_pipe[1]);
            _exit(0);
        }
        close_fds_except(status_pipe[1], data_fd);
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)::dup2(null_fd, STDIN_FILENO);
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                (void)::close(null_fd);
        }
        x11_clipboard_owner(data_fd, length, status_pipe[1]);
    }

    (void)::close(status_pipe[1]);
    ClipboardStartup startup{};
    if (!read_all_fd(status_pipe[0], reinterpret_cast<unsigned char *>(&startup), sizeof(startup))) {
        const int saved_errno = errno;
        (void)::close(status_pipe[0]);
        int child_status = 0;
        (void)wait_for_child(first_child, &child_status);
        reason = "clipboard owner exited before confirming startup: " + errno_message(saved_errno);
        return false;
    }
    (void)::close(status_pipe[0]);
    int child_status = 0;
    (void)wait_for_child(first_child, &child_status);
    if (startup.status != 0) {
        reason = startup.message[0] != '\0' ? startup.message : "native clipboard owner failed to start";
        return false;
    }
    return true;
}

#else

[[nodiscard]] bool copy_to_clipboard(int, std::uint64_t, std::string &reason)
{
    reason = "lamepaste was built without native X11 support; install libx11-dev and rebuild";
    return false;
}

#endif

void print_version()
{
    std::cout << APP_DISPLAY_NAME << ' ' << APP_VERSION << '\n'
              << BINARY_MARK << '\n';
}

int run(int argc, char **argv)
{
    (void)::umask(0077);
    Options options;
    std::string parse_error;
    if (!parse_arguments(argc, argv, options, parse_error)) {
        std::cerr << APP_NAME << ": " << parse_error << '\n';
        if (parse_error.find("unknown option") != std::string::npos)
            print_usage(std::cerr);
        return 2;
    }

    if (options.action == Action::Help) {
        print_usage(std::cout);
        return std::cout.good() ? 0 : 1;
    }
    if (options.action == Action::Version) {
        print_version();
        return std::cout.good() ? 0 : 1;
    }

    std::string database_path;
    if (options.database_path_set) {
        database_path = options.database_path;
    } else {
        const char *environment_path = std::getenv("LAMEPASTE_DB");
        database_path = (environment_path != nullptr && *environment_path != '\0')
            ? environment_path : default_database_path();
    }

    Database database(database_path);
    sqlite3 *db = database.get();
    const std::uint64_t maximum_blob_size = database.max_blob_size();

    if (options.action == Action::History) {
        show_history(db, options.history_limit);
        return 0;
    }
    if (options.action == Action::Show) {
        show_clip(db, options.clip_id);
        return 0;
    }

    TempSpool input;
    Progress progress;
    progress.start(options.action == Action::Pipe ? "Reading input" : "Restoring history entry");

    if (options.action == Action::Pipe) {
        struct stat input_status{};
        if (::fstat(STDIN_FILENO, &input_status) == 0 && S_ISREG(input_status.st_mode) &&
            input_status.st_size > 0)
            progress.total(static_cast<std::uint64_t>(input_status.st_size));
        if (::isatty(STDIN_FILENO)) {
            std::cerr << APP_NAME << ": expected piped or redirected input; example: printf 'hello' | "
                      << APP_NAME << '\n';
            return 2;
        }

        std::array<unsigned char, IO_CHUNK_SIZE> buffer{};
        for (;;) {
            const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count == 0)
                break;
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw_system_error("reading stdin failed");
            }
            const auto amount = static_cast<std::size_t>(count);
            if (input.size() > maximum_blob_size || amount > maximum_blob_size - input.size())
                throw std::runtime_error("input exceeds this SQLite build's safe BLOB size limit (" +
                                         std::to_string(maximum_blob_size) + " bytes)");
            input.append(buffer.data(), amount);
            progress.bytes(input.size());
        }
    } else if (options.action == Action::Restore) {
        load_clip(db, options.clip_id, input, progress);
        if (input.size() > maximum_blob_size)
            throw std::runtime_error("selected history entry exceeds this SQLite build's BLOB size limit");
    }

    progress.phase("Saving to SQLite");
    progress.total(input.size());
    sqlite3_int64 saved_id = 0;
    insert_clip(db, input, saved_id, progress);

    if (!options.no_clipboard) {
        progress.phase("Copying to clipboard");
        // Stop the renderer before forking. This avoids forking a process while
        // another thread may hold a C/C++ runtime lock.
        progress.stop();
        std::string reason;
        if (!copy_to_clipboard(input.fd(), input.size(), reason)) {
            std::cerr << APP_NAME << ": saved entry #" << saved_id << " ("
                      << input.size() << " bytes) to history, but native clipboard copy failed: "
                      << reason << '\n';
            return 1;
        }
    }

    progress.stop();
    if (options.no_clipboard) {
        std::cerr << APP_NAME << ": saved entry #" << saved_id << " ("
                  << input.size() << " bytes) to " << database_path
                  << " (clipboard skipped)\n";
    } else {
        std::cerr << APP_NAME << ": saved entry #" << saved_id << " ("
                  << input.size() << " bytes) to history and native clipboard\n";
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception &error) {
        std::cerr << APP_NAME << ": " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << APP_NAME << ": unexpected fatal error\n";
        return 1;
    }
}
