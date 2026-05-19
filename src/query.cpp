#include "query.hpp"
#include "prompts.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using mdmem::StorageEngine;

namespace {

// ── Trim helper ───────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size()
           && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start
           && (s[end - 1] == ' ' || s[end - 1] == '\t'
               || s[end - 1] == '\r')) {
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

        std::string name      = trim(line.substr(0, colon));
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

// ── Multi-branch traversal for QUERY §6.1, §6.2 ───────────────────────

// Returns a vector of (folder_path, leaf_response) for every leaf
// reached during traversal. Leaves that return [NO_RESULTS] are
// included — filtering happens in run_query().
std::vector<std::pair<std::string, std::string>> traverse_for_query(
    const std::string& path,
    const std::string& query,
    ModelEngine& model,
    StorageEngine& storage,
    int threshold,
    size_t leaf_max_chars)
{
    // ── Branch level: score subdirectories and recurse §6.1 ──────────

    if (StorageEngine::has_subdirs(path)) {
        auto all_entries = StorageEngine::list_directory(path);
        std::vector<std::string> subdir_names;
        for (const auto& entry : all_entries) {
            std::string full = path + "/" + entry;
            if (StorageEngine::is_directory(full)) {
                subdir_names.push_back(entry);
            }
        }

        if (subdir_names.empty()) {
            return {};  // has_subdirs said true but nothing found — safety
        }

        // Score all subdirectory names via LLM
        std::string system = mdmem::folder_scoring_system_prompt();
        std::string user   = mdmem::folder_scoring_user_prompt(
            query, subdir_names);
        std::string response = model.infer(system, user);

        auto scores = parse_folder_scores(response);

        // Follow every folder with score >= threshold (multi-branch)
        std::vector<std::pair<std::string, std::string>> results;
        for (const auto& name : subdir_names) {
            int score = 0;
            auto it = scores.find(name);
            if (it != scores.end()) score = it->second;

            if (score >= threshold) {
                std::string child_path = path + "/" + name;
                auto child_results = traverse_for_query(
                    child_path, query, model, storage,
                    threshold, leaf_max_chars);
                results.insert(results.end(),
                               child_results.begin(), child_results.end());
            }
        }

        return results;  // may be empty if all children were pruned
    }

    // ── Leaf level: .md files → query synthesis §7.3.2 ───────────────

    if (StorageEngine::is_leaf(path)) {
        // List .md files sorted by date, newest first
        auto md_files = StorageEngine::list_md_files(path);

        std::vector<std::pair<std::string, std::string>> included_files;
        size_t total_chars    = 0;
        size_t total_files    = md_files.size();
        size_t skipped_oversize = 0;

        for (const auto& filename : md_files) {
            std::string full_path = path + "/" + filename;
            std::string content   = StorageEngine::read_file(full_path);

            // Parse frontmatter to check body size
            auto fm          = StorageEngine::parse_frontmatter(content);
            size_t body_len  = fm.body.size();

            // Per-file: skip if body exceeds leaf_max_chars §7.3.2
            if (body_len > leaf_max_chars) {
                std::fprintf(stderr,
                    "[WARN] Skipped oversized file \"%s\" "
                    "(%zu chars, limit: %zu)\n",
                    filename.c_str(), body_len, leaf_max_chars);
                ++skipped_oversize;
                continue;
            }

            // Check if adding this file would exceed the char budget §2.3
            if (total_chars + content.size() > leaf_max_chars) {
                break;  // budget exhausted — remaining files excluded
            }

            included_files.emplace_back(filename, content);
            total_chars += content.size();
        }

        // All files skipped (all oversized or no files at all)
        if (included_files.empty()) {
            return { {path,
                std::string("[NO_RESULTS] No relevant memories "
                            "found in this folder.")} };
        }

        // ── Assemble leaf query prompt and infer §7.3.2 ──────────────

        size_t files_included = included_files.size();
        std::string system = mdmem::leaf_query_system_prompt();
        std::string user   = mdmem::leaf_query_user_prompt(
            query, path, included_files, files_included, total_files);
        std::string leaf_response = model.infer(system, user);

        return { {path, leaf_response} };
    }

    // Neither branch nor leaf (empty dir, mixed content, etc.) — skip
    return {};
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  run_query — §8.1 query-mode entry point
// ═══════════════════════════════════════════════════════════════════════

int run_query(const CliArgs& args,
              ModelEngine& model,
              StorageEngine& storage)
{
    if (!args.query.has_value()) {
        // Should not happen — CLI parser enforces --query presence
        std::fprintf(stderr, "error: no query prompt provided\n");
        return 1;
    }

    const std::string& query   = args.query.value();
    int threshold              = args.threshold;
    size_t leaf_max_chars      =
        StorageEngine::compute_leaf_max_chars(args.ctx_size);

    // ── Step 1: Multi-branch traversal §8.1 items 2–4 ─────────────────

    auto leaf_responses = traverse_for_query(
        args.memdir, query, model, storage, threshold, leaf_max_chars);

    // ── Step 2: Filter out [NO_RESULTS] leaves §8.1 item 5 ────────────

    std::vector<std::pair<std::string, std::string>> valid_responses;
    for (const auto& [path, resp] : leaf_responses) {
        if (resp.empty()) continue;
        // Skip leaves that returned [NO_RESULTS]
        if (resp.rfind("[NO_RESULTS]", 0) == 0) continue;
        valid_responses.emplace_back(path, resp);
    }

    // ── Step 3: Output §8.1 items 6–7, §6.3, §7.3.3 ──────────────────

    // If no branches qualified at any point (empty leaf_responses)
    // or all leaves returned [NO_RESULTS] → valid [NO_RESULTS] response
    if (valid_responses.empty()) {
        std::cout
            << "[NO_RESULTS] No relevant memories found for this query.\n";
        return 0;
    }

    if (valid_responses.size() == 1) {
        // Exactly one valid leaf response → output directly
        std::cout << valid_responses[0].second << '\n';
        return 0;
    }

    // Multiple valid responses → final multi-branch synthesis §6.3, §7.3.3
    std::string system = mdmem::final_synthesis_system_prompt();
    std::string user   = mdmem::final_synthesis_user_prompt(
        query, valid_responses);
    std::string final_response = model.infer(system, user);

    std::cout << final_response << '\n';
    return 0;
}
