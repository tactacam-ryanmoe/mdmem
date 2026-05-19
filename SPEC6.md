# mdmem — Agentic Memory System (CLI)

**Status:** Spec v6 — Implemented via Kanban
**Language:** C++20
**LLM Engine:** llama.cpp (embedded, CPU-only GGUF inference)
**Project:** /home/ai/projects/mdmem
**GitHub:** https://github.com/tactacam-ryanmoe/mdmem.git
**Branch:** feat/spec6
**Kanban Board:** mdmem

---

## 1. Overview

`mdmem` is a single-binary C++20 CLI application that stores and retrieves natural language memories as structured Markdown files on disk. It embeds `llama.cpp` directly for local, CPU-based inference — no network calls, no API keys, no external dependencies beyond the GGUF model file. The app orchestrates file I/O and in-process LLM calls.

**Target users:** Agentic AI systems that need a durable, tree-indexed text memory backend. The returned responses are structured for direct ingestion into another LLM's context window.

**Core principle:** The directory tree is the index. At each non-leaf level, all subfolder names are scored by the LLM (0–100%). Branches above a configurable threshold are followed recursively. At leaf nodes, all `.md` file contents are sent to the LLM for synthesis (query) or a new `.md` is written (store).

**Changes from SPEC5:**
- Pinned llama.cpp to release tag `b9045`.
- Default `--ctx-size` raised from 4096 to 65536.
- Locking: query uses shared lock (`LOCK_SH`), store uses exclusive (`LOCK_EX`). Blocking wait instead of exit-on-conflict.
- Temperature changed from 0.0 to 0.2 for improved output quality on small models.
- `leaf_max_chars` token-to-char factor adjusted from 3.5 to 3.0 (more conservative).
- Store mode now traverses multi-branch (all folders ≥ threshold), not single-path. Dead-end branches are pruned; surviving leaves are evaluated together for final placement decision.
- Consolidated store fallback logic (§8.3/§8.4 → §8.3).
- KV cache: use `llama_kv_cache_clear()` for performance (O(1) vs realloc).
- Explicit per-API-call error handling in Model Engine.
- Makefile discovers llama.cpp libraries dynamically instead of hardcoding names.

---

## 2. Architecture

### 2.1 Components

| Component | Responsibility |
|---|---|
| CLI Parser | Parse arguments, validate, route to subcommands |
| Storage Engine | File I/O, directory traversal, frontmatter parsing, locking |
| Model Engine | Load/unload GGUF model via llama.cpp; manage context window and inference lifecycle |
| Prompt Engine | Assemble prompt templates (hardcoded `constexpr` strings) with strict format enforcement and deterministic output expectations |
| Query Mode | Multi-branch folder scoring → leaf-node synthesis |
| Store Mode | Multi-branch folder scoring → surviving leaves evaluated together → single-path placement decision |

### 2.2 Data Flow

```
CLI args → validate → load model → [Storage + Model Engine] → result to stdout → unload model
```

No background processes, daemons, network calls, or persistent state beyond the filesystem. The GGUF model is loaded in-process on startup and freed on exit.

### 2.3 Context Window Budget

The `--ctx-size` flag controls llama.cpp's total context window (in tokens). The app partitions it:

- **~20% reserved** for system prompt, user prompt template, JSON/file headers, and response output.
- **~80% available** for `.md` file content at leaf nodes during query traversal.

The derived leaf content limit is calculated as:

```
leaf_max_chars = ctx_size * 0.8 * 3.0
```

This uses an average of 3.0 characters per token for English text (conservative estimate accounting for frontmatter). Files are sorted by date (newest first) and accumulated until the character budget would be exceeded; subsequent files are skipped with a stderr warning.

---

## 3. CLI Specification

### 3.1 Entry Point

```bash
mdmem --query "<prompt>" [options]
mdmem --store "<memory text>" [options]
```

Two subcommands. No nesting. All options are global unless noted.

### 3.2 Global Options

| Flag | Short | Type | Required | Default | Description |
|---|---|---|---|---|---|
| `--query` | `-q` | string | *either --query or --store* | — | Natural language query prompt |
| `--store` | `-s` | string | *either --query or --store* | — | Natural language memory text to store |
| `--memdir` | `-m` | string | **yes** | — | Root path to the memory folder tree |
| `--model` | — | string | **yes** | — | Path to local `.gguf` model file |
| `--threads` | — | int | no | physical cores / 2 | Number of CPU threads for inference (default: `std::thread::hardware_concurrency() / 2`, minimum 1) |
| `--ctx-size` | — | int | no | `65536` | Total context window size in tokens. Derives leaf content budget automatically |
| `--threshold` | `-t` | int | no | `50` | Minimum folder score (0–100) to follow a branch during traversal |
| `--max-tokens` | — | int | no | `1024` | Max tokens per LLM response call |
| `--min-body-chars` | — | int | no | `10` | Minimum character length in stored memory body. Two-pass validation with retry on first failure |
| `--max-body-chars` | — | int | no | `500` | Maximum character length in stored memory body. Two-pass validation with retry on first failure |
| `--help` | `-h` | flag | no | — | Print usage information and exit 0 |

### 3.3 Validation Rules

- Exactly one of `--query` or `--store` must be provided. If neither or both → error, exit 1.
- `--memdir` and `--model` are mandatory. Missing any → error, exit 1.
- `--memdir` must exist and be a readable directory. Else → error, exit 2.
- `--model` must point to an existing file. Else → error, exit 6. The app does not validate GGUF format beyond existence; llama.cpp handles format errors.

### 3.4 Help Output

Running `mdmem -h` (or with no args) prints:

```text
mdmem — Agentic Memory System (CLI)

USAGE:
  mdmem --query "<prompt>" [options]   Search memories and synthesize a response
  mdmem --store "<memory text>" [options]  Store a new memory

OPTIONS:
  -q, --query <text>                   Natural language query prompt
  -s, --store <text>                   Memory text to store
  -m, --memdir <path>                  Root path to the memory folder tree (required)
      --model <path>                   Path to local .gguf model file (required)
      --threads <N>                    CPU threads for inference (default: hardware_concurrency()/2)
      --ctx-size <tokens>              Total context window in tokens (default: 65536)
  -t, --threshold <0-100>              Minimum folder score to follow a branch (default: 50)
      --max-tokens <int>               Max tokens per LLM response call (default: 1024)
      --min-body-chars <n>             Min characters in stored memory body (default: 10)
      --max-body-chars <n>             Max characters in stored memory body (default: 500)
  -h, --help                           Show this help message and exit

EXIT CODES:
  0  Success
  1  Usage error (missing/invalid arguments)
  2  File system error (cannot read memdir, create file, etc.)
  3  LLM inference error (OOM, context overflow, model crash)
  4  Parse or validation error (LLM returned unexpected format or failed two-pass validation)
  6  Model error (GGUF file not found, load failure, OOM on init)

EXAMPLES:
  mdmem -q "What did I eat last Tuesday?" -m ~/memories --model /models/phi-3-mini-4k-instruct-q4.gguf
  mdmem -s "I had eggs for breakfast on Tuesday" -m ~/memories --model /models/phi-3-mini-4k-instruct-q4.gguf

ERRORS go to stderr. Successful output goes to stdout.
```

---

## 4. Memory `.md` File Format

Every memory is a single `.md` file with YAML frontmatter and a human-readable body. The filename serves as the unique identifier — no UUID field is stored in frontmatter. Information already captured by the filesystem (folder name, relative path) is not duplicated in frontmatter to facilitate future reorganization.

### 4.1 Structure

```markdown
---
date: <YYYY-MM-DD>
created: <YYYY-MM-DDTHH:MM:SSZ>
tags: <comma-separated keywords>
---

# <Title>

<Body text>
```

### 4.2 Field Rules

| Field | Source | Description |
|---|---|---|
| `date` | LLM (extracted) | The event date from the memory content. If undeterminable, use today's UTC date. Format: `YYYY-MM-DD`. |
| `created` | App (generated) | UTC timestamp when written to disk. Format: `YYYY-MM-DDTHH:MM:SSZ`. |
| `tags` | LLM (generated) | Comma-separated keywords for context. May contain colons in values (e.g., `time: 9am`). Frontmatter parser splits on first colon only, so colons in values are safe. Optional. |
| Title | LLM (generated) | One-line descriptive title. |
| Body | LLM (normalized) | The memory text, normalized to third-person past tense where applicable. Cleaned of conversational filler. Subject to `--min-body-chars` / `--max-body-chars` validation with two-pass retry. |

### 4.3 Naming Convention

```
<YYYY-MM-DD>_<slug>.md
```

- `YYYY-MM-DD` matches the `date` field.
- `slug` is a brief, lowercase, hyphen-separated identifier (e.g., `eggs-breakfast`). Generated by the LLM during store. **The app sanitizes the slug** — any characters not matching `[a-z0-9-]` are stripped before writing.
- The filename is the unique identifier for each memory.
- **Collision resolution:** If a file with the same name already exists in the target directory, the app appends `-1`, `-2`, etc. (`2026-04-21_eggs-breakfast-1.md`).

### 4.4 Frontmatter Parsing

The storage engine parses frontmatter by:
1. Reading lines until the first `---` delimiter.
2. If the file starts with `---`, reading until the second `---`.
3. Splitting on `:` for key-value extraction (split on **first colon only** — values may contain colons).
4. The body begins after the closing `---` and a blank line.

The engine exposes these fields to the query/store logic. Unknown keys are silently ignored.

---

## 5. Storage Layout

### 5.1 Structure

```
<memdir>/
├── .mdmem.lock                    ← flock lock file (see §9)
├── <category-1>/                  ← LLM-managed
│   ├── <subfolder>/               ← can nest arbitrarily deep
│   │   └── 2026-04-15_*.md
│   └── 2026-04-01_*.md
└── <category-2>/
    └── 2026-03-10_*.md
```

### 5.2 Leaf Constraint

**A directory is either a branch or a leaf, never both.**
- **Branch:** Contains only subdirectories (no `.md` files). The LLM scores folder names.
- **Leaf:** Contains only `.md` files (no subdirectories). The LLM processes file contents.

The app enforces this: during store, if the LLM selects a branch directory, it continues traversal. Only leaf directories receive new `.md` writes.

### 5.3 App-Directories vs LLM-Directories

- The app **reads** directory listings and sends folder names to the LLM.
- The app **creates** directories only when instructed by the LLM during store mode.
- The app **never** renames or deletes directories on its own.

---

## 6. Traversal Algorithm

Both query and store use multi-branch scoring traversal with different leaf-node actions.

### 6.1 High-Level Flow

```
Start at --memdir root
  ↓
For each level:
  Read directory entries
    ↓
  If branch (subdirs only):
    Send folder names to LLM → receive scores (0–100)
    Keep folders with score ≥ --threshold
    Recurse into each kept folder
    If no folders meet threshold → prune this branch entirely
    ↓
  If leaf (.md files only):
    Query: send all .md contents to LLM → synthesize response
    Store: collect file listings for later placement decision
```

### 6.2 Pseudocode

```
function traverse(path, prompt, mode):
    entries = list_directory(path)

    if entries contains subdirectories:
        scores = llm_score_folders(prompt, [subdir names])
        candidates = filter folders where score >= threshold

        if candidates is empty:
            return nil  // branch pruned — no longer relevant

        results = []
        for each candidate in candidates:
            child_results = traverse(candidate.path, prompt, mode)
            if child_results not nil:
                results.append(child_results)

        return results  // may be empty if all children were pruned at deeper levels

    else (leaf - only .md files):
        if mode == QUERY:
            contents = read_all_md_files(path, limited_by derived_leaf_max_chars)
            return llm_leaf_query(prompt, contents)

        else if mode == STORE:
            // Collect file listings from surviving leaves for placement decision
            return { path: path, files: list_md_files(path) }
```

### 6.3 Final Synthesis (Query Mode)

If multiple leaf branches returned results, a final synthesis call merges them:

**System prompt:**
```
You are a memory response synthesizer. You have received responses from
multiple memory folders that were scored as relevant to the user's query.
Combine these partial responses into a single coherent answer.

Rules:
- Answer directly and concisely.
- Cite dates when relevant (e.g., "On April 21, you had...").
- If memories contradict each other, explicitly state the conflict:
  [CONFLICT] Memory A (date: X) says "<summary>" but Memory B (date: Y)
  says "<summary>". The discrepancy is unresolved.
- Do not invent information not present in the source memories.
- Format the entire response as plain text suitable for ingestion by another LLM.
- If no relevant information was found across any branches, respond with:
  [NO_RESULTS] No relevant memories found for this query.
```

### 6.4 Store Placement Decision (Store Mode)

After multi-branch traversal collects all surviving leaf nodes, the app sends a placement decision prompt to the LLM containing file listings from each qualifying leaf. The LLM selects the single best destination directory.

**System prompt:**
```
You are a memory placement assistant. You are given a user's memory text and
a list of candidate destination folders with their existing file listings.
Select the single folder that is the best destination for this memory.

Candidate folders (survived multi-branch scoring traversal):
<folder_path_1>
  Existing files: <file_list_1>
<folder_path_2>
  Existing files: <file_list_2>
...

Rules:
- Return EXACTLY one line in the format: FOLDER:<path>
- Choose the folder whose existing content best fits with this memory.
- Do NOT include any other text, explanations, or formatting.
```

**User prompt:**
```
Memory to store: <user's memory text>
```

If no leaf nodes survived (all branches were pruned at some depth), the new-folder fallback (§8.3) is triggered at the memdir root.

---

## 7. LLM Integration

### 7.1 Model Engine (llama.cpp)

All LLM calls are in-process through the llama.cpp `common` API. No network involved.

**Pinned version:** llama.cpp release tag `b9045`. All API references correspond to this release.

**Initialization:**
- On startup, load the GGUF model via `llama_load_model_from_file()` using `--model` path.
- Configure inference parameters: temperature = 0.2, `n_threads` from `--threads`, `n_ctx` from `--ctx-size`, `n_predict` from `--max-tokens`.
- Create a single `llama_context` and reuse it for all calls during the invocation.
- No warm-up checks. Load the model and proceed immediately.

**Per-call pattern:**
1. Assemble prompt (system + user) into a string.
2. Convert to tokens via `llama_tokenize()` — check return value; on failure, log error to stderr and exit 3.
3. Submit to context via `llama_decode()` — check return value:
   - `0`: success.
   - Positive integer: partial context overflow (only some batches decoded). Log warning to stderr and proceed with what was decoded.
   - Negative integer: complete failure. Log error to stderr and exit 3.
4. Generate response via `llama_sampler` loop until stop condition or `max_tokens` reached.
5. Detokenize and return response text.
6. Reset context for the next call using `llama_kv_cache_clear()` — O(1) operation, preferred over freeing/recreating the context. Check for errors on clear.

**Context management:**
- After each LLM call, clear the KV cache via `llama_kv_cache_clear()` to reset the context window for the next prompt. This ensures each folder-scoring pass, leaf query, and classification call starts with a fresh context budget.
- If a single prompt + file content exceeds `--ctx-size`, the app logs a warning to stderr and truncates input to fit, then proceeds.

**Shutdown:**
- On normal or abnormal exit, free the llama context and model via destructors (RAII wrapper). No explicit cleanup command needed.

### 7.2 Prompt Engineering Principles

**Prompt templates are critical path.** Every prompt must be designed for maximum determinism and machine-parseable output:

1. **Explicit format strings** — specify exact prefixes (e.g., `FOLDER:`, `TITLE:`, `BODY:`) and one-field-per-line structure
2. **Negative constraints** — "Do NOT include any other text, explanations, or formatting"
3. **No optional commentary** — the LLM must not add preamble, reasoning, or postamble unless explicitly requested
4. **Grace window on parsing** — tolerate minor whitespace variance (leading/trailing spaces), but reject structural deviations
5. **Line-by-line prefix matching** — every response uses `<PREFIX>:<value>` format for reliable extraction

### 7.3 Prompt Templates

#### 7.3.1 Folder Scoring (Branch Level) — Query & Store

**System prompt:**
```
You are a memory folder scoring assistant. You are given a user's request and a list of
folder names at the current level of a directory hierarchy. The user's memories are stored
as .md files in leaf folders. Your job is to score each folder on how likely it contains
memories relevant to the user's request.

Rules:
- Score each folder from 0 to 100, where 100 means "definitely relevant" and 0 means
  "completely irrelevant".
- Return one line per folder in the exact format: <folder_name>:<score>
- Do NOT include any other text, explanations, reasoning, or formatting.
- Do NOT add blank lines, markdown, quotes, or bullet points.
- If a folder name clearly matches the topic, give it 80+.
- Partially related folders get 30–79.
- Unrelated folders get 0–29.
- You MUST score EVERY folder listed. Omitted folders are treated as score 0.
```

**User prompt:**
```
Request: <user's query or memory text>

Folders at this level (sorted alphabetically):
<folder_name_1>
<folder_name_2>
...
```

**Response format:**
```
personal_events:85
work_projects:12
recipes:67
gym_workouts:5
```

**Parsing:** The app splits on newlines, then extracts `<name>:<integer>` from each line using prefix matching. Folders not mentioned by the LLM are treated as score 0. Minor whitespace around scores is tolerated (trimmed).

#### 7.3.2 Leaf Node — Query Synthesis

The app constructs a single prompt containing all relevant `.md` file contents from one leaf folder.

**System prompt:**
```
You are a memory retrieval assistant. You are given a user's query and the full
contents of memory files stored in a folder. Analyze these memories and provide
a direct, concise answer to the query.

Rules:
- Answer directly based ONLY on the provided memories.
- Cite dates when relevant (e.g., "On April 21, you had...").
- If memories contradict each other, state the conflict explicitly in this format:
  [CONFLICT] Memory "<title>" (date: YYYY-MM-DD) says "<brief summary>" but
  Memory "<other_title>" (date: YYYY-MM-DD) says "<brief summary>". The discrepancy is unresolved.
- Do NOT invent or hallucinate information. If the memories don't fully answer
  the query, say so explicitly.
- If no memory is relevant to the query, respond with exactly:
  [NO_RESULTS] No relevant memories found in this folder.
- Your entire response will be ingested by another LLM agent, so format it as
  clean plain text with clear structure. Use headers, bullet points, or numbered
  lists where appropriate for easy machine parsing.
```

**User prompt:**
```
Query: <user's natural language query>

Memory files from folder "<folder_path>":

<---FILE SEPARATOR--->
[File: <filename.md>]
<full file contents including frontmatter>

<---FILE SEPARATOR--->
[File: <filename.md>]
<full file contents including frontmatter>

...
```

**Leaf content limits:** Files are sorted by date (newest first) and accumulated until the total character count would exceed the derived `leaf_max_chars` (see §2.3). The last file that fits is included; subsequent files are skipped. A note is appended to the prompt: `[NOTE: Content limited due to size constraint. <N> of <M> files included.]`

**Per-file validation:** If an individual `.md` file's body exceeds the derived `leaf_max_chars`, it is skipped and a warning is logged to stderr: `[WARN] Skipped oversized file "<filename>" (<char_count> chars, limit: <leaf_max_chars>)`. This ensures no single file can dominate the context window.

#### 7.3.3 Leaf Node — Multi-Branch Final Synthesis

After all qualifying branches return their leaf-level responses, the app sends them together for a final merge (see §6.3).

**User prompt:**
```
Query: <user's query>

Responses from relevant memory folders:

[Folder: <path_1>]
<response from folder 1>

[Folder: <path_2>]
<response from folder 2>

...

Synthesize these into a single answer.
```

#### 7.3.4 Leaf Node — Store Classification & Metadata

When storing, at the selected leaf node (chosen by placement decision in §6.4), the LLM extracts structured metadata from the user's memory text.

**System prompt:**
```
You are a memory classification assistant. You are given a natural language
description of an event or fact from a user's life. Extract structured information
to create a memory file.

The existing files in this folder are:
<list of existing .md filenames only, one per line>

Your job is to determine:
1. A short descriptive title (one line)
2. The event date (YYYY-MM-DD). If no specific date can be determined, use
   today's date (UTC): <current_date>.
3. A normalized body text written in third-person past tense where applicable.
   Remove conversational filler. Keep it concise but preserve all factual details.
   The body MUST be between <min_body_chars> and <max_body_chars> characters long.
4. A comma-separated list of 2–5 relevant tags. Tags may contain colons in values
   (e.g., "time:9am") for structured tag semantics.
5. A short URL-safe slug for the filename (lowercase, hyphens only, no spaces).

Rules:
- The slug should be brief and descriptive (e.g., "eggs-breakfast", "meeting-client-x").
- The slug MUST contain only lowercase letters, digits, and hyphens.
- The body MUST be between <min_body_chars> and <max_body_chars> characters. Count
  all characters including spaces and punctuation.
- Return your response in EXACTLY this format, one field per line:
  TITLE:<title text>
  DATE:<YYYY-MM-DD>
  BODY:<normalized body text>
  TAGS:<tag1,tag2,...>
  SLUG:<slug>
- Do NOT include any other text, explanations, or formatting.
```

**User prompt:**
```
Memory to store: <user's memory text>

Existing files in folder "<target_path>":
<existing_1.md>
<existing_2.md>
...
```

**Parsing:** The app extracts each field by line prefix matching (`TITLE:`, `DATE:`, `BODY:`, `TAGS:`, `SLUG:`). Missing fields cause a parse error (exit 4). The app generates `created` itself.

#### 7.3.5 Store Retry — Classification With Emphasis (Second Pass)

If the first-pass body fails character count validation, the LLM is called again with an amended system prompt:

**Amended system prompt** (appended to the original):
```
--- IMPORTANT RETRY ---
Your previous response's body did not meet the required length. The body MUST be
between <min_body_chars> and <max_body_chars> characters long. Your previous body
had <actual_count> characters. Adjust the body accordingly while keeping all factual
content. Return EXACTLY the same format as requested.
```

If the second pass also fails, the store operation fails with exit code 4. The error message on stderr is formatted for LLM consumption: `[STORE_VALIDATION_FAILED] Body length validation failed after 2 attempts. Expected <min_body_chars>-<max_body_chars> chars, got <actual_count>. Adjust your prompt or relax --min-body-chars / --max-body-chars constraints.`

#### 7.3.6 New Folder Creation (Store Fallback — Any Depth)

When the new-folder creation fallback is triggered:

**System prompt:**
```
You are a memory storage assistant. No existing folder at the current level of the
memory tree appears relevant to this memory. Suggest a new folder name that best
describes the category of this memory.

Rules:
- Return EXACTLY one line in the format: FOLDER:<name>
- The name MUST be descriptive, lowercase, and use hyphens (e.g., "travel-europe-2025").
- Do NOT include any other text, explanations, or formatting.
```

**User prompt:**
```
Memory to store: <user's memory text>

Current path: <current_path>
Existing folders at this level:
<folder_1>
<folder_2>
...
```

The app creates the new folder at the current path and writes the `.md` file into it.

---

## 8. Mode-Specific Logic

### 8.1 Query Mode

1. Validate args, load model, acquire shared lock.
2. Start traversal at `--memdir`.
3. For each branch level, score folders, recurse into qualifying branches.
4. At each qualifying leaf node, send `.md` contents to LLM for synthesis.
5. Collect all non-empty leaf responses.
6. If multiple leaf responses exist, run final multi-branch synthesis (§7.3.3).
7. Output the result to stdout.
8. Release lock, unload model, exit 0.

**If no branches qualify at any point:** Output `[NO_RESULTS] No relevant memories found for this query.` to stdout, exit 0. (Zero results is a valid response, not an error.)

### 8.2 Store Mode

1. Validate args, load model, acquire exclusive lock.
2. Start traversal at `--memdir`.
3. Score folders at each branch level; recurse into ALL folders scoring ≥ threshold (multi-branch).
4. At each branch level where all sub-folders score below threshold, that branch is pruned — no longer relevant.
5. Collect all surviving leaf nodes from multi-branch traversal.
6. **Placement decision:** If one or more leaves survived:
   - Send the placement decision prompt (§6.4) with file listings from each surviving leaf.
   - LLM selects the single best destination directory.
7. At the selected leaf node:
   - List existing `.md` files.
   - Send classification prompt (§7.3.4).
   - Validate body character count against `--min-body-chars` / `--max-body-chars`.
   - On first failure, retry with emphasis (§7.3.5). On second failure, abort (exit 4).
   - Write the new `.md` file with generated metadata.
   - Handle filename collision by appending `-1`, `-2`, etc.
8. Output `stored:<filename>:<date>` to stdout.
9. Release lock, unload model, exit 0.

### 8.3 Store Fallback: New Folder Creation (Any Depth)

When **all** branches score below threshold during store at any depth level — including the final case where multi-branch traversal yields zero surviving leaves:

- The new-folder creation prompt (§7.3.6) is triggered at the current path where the dead end occurred.
- During the initial placement decision (§6.4), if zero leaves survived the entire traversal, trigger fallback at the `--memdir` root.
- If a branch was pruned mid-traversal (all sub-folders scored below threshold at that level), the caller can trigger new-folder creation at that specific path.
- The LLM suggests a folder name; the app creates it as a leaf and writes the `.md` file into it.
- This ensures the leaf constraint (§5.2) is always maintained: sub-folders are never mixed with `.md` files at the same level.

If new-folder creation fails (e.g., LLM returns unparseable response), the app reports an error to stderr:

```
[STORE_TRAVERSAL_FAILED] No qualifying folder found at "<dead_end_path>".
All sub-folders scored below threshold (<threshold>) and new-folder fallback failed.
The memory tree does not contain a suitable destination for this memory.
```

- Exit code: 4 (parse/validation error — the LLM caller should interpret this as "no valid storage path exists").

### 8.4 Body Character Validation

The app validates body text from the LLM against `--min-body-chars` and `--max-body-chars`:

1. Count total characters in the body string (including spaces and punctuation).
2. If count is within `[min, max]` → pass.
3. If outside range on first attempt → retry with emphasis prompt (§7.3.5).
4. If outside range on second attempt → store fails, exit 4.

---

## 9. Concurrency

A single `flock`-based lock file (`<memdir>/.mdmem.lock`) coordinates concurrent access to the same memory dataset. Lock contention causes the requesting process to **block and wait** — it does not return an error.

### 9.1 Lock Modes

| Mode | Lock Type | Behavior |
|---|---|---|
| Query (`--query`) | `LOCK_SH` (shared) | Multiple concurrent queries can hold the lock simultaneously. Blocks only while a store is in progress. |
| Store (`--store`) | `LOCK_EX` (exclusive) | Blocks until no other query or store holds the lock. Then proceeds exclusively. |

### 9.2 Behavior

- On startup, attempt blocking `flock()` with the appropriate lock type for the mode.
- The process waits indefinitely until the lock is acquired. No timeout, no error exit on contention.
- Lock is automatically released when the process exits (normal or abnormal) via file descriptor closure.
- The same `--memdir` value must be accessed by concurrent instances for coordination — different memdirs are independent.

### 9.3 Implementation

```cpp
// Pseudocode
int fd = open(".mdmem.lock", O_CREAT | O_RDWR, 0600);

if (mode == QUERY) {
    flock(fd, LOCK_SH);       // shared lock — blocks until acquired
} else {
    flock(fd, LOCK_EX);       // exclusive lock — blocks until acquired
}

// ... do work ...
// flock released on process exit via fd closure (RAII wrapper)
```

---

## 10. Exit Codes

| Code | Condition | stderr Message |
|---|---|---|
| `0` | Success | — |
| `1` | Usage error (missing args, invalid values) | Description of what's wrong |
| `2` | File system error (cannot read memdir, create file, I/O failure) | OS error + context |
| `3` | LLM inference error (OOM during decode, tokenization failure, decode failure) | Details from specific llama.cpp API call + retry hint |
| `4` | Parse or validation error (LLM returned unexpected format, body validation failed after two passes, store traversal dead-end) | Response snippet for debugging, formatted for LLM consumption |
| `6` | Model error (GGUF file not found, corrupt model, OOM on load) | File path + OS/model error details |

---

## 11. Build System

### 11.1 Dependencies

| Dependency | Type | Notes |
|---|---|---|
| C++20 compiler | Required | `g++ -std=c++20` or `clang++ -std=c++20` |
| llama.cpp | Required (git submodule) | Embedded inference engine at release tag `b9045`. No GPU support compiled in (`GGML_CUDA=OFF`, `GGML_METAL=OFF`). Static link for self-contained binary. Submodule path: `repos/llama.cpp` |

### 11.2 Build Command

```bash
# Step 1: Initialize llama.cpp submodule at release tag b9045 (one-time)
git submodule update --init repos/llama.cpp
cd repos/llama.cpp && git checkout b9045

# Step 2: Build llama.cpp static library (CPU-only, no GPU)
cd repos/llama.cpp && make -j$(nproc) GGML_CUDA=OFF GGML_METAL=OFF BUILD_SHARED_LIBS=0

# Step 3: Discover and link libraries dynamically
# The Makefile scans repos/llama.cpp/build/ for .a files and includes them
# This avoids hardcoding library names that may vary across llama.cpp versions.

g++ -std=c++20 -O2 \
    -I. \
    -Irepos/llama.cpp/include \
    -Irepos/llama.cpp/ggml/include \
    src/main.cpp src/cli.cpp src/storage.cpp src/model_engine.cpp src/query.cpp src/store.cpp \
    -o mdmem \
    $(find repos/llama.cpp/build -name '*.a' | tr '\n' ' ') \
    $(pkg-config --libs --static pthread) \
    -lpthread -lm -ldl
```

Makefile wraps this for convenience. The final binary is self-contained — no runtime dependencies on shared llama libraries.

### 11.3 Project Layout

```
mdmem/
├── SPEC5.md                     ← Previous spec (archived)
├── SPEC6.md                     ← This document (spec v6, current)
├── Makefile
├── repos/
│   └── llama.cpp                ← git submodule (tag: b9045)
├── src/
│   ├── main.cpp                 ← Entry point, argument dispatch, model lifecycle
│   ├── main.hpp                 ← Main declarations
│   ├── cli.cpp                  ← Argument parsing, validation, help text
│   ├── cli.hpp                  ← CLI declarations
│   ├── storage.cpp              ← File I/O, directory traversal, frontmatter parsing, locking
│   ├── storage.hpp              ← Storage declarations
│   ├── model_engine.cpp         ← llama.cpp wrapper: load/unload, inference, context management
│   ├── model_engine.hpp         ← Model engine declarations
│   ├── query.cpp                ← Query mode: multi-branch scoring + synthesis
│   ├── query.hpp                ← Query declarations
│   ├── store.cpp                ← Store mode: multi-branch scoring + placement decision + file creation
│   ├── store.hpp                ← Store declarations
│   ├── prompts.cpp              ← Prompt template strings (constexpr literals)
│   └── prompts.hpp              ← Prompt function declarations
```

---

## 12. Development Workflow

### Phase 1: Foundation
1. Set up llama.cpp submodule at tag `b9045` and verify CPU-only static build produces `.a` libraries.
2. Implement CLI parser with full help text and validation (§3).
3. Implement Model Engine: RAII wrapper around llama.cpp — load GGUF, configure params (temp=0.2, threads, ctx-size), inference loop with per-call error checking (`llama_tokenize`, `llama_decode` return values), KV cache reset via `llama_kv_cache_clear()`, unload on destruct (§7.1). Verify with a simple echo prompt.
4. Implement Storage Engine: directory listing, frontmatter parsing, file I/O, flock locking with shared/exclusive modes (§4, §5, §9).

### Phase 2: Store Mode
5. Implement folder scoring prompt assembly and parsing (§7.3.1).
6. Implement multi-branch traversal for store (all folders ≥ threshold) (§8.2).
7. Implement placement decision prompt across surviving leaves (§6.4).
8. Implement leaf-node classification + `.md` file creation (§7.3.4).
9. Implement two-pass body character validation with retry emphasis (§8.4, §7.3.5).
10. Implement store fallback: new-folder creation at any depth (§8.3).
11. Test end-to-end: store a memory, verify file on disk.

### Phase 3: Query Mode
12. Implement multi-branch scoring traversal (follow all qualifying branches) (§8.1).
13. Implement leaf-node query synthesis with context-derived content limits (§7.3.2).
14. Implement multi-branch final synthesis (§7.3.3).
15. Implement `[NO_RESULTS]` path.
16. Test end-to-end: store memories, query them, verify response quality.

### Phase 4: Polish
17. Error messages, edge cases, context overflow handling.
18. Cross-test: store → query the stored memory → verify retrieval.
19. Final review of help text accuracy and exit code compliance.
20. Prompt template audit — ensure all templates follow engineering principles (§7.2).

---

_Document created: 2026-05-06_
_Author: Claw ⚙️_
