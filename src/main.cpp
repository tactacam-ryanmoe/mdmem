#include "main.hpp"
#include "cli.hpp"
#include "model_engine.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    CliArgs args = parse_args(argc, argv);

    ModelEngine engine(args.model, args.threads, args.ctx_size, args.max_tokens);

    std::string response = engine.infer("You are a helpful assistant.", "Say hello.");
    std::cout << response << '\n';

    return 0;
}
