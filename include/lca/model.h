// =============================================================================
//  lca/model.h  --  the agent's reasoning layer
// -----------------------------------------------------------------------------
//  Two brains ship with the agent, both free of paid APIs:
//
//    1. HeuristicBrain  -- built in, offline, deterministic.  It parses the task
//       with an intent grammar (create / implement / fix / refactor / document /
//       test / search / scrape / run / explain), inspects the workspace map and
//       emits concrete tool calls.  It also carries a template library so it can
//       scaffold whole projects without a language model.
//
//    2. LocalModelBrain -- talks to a model server the user runs on their own
//       machine (llama.cpp / llama-server, Ollama, LM Studio, vLLM, ...) over
//       plain HTTP on localhost.  It speaks the OpenAI-compatible chat API and a
//       strict JSON tool protocol.  No hosted service, no API key, no billing.
//
//  A brain never touches the file system or the network directly: it only
//  proposes tool calls, which the agent validates and executes.
// =============================================================================
#ifndef LCA_MODEL_H
#define LCA_MODEL_H

#include "lca/buf.h"
#include "lca/common.h"

namespace lca {

// -----------------------------------------------------------------------------
// Tool calls
// -----------------------------------------------------------------------------
struct ToolCall {
    std::string id;
    std::string tool;
    Json        args;
    std::string rationale;      // why the brain wants this, shown in the trace

    Json to_json() const;
    std::string to_text() const;
};

struct ToolSpec {
    std::string name;
    std::string summary;
    std::string args_help;
    Json        schema;         // JSON-Schema-ish description for local models
};

std::vector<ToolSpec> default_tool_specs();

// -----------------------------------------------------------------------------
// Brain protocol
// -----------------------------------------------------------------------------
struct AgentMessage {
    std::string role;           // "user" | "assistant" | "tool"
    std::string content;
    std::string tool_name;
    bool        ok{true};
    int64_t     when_ms{0};

    Json to_json() const;
    std::string to_text() const;
};

struct BrainContext {
    std::string               task;
    std::string               workspace_summary;
    std::vector<AgentMessage> history;       // oldest first
    std::vector<ToolSpec>     tools;
    int                       step{0};
    int                       max_steps{0};
    std::vector<std::string>  notes;         // scratchpad hints from the agent
};

struct BrainReply {
    std::string           text;              // natural-language part of the reply
    std::vector<ToolCall> calls;             // empty => the brain is finished
    bool                  done{false};       // explicit "finish" signal
    std::string           final_answer;
    int64_t               latency_ms{0};

    std::string to_text() const;
};

class Brain {
public:
    virtual ~Brain() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    // Produces the next step: either tool calls or a final answer.
    virtual Result<BrainReply> step(const BrainContext& ctx) = 0;
    // Optional: called after each tool result so a brain can keep state.
    virtual void observe(const ToolCall& call, const Json& result, bool ok) {
        (void)call; (void)result; (void)ok;
    }
    virtual Json settings() const { return Json::object(); }
};

// -----------------------------------------------------------------------------
// Built-in deterministic brain
// -----------------------------------------------------------------------------
struct HeuristicConfig {
    bool   verbose{false};
    bool   prefer_python{false};
    bool   create_readme{true};
    bool   create_tests{true};
    std::string language;        // "" = infer from the task, else cpp|python|javascript|rust|go
};

class HeuristicBrain : public Brain {
public:
    explicit HeuristicBrain(HeuristicConfig cfg = {}) : cfg_(std::move(cfg)) {}
    std::string name() const override { return "heuristic"; }
    std::string description() const override;
    Result<BrainReply> step(const BrainContext& ctx) override;
    Json settings() const override;

    // Exposed for tests: classifies a task description.
    enum class Intent {
        Unknown, Explain, CreateProject, CreateFile, Implement, Fix, Refactor,
        Rename, AddTests, Format, Document, Search, Scrape, Run, GitStatus,
    };
    static Intent classify(const std::string& task);
    static const char* intent_name(Intent intent);

private:
    HeuristicConfig cfg_;
    int             planned_{0};
};

// -----------------------------------------------------------------------------
// Local model brain (user-hosted, OpenAI-compatible HTTP endpoint)
// -----------------------------------------------------------------------------
struct LocalModelConfig {
    std::string endpoint{"http://127.0.0.1:8080"};   // llama-server / ollama / LM Studio
    std::string path{"/v1/chat/completions"};
    std::string model{"local-model"};
    std::string api_key;                              // usually empty
    double      temperature{0.2};
    int         max_tokens{1024};
    int         timeout_ms{120000};
    int         context_messages{24};                 // history window
    bool        use_json_mode{true};
    std::string system_prompt_extra;
};

class LocalModelBrain : public Brain {
public:
    explicit LocalModelBrain(LocalModelConfig cfg = {}) : cfg_(std::move(cfg)) {}
    std::string name() const override { return "local:" + cfg_.model; }
    std::string description() const override;
    Result<BrainReply> step(const BrainContext& ctx) override;
    Json settings() const override;

    // Probes the endpoint for liveness and model names.
    Result<std::vector<std::string>> list_models() const;
    const LocalModelConfig& config() const { return cfg_; }

    // Parsing of a model reply is pure and unit-tested separately.
    static Result<BrainReply> parse_model_reply(const std::string& content);
    static std::string build_system_prompt(const std::vector<ToolSpec>& tools,
                                           const std::string& extra);

private:
    LocalModelConfig cfg_;
    std::string      conversation_id_;
};

// Chooses a brain from configuration.
//   "auto"      -> local model if reachable, otherwise the heuristic brain
//   "heuristic" -> built-in planner
//   "local"     -> local model (fails when unreachable)
std::unique_ptr<Brain> make_brain(const std::string& kind, const LocalModelConfig& local,
                                  const HeuristicConfig& heuristic, std::string* note);

// -----------------------------------------------------------------------------
// Prompt / transcript rendering
// -----------------------------------------------------------------------------
std::string render_transcript(const std::vector<AgentMessage>& messages, size_t max_bytes = 24000);

}  // namespace lca

#endif  // LCA_MODEL_H
