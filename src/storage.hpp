#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace mdmem {

// ── Frontmatter §4.4 ──────────────────────────────────────────────

struct Frontmatter {
    std::string date;       // YYYY-MM-DD (LLM-extracted)
    std::string created;    // YYYY-MM-DDTHH:MM:SSZ (app-generated)
    std::string tags;       // comma-separated keywords
    std::string title;      // one-line descriptive title
    std::string body;       // body text (after frontmatter + blank line)
};

// ── FileLock RAII §9 ──────────────────────────────────────────────

enum class LockMode {
    SHARED,      // LOCK_SH  — query
    EXCLUSIVE    // LOCK_EX  — store
};

class FileLock {
public:
    // Opens <memdir>/.mdmem.lock and calls flock() with the given mode.
    // Blocks until the lock is acquired. Throws on open/create failure.
    FileLock(const std::string& memdir, LockMode mode);
    ~FileLock();

    // Non-copyable, non-movable
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) = delete;
    FileLock& operator=(FileLock&&) = delete;

private:
    int fd_;
};

// ── StorageEngine ─────────────────────────────────────────────────

class StorageEngine {
public:
    // ── File I/O ──────────────────────────────────────────────────

    // Returns sorted list of entry names in `path`.
    // Throws std::runtime_error if path cannot be opened.
    static std::vector<std::string> list_directory(const std::string& path);

    // Returns true if `path` is a directory.
    static bool is_directory(const std::string& path);

    // Reads entire file content. Throws on failure.
    static std::string read_file(const std::string& path);

    // Writes `content` to `path`, creating parent directories as needed.
    // Returns true on success. Throws on I/O failure.
    static bool write_file(const std::string& path, const std::string& content);

    // ── Frontmatter parsing §4.4 ──────────────────────────────────

    // Parses YAML frontmatter between --- delimiters.
    // Splits on FIRST colon only (values may contain colons per §4.2).
    // Unknown frontmatter keys silently ignored.
    // Returns Frontmatter with body containing everything after
    // the closing --- and blank line.
    static Frontmatter parse_frontmatter(const std::string& content);

    // ── Leaf/branch detection §5.2 ────────────────────────────────

    // True if directory contains only .md files, no subdirs.
    static bool is_leaf(const std::string& path);

    // True if directory contains only subdirs, no .md files.
    static bool is_branch(const std::string& path);

    // Lists .md files in `path`, sorted by date (newest first).
    // Date is extracted from YYYY-MM-DD prefix in filename.
    static std::vector<std::string> list_md_files(const std::string& path);

    // True if directory contains subdirectories.
    static bool has_subdirs(const std::string& path);

    // ── Context budget helper §2.3 ────────────────────────────────

    // Returns ctx_size * 0.8 * 3.0
    static size_t compute_leaf_max_chars(int ctx_size);

    // ── Filename collision §4.3 ───────────────────────────────────

    // If `dir`/`base_name` exists, returns `dir`/`base_name-1`,
    // then -2, etc. until an unused name is found.
    static std::string resolve_collision(const std::string& dir,
                                         const std::string& base_name);
};

} // namespace mdmem
