#include "model_engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

ModelEngine::ModelEngine(const std::string& model_path, int n_threads, int n_ctx, int n_predict)
    : n_predict_(n_predict), n_ctx_(n_ctx)
{
    // Load model
    auto model_params = llama_model_default_params();
    model_ = llama_model_load_from_file(model_path.c_str(), model_params);
    if (model_ == nullptr) {
        std::fprintf(stderr, "Error: failed to load model from '%s'\n", model_path.c_str());
        std::exit(6);
    }

    // Create context
    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx     = static_cast<uint32_t>(n_ctx);
    ctx_params.n_threads = n_threads;
    ctx_params.n_threads_batch = n_threads;

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (ctx_ == nullptr) {
        std::fprintf(stderr, "Error: failed to create context\n");
        llama_model_free(model_);
        model_ = nullptr;
        std::exit(6);
    }

    // Store vocab reference
    vocab_ = llama_model_get_vocab(model_);

    // Create sampler chain with temperature 0.2
    auto sparams = llama_sampler_chain_default_params();
    smpl_ = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl_, llama_sampler_init_temp(0.2f));
    llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
}

ModelEngine::~ModelEngine()
{
    // RAII cleanup: free sampler, context, model in reverse order
    if (smpl_ != nullptr) {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    if (ctx_ != nullptr) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_ != nullptr) {
        llama_model_free(model_);
        model_ = nullptr;
    }
}

void ModelEngine::clear_kv_cache()
{
    // b9045 API: use llama_get_memory + llama_memory_seq_rm to clear KV cache
    // Equivalent to llama_kv_cache_clear() in newer versions
    llama_memory_t mem = llama_get_memory(ctx_);
    // seq_id < 0: match any sequence; p0 < 0: from position 0; p1 < 0: to infinity
    bool ok = llama_memory_seq_rm(mem, -1, -1, -1);
    if (!ok) {
        std::fprintf(stderr, "Warning: failed to clear KV cache\n");
    }
}

std::string ModelEngine::infer(const std::string& system_prompt, const std::string& user_prompt)
{
    // 1. Assemble full prompt
    std::string prompt = system_prompt + user_prompt;

    // 2. Tokenize
    int32_t n_tokens_max = n_ctx_;
    std::vector<llama_token> tokens(static_cast<size_t>(n_tokens_max));

    int32_t n_tokens = llama_tokenize(
        vocab_,
        prompt.c_str(),
        static_cast<int32_t>(prompt.size()),
        tokens.data(),
        n_tokens_max,
        true,   // add_special (BOS)
        false   // parse_special
    );

    if (n_tokens < 0) {
        // Buffer too small — negative return is -needed_size
        int32_t needed = -n_tokens;
        if (needed > n_ctx_) {
            std::fprintf(stderr, "Error: prompt requires %d tokens but context size is %d\n", needed, n_ctx_);
            std::exit(3);
        }
        tokens.resize(static_cast<size_t>(needed));
        n_tokens = llama_tokenize(
            vocab_,
            prompt.c_str(),
            static_cast<int32_t>(prompt.size()),
            tokens.data(),
            needed,
            true,
            false
        );
    }

    if (n_tokens < 0) {
        std::fprintf(stderr, "Error: tokenization failed (returned %d)\n", n_tokens);
        std::exit(3);
    }

    tokens.resize(static_cast<size_t>(n_tokens));

    // 3. Decode initial prompt
    auto batch = llama_batch_get_one(tokens.data(), n_tokens);
    int32_t decode_rc = llama_decode(ctx_, batch);

    if (decode_rc < 0) {
        std::fprintf(stderr, "Error: llama_decode failed (returned %d)\n", decode_rc);
        std::exit(3);
    }
    if (decode_rc > 0) {
        std::fprintf(stderr, "Warning: partial context overflow in llama_decode (%d tokens dropped)\n", decode_rc);
    }

    // 4. Generate via sampler loop
    std::vector<llama_token> response_tokens;
    response_tokens.reserve(static_cast<size_t>(n_predict_));

    llama_token eos_token = llama_vocab_eos(vocab_);
    llama_token eot_token = llama_vocab_eot(vocab_);

    for (int i = 0; i < n_predict_; ++i) {
        // Sample next token using the last position
        llama_token new_token = llama_sampler_sample(smpl_, ctx_, -1);

        // Check for end-of-generation
        if (new_token == eos_token || new_token == eot_token) {
            break;
        }

        response_tokens.push_back(new_token);

        // Feed the token back for next iteration
        // llama_batch_get_one returns by-value; no free needed
        auto single_batch = llama_batch_get_one(&new_token, 1);
        decode_rc = llama_decode(ctx_, single_batch);

        if (decode_rc < 0) {
            std::fprintf(stderr, "Error: llama_decode failed during generation (returned %d)\n", decode_rc);
            std::exit(3);
        }
        if (decode_rc > 0) {
            std::fprintf(stderr, "Warning: partial overflow during generation (%d tokens dropped)\n", decode_rc);
        }
    }

    // 5. Detokenize response
    std::string response;
    if (!response_tokens.empty()) {
        // Estimate buffer size: each token can produce up to ~16 bytes of text
        size_t buf_size = response_tokens.size() * 16 + 1;
        std::vector<char> text_buf(buf_size);

        int32_t n_chars = llama_detokenize(
            vocab_,
            response_tokens.data(),
            static_cast<int32_t>(response_tokens.size()),
            text_buf.data(),
            static_cast<int32_t>(buf_size),
            true,   // remove_special
            false   // unparse_special
        );

        if (n_chars < 0) {
            // Buffer too small
            int32_t needed = -n_chars;
            text_buf.resize(static_cast<size_t>(needed));
            n_chars = llama_detokenize(
                vocab_,
                response_tokens.data(),
                static_cast<int32_t>(response_tokens.size()),
                text_buf.data(),
                needed,
                true,
                false
            );
        }

        if (n_chars >= 0) {
            response.assign(text_buf.data(), static_cast<size_t>(n_chars));
        } else {
            std::fprintf(stderr, "Error: detokenization failed (returned %d)\n", n_chars);
            std::exit(3);
        }
    }

    // 6. Clear KV cache for next call
    clear_kv_cache();

    // 7. Return response
    return response;
}
