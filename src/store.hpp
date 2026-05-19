#ifndef MDMEM_STORE_HPP
#define MDMEM_STORE_HPP

#include "cli.hpp"
#include "model_engine.hpp"
#include "storage.hpp"

// ── STORE candidate leaf ────────────────────────────────────────────────

struct LeafCandidate {
    std::string              path;       // absolute leaf directory path
    std::vector<std::string> file_list;  // .md filenames in this leaf
    bool                     stored = false;  // true if fallback already wrote the .md file
};

// ── Store mode entry point §8.2 ────────────────────────────────────────

// Execute the full store pipeline: multi-branch traversal,
// placement decision, classification, body validation, file write.
// Returns exit code (0 on success, 4 on parse/validation failure).
int run_store(const CliArgs& args, ModelEngine& model,
              mdmem::StorageEngine& storage);

#endif // MDMEM_STORE_HPP
