#pragma once
#include <string>
#include "llama.h"

class ModelEngine {
public:
    ModelEngine(const std::string& model_path, int n_threads, int n_ctx, int n_predict);
    ~ModelEngine();

    // Non-copyable
    ModelEngine(const ModelEngine&) = delete;
    ModelEngine& operator=(const ModelEngine&) = delete;

    // Run a single inference call with the given prompt
    // Returns the generated text response
    std::string infer(const std::string& system_prompt, const std::string& user_prompt);

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    llama_sampler* smpl_ = nullptr;

    int n_predict_;
    int n_ctx_;

    void clear_kv_cache();
};
