#include "cli.hpp"
#include "main.hpp"
#include "model_engine.hpp"
#include "query.hpp"
#include "storage.hpp"
#include "store.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    try {
        // ── Parse arguments ──────────────────────────────────────────

        CliArgs args = parse_args(argc, argv);

        // ── Load model ───────────────────────────────────────────────

        ModelEngine engine(args.model, args.threads,
                           args.ctx_size, args.max_tokens);

        // ── Acquire lock §9 ──────────────────────────────────────────

        mdmem::LockMode lock_mode = args.store.has_value()
            ? mdmem::LockMode::EXCLUSIVE
            : mdmem::LockMode::SHARED;

        mdmem::FileLock lock(args.memdir, lock_mode);

        // ── Route to mode ────────────────────────────────────────────

        if (args.store.has_value()) {
            // Store mode: multi-branch traversal → placement →
            // classification → body validation → file write
            mdmem::StorageEngine storage;
            return run_store(args, engine, storage);
        }

        // Query mode: multi-branch scoring → leaf synthesis → merge
        if (args.query.has_value()) {
            mdmem::StorageEngine storage;
            return run_query(args, engine, storage);
        }

        return 0;

    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
