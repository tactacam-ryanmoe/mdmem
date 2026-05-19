#include "prompts.hpp"

#include <sstream>

namespace mdmem {

// ═══════════════════════════════════════════════════════════════════
//  7.3.1  Folder Scoring
// ═══════════════════════════════════════════════════════════════════

const char* folder_scoring_system_prompt()
{
    return
        "You are a memory folder scoring assistant. You are given a user's request and a list of\n"
        "folder names at the current level of a directory hierarchy. The user's memories are stored\n"
        "as .md files in leaf folders. Your job is to score each folder on how likely it contains\n"
        "memories relevant to the user's request.\n"
        "\n"
        "Rules:\n"
        "- Score each folder from 0 to 100, where 100 means \"definitely relevant\" and 0 means\n"
        "  \"completely irrelevant\".\n"
        "- Return one line per folder in the exact format: <folder_name>:<score>\n"
        "- Do NOT include any other text, explanations, reasoning, or formatting.\n"
        "- Do NOT add blank lines, markdown, quotes, or bullet points.\n"
        "- If a folder name clearly matches the topic, give it 80+.\n"
        "- Partially related folders get 30–79.\n"
        "- Unrelated folders get 0–29.\n"
        "- You MUST score EVERY folder listed. Omitted folders are treated as score 0.\n";
}

std::string folder_scoring_user_prompt(
    const std::string& request,
    const std::vector<std::string>& folders)
{
    std::ostringstream oss;
    oss << "Request: " << request << "\n"
        << "\n"
        << "Folders at this level (sorted alphabetically):\n";
    for (const auto& f : folders) {
        oss << f << "\n";
    }
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  7.3.2  Leaf Query Synthesis
// ═══════════════════════════════════════════════════════════════════

const char* leaf_query_system_prompt()
{
    return
        "You are a memory retrieval assistant. You are given a user's query and the full\n"
        "contents of memory files stored in a folder. Analyze these memories and provide\n"
        "a direct, concise answer to the query.\n"
        "\n"
        "Rules:\n"
        "- Answer directly based ONLY on the provided memories.\n"
        "- Cite dates when relevant (e.g., \"On April 21, you had...\").\n"
        "- If memories contradict each other, state the conflict explicitly in this format:\n"
        "  [CONFLICT] Memory \"<title>\" (date: YYYY-MM-DD) says \"<brief summary>\" but\n"
        "  Memory \"<other_title>\" (date: YYYY-MM-DD) says \"<brief summary>\". The discrepancy is unresolved.\n"
        "- Do NOT invent or hallucinate information. If the memories don't fully answer\n"
        "  the query, say so explicitly.\n"
        "- If no memory is relevant to the query, respond with exactly:\n"
        "  [NO_RESULTS] No relevant memories found in this folder.\n"
        "- Your entire response will be ingested by another LLM agent, so format it as\n"
        "  clean plain text with clear structure. Use headers, bullet points, or numbered\n"
        "  lists where appropriate for easy machine parsing.\n";
}

std::string leaf_query_user_prompt(
    const std::string& query,
    const std::string& folder_path,
    const std::vector<std::pair<std::string, std::string>>& file_contents,
    size_t files_included,
    size_t files_total)
{
    std::ostringstream oss;
    oss << "Query: " << query << "\n"
        << "\n"
        << "Memory files from folder \"" << folder_path << "\":\n";

    for (size_t i = 0; i < file_contents.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << "\n"
            << "<---FILE SEPARATOR--->\n"
            << "[File: " << file_contents[i].first << "]\n"
            << file_contents[i].second;
    }

    if (files_included < files_total) {
        oss << "\n\n"
            << "[NOTE: Content limited due to size constraint. "
            << files_included << " of " << files_total
            << " files included.]\n";
    }

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  7.3.3 / 6.3  Multi-Branch Final Synthesis
// ═══════════════════════════════════════════════════════════════════

const char* final_synthesis_system_prompt()
{
    return
        "You are a memory response synthesizer. You have received responses from\n"
        "multiple memory folders that were scored as relevant to the user's query.\n"
        "Combine these partial responses into a single coherent answer.\n"
        "\n"
        "Rules:\n"
        "- Answer directly and concisely.\n"
        "- Cite dates when relevant (e.g., \"On April 21, you had...\").\n"
        "- If memories contradict each other, explicitly state the conflict:\n"
        "  [CONFLICT] Memory A (date: X) says \"<summary>\" but Memory B (date: Y)\n"
        "  says \"<summary>\". The discrepancy is unresolved.\n"
        "- Do not invent information not present in the source memories.\n"
        "- Format the entire response as plain text suitable for ingestion by another LLM.\n"
        "- If no relevant information was found across any branches, respond with:\n"
        "  [NO_RESULTS] No relevant memories found for this query.\n";
}

std::string final_synthesis_user_prompt(
    const std::string& query,
    const std::vector<std::pair<std::string, std::string>>& folder_responses)
{
    std::ostringstream oss;
    oss << "Query: " << query << "\n"
        << "\n"
        << "Responses from relevant memory folders:\n";

    for (const auto& [path, response] : folder_responses) {
        oss << "\n"
            << "[Folder: " << path << "]\n"
            << response << "\n";
    }

    oss << "\n"
        << "Synthesize these into a single answer.\n";

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  7.3.4  Store Classification & Metadata
// ═══════════════════════════════════════════════════════════════════

std::string classification_system_prompt(
    const std::string& current_date,
    int min_body,
    int max_body)
{
    std::ostringstream oss;
    oss << "You are a memory classification assistant. You are given a natural language\n"
        << "description of an event or fact from a user's life. Extract structured information\n"
        << "to create a memory file.\n"
        << "\n"
        << "Your job is to determine:\n"
        << "1. A short descriptive title (one line)\n"
        << "2. The event date (YYYY-MM-DD). If no specific date can be determined, use\n"
        << "   today's date (UTC): " << current_date << ".\n"
        << "3. A normalized body text written in third-person past tense where applicable.\n"
        << "   Remove conversational filler. Keep it concise but preserve all factual details.\n"
        << "   The body MUST be between " << min_body << " and " << max_body << " characters long.\n"
        << "4. A comma-separated list of 2–5 relevant tags. Tags may contain colons in values\n"
        << "   (e.g., \"time:9am\") for structured tag semantics.\n"
        << "5. A short URL-safe slug for the filename (lowercase, hyphens only, no spaces).\n"
        << "\n"
        << "Rules:\n"
        << "- The slug should be brief and descriptive (e.g., \"eggs-breakfast\", \"meeting-client-x\").\n"
        << "- The slug MUST contain only lowercase letters, digits, and hyphens.\n"
        << "- The body MUST be between " << min_body << " and " << max_body << " characters. Count\n"
        << "  all characters including spaces and punctuation.\n"
        << "- Return your response in EXACTLY this format, one field per line:\n"
        << "  TITLE:<title text>\n"
        << "  DATE:<YYYY-MM-DD>\n"
        << "  BODY:<normalized body text>\n"
        << "  TAGS:<tag1,tag2,...>\n"
        << "  SLUG:<slug>\n"
        << "- Do NOT include any other text, explanations, or formatting.\n";
    return oss.str();
}

std::string classification_user_prompt(
    const std::string& memory_text,
    const std::string& target_path,
    const std::vector<std::string>& existing_files)
{
    std::ostringstream oss;
    oss << "Memory to store: " << memory_text << "\n"
        << "\n"
        << "Existing files in folder \"" << target_path << "\":\n";
    for (const auto& f : existing_files) {
        oss << f << "\n";
    }
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  7.3.5  Store Retry Emphasis
// ═══════════════════════════════════════════════════════════════════

std::string retry_emphasis(int min_body, int max_body, int actual_count)
{
    std::ostringstream oss;
    oss << "--- IMPORTANT RETRY ---\n"
        << "Your previous response's body did not meet the required length. The body MUST be\n"
        << "between " << min_body << " and " << max_body << " characters long. Your previous body\n"
        << "had " << actual_count << " characters. Adjust the body accordingly while keeping all factual\n"
        << "content. Return EXACTLY the same format as requested.\n";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  6.4  Placement Decision (Store Mode)
// ═══════════════════════════════════════════════════════════════════

const char* placement_system_prompt()
{
    return
        "You are a memory placement assistant. You are given a user's memory text and\n"
        "a list of candidate destination folders with their existing file listings.\n"
        "Select the single folder that is the best destination for this memory.\n"
        "\n"
        "Rules:\n"
        "- Return EXACTLY one line in the format: FOLDER:<path>\n"
        "- Choose the folder whose existing content best fits with this memory.\n"
        "- Do NOT include any other text, explanations, or formatting.\n";
}

std::string placement_user_prompt(
    const std::string& memory_text,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& candidate_leaves)
{
    std::ostringstream oss;
    oss << "Candidate folders (survived multi-branch scoring traversal):\n";
    for (const auto& [path, files] : candidate_leaves) {
        oss << path << "\n";
        oss << "  Existing files:";
        for (const auto& f : files) {
            oss << " " << f;
        }
        oss << "\n";
    }
    oss << "\n"
        << "Memory to store: " << memory_text << "\n";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  7.3.6  New Folder Creation (Store Fallback)
// ═══════════════════════════════════════════════════════════════════

const char* new_folder_system_prompt()
{
    return
        "You are a memory storage assistant. No existing folder at the current level of the\n"
        "memory tree appears relevant to this memory. Suggest a new folder name that best\n"
        "describes the category of this memory.\n"
        "\n"
        "Rules:\n"
        "- Return EXACTLY one line in the format: FOLDER:<name>\n"
        "- The name MUST be descriptive, lowercase, and use hyphens (e.g., \"travel-europe-2025\").\n"
        "- Do NOT include any other text, explanations, or formatting.\n";
}

std::string new_folder_user_prompt(
    const std::string& memory_text,
    const std::string& current_path,
    const std::vector<std::string>& existing_folders)
{
    std::ostringstream oss;
    oss << "Memory to store: " << memory_text << "\n"
        << "\n"
        << "Current path: " << current_path << "\n"
        << "Existing folders at this level:\n";
    for (const auto& f : existing_folders) {
        oss << f << "\n";
    }
    return oss.str();
}

} // namespace mdmem
