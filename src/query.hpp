#ifndef MDMEM_QUERY_HPP
#define MDMEM_QUERY_HPP

#include "cli.hpp"
#include "model_engine.hpp"
#include "storage.hpp"

// ── Query mode entry point §8.1 ──────────────────────────────────────

// Execute the full query pipeline: multi-branch traversal,
// per-leaf synthesis, optional final multi-branch synthesis.
// Returns exit code (0 on success, including [NO_RESULTS]).
int run_query(const CliArgs& args, ModelEngine& model,
              mdmem::StorageEngine& storage);

#endif // MDMEM_QUERY_HPP
