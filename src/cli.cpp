#include "cli.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

// ── help text (exact match to SPEC6 §3.4) ───────────────────────────────────

static void print_help() {
    std::cout << R"(mdmem — Agentic Memory System (CLI)

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
)";
}

// ── helpers ─────────────────────────────────────────────────────────────────

[[noreturn]] static void die(const std::string& msg, int code) {
    std::cerr << msg << '\n';
    std::exit(code);
}

static void require_value(int& i, int argc, char* argv[],
                          const std::string& flag) {
    if (i + 1 >= argc)
        die("error: " + flag + " requires a value", 1);
}

static int parse_int(const std::string& raw, const std::string& flag) {
    try {
        return std::stoi(raw);
    } catch (const std::invalid_argument&) {
        die("error: " + flag + " expects an integer, got '" + raw + "'", 1);
    } catch (const std::out_of_range&) {
        die("error: " + flag + " value '" + raw + "' is out of range", 1);
    }
    return 0; // unreachable
}

// ── main parser ─────────────────────────────────────────────────────────────

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    // defaults
    args.threads = std::max(1, static_cast<int>(
        std::thread::hardware_concurrency() / 2));
    args.ctx_size      = 65536;
    args.threshold     = 50;
    args.max_tokens    = 1024;
    args.min_body_chars = 10;
    args.max_body_chars = 500;
    args.help          = false;

    // no-args → show help, exit 0
    if (argc <= 1) {
        print_help();
        std::exit(0);
    }

    // flag dispatch loop
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            args.help = true;
        } else if (arg == "-q" || arg == "--query") {
            require_value(i, argc, argv, std::string(arg));
            args.query = argv[++i];
        } else if (arg == "-s" || arg == "--store") {
            require_value(i, argc, argv, std::string(arg));
            args.store = argv[++i];
        } else if (arg == "-m" || arg == "--memdir") {
            require_value(i, argc, argv, std::string(arg));
            args.memdir = argv[++i];
        } else if (arg == "--model") {
            require_value(i, argc, argv, std::string(arg));
            args.model = argv[++i];
        } else if (arg == "--threads") {
            require_value(i, argc, argv, std::string(arg));
            args.threads = parse_int(argv[++i], std::string(arg));
        } else if (arg == "--ctx-size") {
            require_value(i, argc, argv, std::string(arg));
            args.ctx_size = parse_int(argv[++i], std::string(arg));
        } else if (arg == "-t" || arg == "--threshold") {
            require_value(i, argc, argv, std::string(arg));
            args.threshold = parse_int(argv[++i], std::string(arg));
        } else if (arg == "--max-tokens") {
            require_value(i, argc, argv, std::string(arg));
            args.max_tokens = parse_int(argv[++i], std::string(arg));
        } else if (arg == "--min-body-chars") {
            require_value(i, argc, argv, std::string(arg));
            args.min_body_chars = parse_int(argv[++i], std::string(arg));
        } else if (arg == "--max-body-chars") {
            require_value(i, argc, argv, std::string(arg));
            args.max_body_chars = parse_int(argv[++i], std::string(arg));
        } else {
            die("error: unknown option: " + std::string(arg), 1);
        }
    }

    // --help or -h → show help, exit 0
    if (args.help) {
        print_help();
        std::exit(0);
    }

    // ── validation (SPEC6 §3.3) ─────────────────────────────────────────

    // exactly one of --query/--store
    if (!args.query.has_value() && !args.store.has_value()) {
        die("error: exactly one of --query or --store must be provided", 1);
    }
    if (args.query.has_value() && args.store.has_value()) {
        die("error: exactly one of --query or --store must be provided, not both", 1);
    }

    // --memdir required
    if (args.memdir.empty()) {
        die("error: --memdir is required", 1);
    }

    // --model required
    if (args.model.empty()) {
        die("error: --model is required", 1);
    }

    // --memdir must exist and be a readable directory
    if (!fs::exists(args.memdir) || !fs::is_directory(args.memdir)) {
        die("error: --memdir must exist and be a readable directory: " + args.memdir, 2);
    }

    // --model must point to an existing file
    if (!fs::exists(args.model) || !fs::is_regular_file(args.model)) {
        die("error: --model file not found: " + args.model, 6);
    }

    // range clamping
    if (args.threads < 1) args.threads = 1;
    if (args.threshold < 0) args.threshold = 0;
    if (args.threshold > 100) args.threshold = 100;
    if (args.ctx_size < 1) args.ctx_size = 65536;

    return args;
}
