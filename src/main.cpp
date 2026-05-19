#include "main.hpp"
#include "cli.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    CliArgs args = parse_args(argc, argv);

    std::cout << "Mode: " << (args.query.has_value() ? "query" : "store")
              << ", Memdir: " << args.memdir
              << ", Model: " << args.model
              << ", Threads: " << args.threads
              << ", CtxSize: " << args.ctx_size
              << ", Threshold: " << args.threshold
              << ", MaxTokens: " << args.max_tokens
              << ", MinBodyChars: " << args.min_body_chars
              << ", MaxBodyChars: " << args.max_body_chars
              << '\n';

    return 0;
}
