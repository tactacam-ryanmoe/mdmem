#include "store.hpp"
#include "prompts.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

using mdmem::StorageEngine;

namespace {

// ── Structured classification result ──────────────────────────────────

struct ClassificationResult {
    std::string title;
    std::string date;
    std::string body;
    std::string tags;
    std::string slug;
};

// ── Timestamp helpers §4.2 ────────────────────────────────────────────

std::string make_date() {
    auto now = std::time(nullptr);
    auto* gmt = std::gmtime(&now);
    if (gmt == nullptr) return "1970-01-01";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", gmt);
    return buf;
}

std::string make_timestamp() {
    auto now = std::time(nullptr);
    auto* gmt = std::gmtime(&now);
    if (gmt == nullptr) return "1970-01-01T00:00:00Z";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmt);
    return buf;
}

// ── Slug sanitisation §4.3 ────────────────────────────────────────────

std::string sanitize_slug(const std::string& raw) {
    std::string result;
    for (char c : raw) {
        if (std::islower(static_cast<unsigned char>(c))
            || std::isdigit(static_cast<unsigned char>(c))
            || c == '-') {
            result.push_back(c);
        }
    }
    return result;
}

// ── Trim helper ───────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size()
           && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start
           && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(start, end - start);
}

// ── Parsing: folder scores §7.3.1 ─────────────────────────────────────

std::map<std::string, int> parse_folder_scores(const std::string& response) {
    std::map<std::string, int> scores;
    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;

        // Use rfind so folder names with colons still parse correctly
        auto colon = line.rfind(':');
        if (colon == std::string::npos) continue;

        std::string name  = trim(line.substr(0, colon));
        std::string score_str = trim(line.substr(colon + 1));

        if (name.empty() || score_str.empty()) continue;

        try {
            int score = std::stoi(score_str);
            scores[name] = score;
        } catch (const std::invalid_argument&) {
            // Non-numeric score — skip this line
        } catch (const std::out_of_range&) {
            // Score out of int range — skip
        }
    }
    return scores;
}

// ── Parsing: placement decision §6.4 ──────────────────────────────────

std::string parse_placement(const std::string& response) {
    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.rfind("FOLDER:", 0) == 0) {
            std::string path = trim(line.substr(7));  // after "FOLDER:"
            return path;
        }
    }
    return {};  // No FOLDER: line found → parse error
}

// ── Parsing: classification §7.3.4 ────────────────────────────────────

ClassificationResult parse_classification(const std::string& response) {
    ClassificationResult result;
    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (line.rfind("TITLE:", 0) == 0) {
            result.title = trim(line.substr(6));
        } else if (line.rfind("DATE:", 0) == 0) {
            result.date = trim(line.substr(5));
        } else if (line.rfind("BODY:", 0) == 0) {
            result.body = line.substr(5);
            // Trim leading space only; preserve trailing (part of body)
            if (!result.body.empty() && result.body.front() == ' ') {
                result.body.erase(0, 1);
            }
        } else if (line.rfind("TAGS:", 0) == 0) {
            result.tags = trim(line.substr(5));
        } else if (line.rfind("SLUG:", 0) == 0) {
            result.slug = trim(line.substr(5));
        }
    }
    return result;
}

// ── Store file at a leaf path §7.3.4-§7.3.5, §8.4 ──────────────────

// Returns true if the .md file was written successfully.
// On validation failure, prints error to stderr and returns false.
bool store_file_at_path(
    const std::string& target_path,
    const std::string& memory_text,
    const std::string& current_date,
    ModelEngine& model,
    int min_body,
    int max_body)
{
    auto existing_files = StorageEngine::list_md_files(target_path);

    std::string class_system = mdmem::classification_system_prompt(
        current_date, min_body, max_body);
    std::string class_user = mdmem::classification_user_prompt(
        memory_text, target_path, existing_files);
    std::string class_resp = model.infer(class_system, class_user);

    auto meta = parse_classification(class_resp);

    // ── Body validation §8.4, §7.3.5 ─────────────────────────────
    auto body_len = static_cast<int>(meta.body.size());

    if (body_len < min_body || body_len > max_body) {
        // First attempt failed — retry with emphasis prompt
        std::string retry_system = class_system + "\n"
            + mdmem::retry_emphasis(min_body, max_body, body_len);
        std::string retry_resp = model.infer(retry_system, class_user);

        meta = parse_classification(retry_resp);
        body_len = static_cast<int>(meta.body.size());

        if (body_len < min_body || body_len > max_body) {
            // Second attempt also failed → abort
            std::fprintf(stderr,
                "[STORE_VALIDATION_FAILED] Body length validation failed "
                "after 2 attempts. Expected %d-%d chars, got %d. "
                "Adjust your prompt or relax --min-body-chars / "
                "--max-body-chars constraints.\n",
                min_body, max_body, body_len);
            return false;
        }
    }

    // ── Build & write .md file §4.1, §4.3 ───────────────────────
    if (meta.title.empty()) meta.title = "Untitled";
    if (meta.date.empty())  meta.date  = current_date;
    if (meta.tags.empty())  meta.tags  = "general";

    std::string slug       = sanitize_slug(meta.slug);
    std::string created_ts = make_timestamp();

    // Assemble frontmatter + title + body
    std::ostringstream md;
    md << "---\n"
       << "date: " << meta.date << "\n"
       << "created: " << created_ts << "\n"
       << "tags: " << meta.tags << "\n"
       << "---\n\n"
       << "# " << meta.title << "\n\n"
       << meta.body << "\n";

    // Filename: <date>_<slug>.md
    std::string filename = meta.date + "_" + slug + ".md";

    // Resolve collision via storage engine §4.3
    std::string resolved_name =
        StorageEngine::resolve_collision(target_path, filename);
    std::string full_path = target_path + "/" + resolved_name;

    StorageEngine::write_file(full_path, md.str());

    // Output per §8.2 item 8
    std::cout << "stored:" << resolved_name << ":" << meta.date << '\n';

    return true;
}

// ── New-folder creation fallback §7.3.6, §8.3 ──────────────────────

// Attempts to create a new folder at `dead_end_path` and store the
// memory in it. Returns the path where the file was stored on success,
// or empty string on failure (unparseable LLM response, mkdir failure,
// or validation failure).
std::string attempt_new_folder_store(
    const std::string& dead_end_path,
    const std::string& memory_text,
    ModelEngine& model,
    int threshold,
    int min_body,
    int max_body)
{
    // List existing folders at this level
    std::vector<std::string> folder_names;
    auto all_entries = StorageEngine::list_directory(dead_end_path);
    for (const auto& entry : all_entries) {
        std::string full = dead_end_path + "/" + entry;
        if (StorageEngine::is_directory(full)) {
            folder_names.push_back(entry);
        }
    }

    // Call new-folder prompt §7.3.6
    std::string system = mdmem::new_folder_system_prompt();
    std::string user   = mdmem::new_folder_user_prompt(
        memory_text, dead_end_path, folder_names);
    std::string response = model.infer(system, user);

    // Parse FOLDER:<name> (reuses placement parser)
    std::string folder_name = parse_placement(response);
    if (folder_name.empty()) return {};

    // Create the new folder
    std::string new_folder_path = dead_end_path + "/" + folder_name;
    if (mkdir(new_folder_path.c_str(), 0755) != 0) {
        if (errno != EEXIST) return {};
    }

    // Store the file in the new folder
    std::string current_date = make_date();
    if (!store_file_at_path(new_folder_path, memory_text, current_date,
                            model, min_body, max_body)) {
        return {};
    }

    return new_folder_path;
}

// ── Multi-branch traversal for STORE §6.2 ─────────────────────────────

std::vector<LeafCandidate> traverse_for_store(
    const std::string& path,
    const std::string& memory_text,
    ModelEngine& model,
    StorageEngine& storage,
    int threshold,
    int min_body,
    int max_body)
{
    // ── Branch level: score subdirectories and recurse ────────────────

    if (StorageEngine::has_subdirs(path)) {
        // Collect subdirectory names only
        auto all_entries = StorageEngine::list_directory(path);
        std::vector<std::string> subdir_names;
        for (const auto& entry : all_entries) {
            std::string full = path + "/" + entry;
            if (StorageEngine::is_directory(full)) {
                subdir_names.push_back(entry);
            }
        }

        if (subdir_names.empty()) {
            // has_subdirs said true but no subdirs found — safety fallback
            return {};
        }

        // Score all subdirectory names via LLM
        std::string system = mdmem::folder_scoring_system_prompt();
        std::string user   = mdmem::folder_scoring_user_prompt(
            memory_text, subdir_names);
        std::string response = model.infer(system, user);

        auto scores = parse_folder_scores(response);

        // Follow every folder with score >= threshold
        std::vector<LeafCandidate> results;
        for (const auto& name : subdir_names) {
            int score = 0;
            auto it = scores.find(name);
            if (it != scores.end()) score = it->second;

            if (score >= threshold) {
                std::string child_path = path + "/" + name;
                auto child_results = traverse_for_store(
                    child_path, memory_text, model, storage, threshold,
                    min_body, max_body);
                results.insert(results.end(),
                               child_results.begin(), child_results.end());
            }
        }

        // All children pruned — try new-folder creation at this level
        if (results.empty()) {
            auto stored_path = attempt_new_folder_store(
                path, memory_text, model, threshold, min_body, max_body);
            if (!stored_path.empty()) {
                LeafCandidate done;
                done.path    = stored_path;
                done.stored  = true;
                return { done };
            }
        }

        // May be empty if fallback also failed at this or deeper levels
        return results;
    }

    // ── Leaf level: collect file listing ──────────────────────────────

    if (StorageEngine::is_leaf(path)) {
        LeafCandidate leaf;
        leaf.path = path;
        leaf.file_list = StorageEngine::list_md_files(path);
        return { leaf };
    }

    // Neither branch nor leaf (empty dir, mixed content, etc.)
    return {};
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  run_store — §8.2 store-mode entry point
// ═══════════════════════════════════════════════════════════════════════

int run_store(const CliArgs& args,
              ModelEngine& model,
              StorageEngine& storage)
{
    if (!args.store.has_value()) {
        // Should not happen — CLI parser enforces --store presence
        std::fprintf(stderr, "error: no store prompt provided\n");
        return 1;
    }

    const std::string& memory_text = args.store.value();
    int threshold   = args.threshold;
    int min_body    = args.min_body_chars;
    int max_body    = args.max_body_chars;

    // ── Step 1: Multi-branch traversal §8.2 items 3-5 ─────────────────

    auto leaves = traverse_for_store(args.memdir, memory_text,
                                     model, storage, threshold,
                                     min_body, max_body);

    // ── Step 2: Placement decision §8.2 item 6, §6.4 ─────────────────

    if (leaves.empty()) {
        // All branches pruned at every level — try root-level fallback
        auto stored_path = attempt_new_folder_store(
            args.memdir, memory_text, model, threshold, min_body, max_body);
        if (stored_path.empty()) {
            std::fprintf(stderr,
                "[STORE_TRAVERSAL_FAILED] No qualifying folder found at \"%s\".\n"
                "All sub-folders scored below threshold (%d) "
                "and new-folder fallback failed.\n"
                "The memory tree does not contain a suitable destination "
                "for this memory.\n",
                args.memdir.c_str(), threshold);
            return 4;
        }
        return 0;
    }

    // If traversal already stored via fallback, we're done
    if (leaves[0].stored) return 0;

    // Assemble placement prompt
    std::vector<std::pair<std::string, std::vector<std::string>>>
        candidate_leaves;
    for (const auto& leaf : leaves) {
        candidate_leaves.emplace_back(leaf.path, leaf.file_list);
    }

    std::string placement_system = mdmem::placement_system_prompt();
    std::string placement_user = mdmem::placement_user_prompt(
        memory_text, candidate_leaves);
    std::string placement_resp = model.infer(placement_system, placement_user);

    std::string selected_path = parse_placement(placement_resp);
    if (selected_path.empty()) {
        std::fprintf(stderr,
            "[STORE_PARSE_FAILED] Failed to parse placement decision. "
            "Response:\n%s\n",
            placement_resp.c_str());
        return 4;
    }

    // Validate the LLM-selected path actually exists
    if (!StorageEngine::is_directory(selected_path)) {
        std::fprintf(stderr,
            "[STORE_PARSE_FAILED] Selected path does not exist "
            "or is not a directory: %s\n",
            selected_path.c_str());
        return 4;
    }

    // ── Step 3: Classification + file write §8.2 item 7, §7.3.4-§7.3.5 ──

    std::string current_date = make_date();
    if (!store_file_at_path(selected_path, memory_text, current_date,
                            model, min_body, max_body)) {
        return 4;
    }

    return 0;
}
