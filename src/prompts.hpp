#pragma once

#include <string>
#include <vector>
#include <utility>

namespace mdmem {

// ═══════════════════════════════════════════════════════════════════
//  Folder Scoring  §7.3.1
// ═══════════════════════════════════════════════════════════════════

const char* folder_scoring_system_prompt();

std::string folder_scoring_user_prompt(
    const std::string& request,
    const std::vector<std::string>& folders);

// ═══════════════════════════════════════════════════════════════════
//  Leaf Query Synthesis  §7.3.2
// ═══════════════════════════════════════════════════════════════════

const char* leaf_query_system_prompt();

std::string leaf_query_user_prompt(
    const std::string& query,
    const std::string& folder_path,
    const std::vector<std::pair<std::string, std::string>>& file_contents,
    size_t files_included,
    size_t files_total);

// ═══════════════════════════════════════════════════════════════════
//  Multi-Branch Final Synthesis  §7.3.3 / §6.3
// ═══════════════════════════════════════════════════════════════════

const char* final_synthesis_system_prompt();

std::string final_synthesis_user_prompt(
    const std::string& query,
    const std::vector<std::pair<std::string, std::string>>& folder_responses);

// ═══════════════════════════════════════════════════════════════════
//  Store Classification & Metadata  §7.3.4
// ═══════════════════════════════════════════════════════════════════

// Returns the system prompt with <current_date>, <min_body_chars>,
// <max_body_chars> placeholders substituted.
std::string classification_system_prompt(
    const std::string& current_date,
    int min_body,
    int max_body);

std::string classification_user_prompt(
    const std::string& memory_text,
    const std::string& target_path,
    const std::vector<std::string>& existing_files);

// ═══════════════════════════════════════════════════════════════════
//  Store Retry Emphasis  §7.3.5
// ═══════════════════════════════════════════════════════════════════

std::string retry_emphasis(int min_body, int max_body, int actual_count);

// ═══════════════════════════════════════════════════════════════════
//  Placement Decision  §6.4
// ═══════════════════════════════════════════════════════════════════

const char* placement_system_prompt();

std::string placement_user_prompt(
    const std::string& memory_text,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& candidate_leaves);

// ═══════════════════════════════════════════════════════════════════
//  New Folder Creation  §7.3.6
// ═══════════════════════════════════════════════════════════════════

const char* new_folder_system_prompt();

std::string new_folder_user_prompt(
    const std::string& memory_text,
    const std::string& current_path,
    const std::vector<std::string>& existing_folders);

} // namespace mdmem
