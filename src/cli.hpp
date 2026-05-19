#ifndef MDMEM_CLI_HPP
#define MDMEM_CLI_HPP

#include <optional>
#include <string>

struct CliArgs {
    std::optional<std::string> query;
    std::optional<std::string> store;
    std::string                memdir;
    std::string                model;
    int                        threads       = 0;
    int                        ctx_size      = 65536;
    int                        threshold     = 50;
    int                        max_tokens    = 1024;
    int                        min_body_chars = 10;
    int                        max_body_chars = 500;
    bool                       help          = false;
};

CliArgs parse_args(int argc, char* argv[]);

#endif // MDMEM_CLI_HPP
