#include "storage.hpp"

#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace mdmem {

// ═══════════════════════════════════════════════════════════════════
//  FileLock  §9
// ═══════════════════════════════════════════════════════════════════

FileLock::FileLock(const std::string& memdir, LockMode mode)
    : fd_(-1)
{
    std::string lock_path = memdir + "/.mdmem.lock";

    fd_ = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        throw std::runtime_error(
            "cannot open/create lock file: " + lock_path + " — "
            + std::strerror(errno));
    }

    int lock_op = (mode == LockMode::SHARED) ? LOCK_SH : LOCK_EX;

    if (flock(fd_, lock_op) != 0) {
        int saved_errno = errno;
        close(fd_);
        fd_ = -1;
        throw std::runtime_error(
            "flock failed on " + lock_path + " — "
            + std::strerror(saved_errno));
    }
}

FileLock::~FileLock()
{
    if (fd_ >= 0) {
        // flock is released on close
        close(fd_);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  File I/O helpers
// ═══════════════════════════════════════════════════════════════════

std::vector<std::string> StorageEngine::list_directory(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        throw std::runtime_error(
            "cannot open directory '" + path + "': "
            + std::strerror(errno));
    }

    std::vector<std::string> entries;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // skip . and ..
        if (name == "." || name == "..") continue;
        // skip the lock file
        if (name == ".mdmem.lock") continue;
        entries.push_back(name);
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end());
    return entries;
}

bool StorageEngine::is_directory(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

std::string StorageEngine::read_file(const std::string& path)
{
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error(
            "cannot open file '" + path + "': "
            + std::strerror(errno));
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();

    if (ifs.fail() && !ifs.eof()) {
        throw std::runtime_error(
            "error reading file '" + path + "': "
            + std::strerror(errno));
    }

    return oss.str();
}

bool StorageEngine::write_file(const std::string& path, const std::string& content)
{
    // Create parent directories if needed
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        std::string parent = path.substr(0, slash);
        if (!parent.empty()) {
            // Recursive mkdir
            std::string cmd = "mkdir -p '" + parent + "'";
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                throw std::runtime_error(
                    "cannot create parent directory for '" + path + "'");
            }
        }
    }

    std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        throw std::runtime_error(
            "cannot open file for writing '" + path + "': "
            + std::strerror(errno));
    }

    ofs << content;

    if (ofs.fail()) {
        throw std::runtime_error(
            "error writing file '" + path + "': "
            + std::strerror(errno));
    }

    ofs.close();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  Frontmatter parsing  §4.4
// ═══════════════════════════════════════════════════════════════════

Frontmatter StorageEngine::parse_frontmatter(const std::string& content)
{
    Frontmatter fm;
    std::istringstream stream(content);
    std::string line;

    // Step 1: look for opening ---
    std::getline(stream, line);
    // Trim trailing CR
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // If the file doesn't start with ---, there's no frontmatter;
    // the entire content is the body.
    if (line != "---") {
        fm.body = content;
        return fm;
    }

    // Step 2: read YAML lines until closing ---
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Check for closing ---
        if (line == "---") break;

        // Split on FIRST colon only (values may contain colons §4.2)
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;  // malformed line, skip

        std::string key = line.substr(0, colon);
        // Value starts after the colon; trim one leading space if present
        std::string value;
        if (colon + 1 < line.size()) {
            value = line.substr(colon + 1);
            // Trim leading space
            if (!value.empty() && value[0] == ' ') {
                value.erase(0, 1);
            }
        }

        // Assign to known fields; unknown keys silently ignored
        if (key == "date")       fm.date    = value;
        else if (key == "created") fm.created = value;
        else if (key == "tags")     fm.tags    = value;
        else if (key == "title")    fm.title   = value;
        // unknown keys: silently ignored
    }

    // Step 3: skip the blank line after closing ---
    std::getline(stream, line);  // blank line

    // Step 4: everything remaining is the body
    std::ostringstream body_oss;
    bool first = true;
    while (std::getline(stream, line)) {
        if (!first) body_oss << '\n';
        body_oss << line;
        first = false;
    }
    fm.body = body_oss.str();

    return fm;
}

// ═══════════════════════════════════════════════════════════════════
//  Leaf/branch detection  §5.2
// ═══════════════════════════════════════════════════════════════════

bool StorageEngine::is_leaf(const std::string& path)
{
    if (!is_directory(path)) return false;

    bool has_md = false;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name == ".mdmem.lock") continue;

        std::string full = path + "/" + name;

        if (is_directory(full)) {
            closedir(dir);
            return false;  // has subdir → not a leaf
        }

        // Check if it's a .md file
        if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".md") == 0) {
            has_md = true;
        }
    }
    closedir(dir);
    return has_md;  // leaf if it has .md files and no subdirs
}

bool StorageEngine::is_branch(const std::string& path)
{
    if (!is_directory(path)) return false;

    bool has_subdir = false;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name == ".mdmem.lock") continue;

        std::string full = path + "/" + name;

        if (is_directory(full)) {
            has_subdir = true;
        } else if (name.size() >= 3
                   && name.compare(name.size() - 3, 3, ".md") == 0) {
            closedir(dir);
            return false;  // has .md file → not a branch
        }
    }
    closedir(dir);
    return has_subdir;  // branch if it has subdirs and no .md files
}

// Helper: extract YYYY-MM-DD date from filename prefix for sorting
static std::string extract_date_from_filename(const std::string& filename)
{
    // Filenames are <YYYY-MM-DD>_<slug>.md
    if (filename.size() < 10) return "";
    return filename.substr(0, 10);  // YYYY-MM-DD
}

std::vector<std::string> StorageEngine::list_md_files(const std::string& path)
{
    std::vector<std::string> result;
    DIR* dir = opendir(path.c_str());
    if (!dir) return result;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name == ".mdmem.lock") continue;
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".md") != 0)
            continue;

        std::string full = path + "/" + name;
        if (!is_directory(full)) {
            result.push_back(name);
        }
    }
    closedir(dir);

    // Sort by date, newest first (descending)
    std::sort(result.begin(), result.end(),
        [](const std::string& a, const std::string& b) {
            return extract_date_from_filename(a)
                 > extract_date_from_filename(b);
        });

    return result;
}

bool StorageEngine::has_subdirs(const std::string& path)
{
    if (!is_directory(path)) return false;

    DIR* dir = opendir(path.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name == ".mdmem.lock") continue;

        std::string full = path + "/" + name;
        if (is_directory(full)) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Context budget helper  §2.3
// ═══════════════════════════════════════════════════════════════════

size_t StorageEngine::compute_leaf_max_chars(int ctx_size)
{
    // ctx_size * 0.8 * 3.0  (§2.3)
    return static_cast<size_t>(ctx_size * 0.8 * 3.0);
}

// ═══════════════════════════════════════════════════════════════════
//  Filename collision  §4.3
// ═══════════════════════════════════════════════════════════════════

std::string StorageEngine::resolve_collision(const std::string& dir,
                                              const std::string& base_name)
{
    std::string candidate = base_name;
    std::string full = dir + "/" + candidate;

    // Extract stem and extension
    std::string stem;
    std::string ext;
    auto dot = candidate.rfind('.');
    if (dot != std::string::npos) {
        stem = candidate.substr(0, dot);
        ext = candidate.substr(dot);
    } else {
        stem = candidate;
    }

    // Check if base name is available
    {
        struct stat st;
        if (stat(full.c_str(), &st) != 0) {
            return candidate;  // base name is available
        }
    }

    // Collision — append -1, -2, etc.
    int suffix = 1;
    while (true) {
        candidate = stem + "-" + std::to_string(suffix) + ext;
        full = dir + "/" + candidate;

        struct stat st;
        if (stat(full.c_str(), &st) != 0) {
            return candidate;
        }
        suffix++;
    }
}

} // namespace mdmem
