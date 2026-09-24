// =============================================================================
//  lca/agent.h  --  the autonomous loop
// -----------------------------------------------------------------------------
//  The agent owns the tool set and drives the plan/act/observe/self-correct
//  cycle:
//
//      brain.step() -> tool calls -> execute -> results in the transcript
//                       ^                                   |
//                       +-----------------------------------+
//
//  Everything the agent does is observable: every step is recorded in the run
//  report, every command in the sandbox audit log, and every edit produces a
//  unified diff.  The loop is bounded by max_steps and by the memory budget, and
//  it stops early on repeated failures instead of thrashing.
// =============================================================================
#ifndef LCA_AGENT_H
#define LCA_AGENT_H

#include "lca/fs_engine.h"
#include "lca/mem.h"
#include "lca/model.h"
#include "lca/proc.h"
#include "lca/search.h"

namespace lca {

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
struct AgentConfig {
    WorkspaceConfig  workspace;
    SandboxPolicy    sandbox{SandboxPolicy::developer()};

    std::string      brain{"auto"};            // auto | heuristic | local
    LocalModelConfig local_model;
    HeuristicConfig  heuristic;

    int              max_steps{24};
    int              command_timeout_ms{120000};
    size_t           max_tool_output{96 * 1024};   // per tool result kept in the transcript
    bool             auto_verify{true};            // run a build/test after edits
    bool             dry_run{false};
    bool             verbose{false};
    bool             allow_network{true};
    bool             confirm_commands{false};      // ask before running shell commands
    std::string      verify_command;               // overrides auto-detection
    size_t           max_history_messages{200};
};

// -----------------------------------------------------------------------------
// Tool plumbing
// -----------------------------------------------------------------------------
struct ToolResult {
    bool        ok{false};
    std::string text;         // human/model readable summary
    Json        data;         // structured payload
    int64_t     elapsed_ms{0};
    std::string error;

    Json to_json() const;
};

struct AgentStep {
    int         index{0};
    ToolCall    call;
    ToolResult  result;
    int64_t     when_ms{0};
    bool        verification{false};   // true for the automatic post-edit check
};

// -----------------------------------------------------------------------------
// Run report
// -----------------------------------------------------------------------------
struct RunReport {
    std::string  task;
    std::string  brain;
    bool         ok{false};
    bool         finished{false};
    std::string  summary;
    std::string  final_answer;
    std::vector<AgentStep> steps;
    std::vector<std::string> changed_paths;
    std::vector<std::string> commands_run;
    std::string  verification_output;
    bool         verified{false};
    int64_t      elapsed_ms{0};
    uint64_t     peak_ram_bytes{0};
    uint64_t     peak_vram_bytes{0};
    size_t       tool_calls{0};
    size_t       failures{0};

    Json        to_json() const;
    std::string to_text(bool include_steps = true) const;
    std::string diff_summary() const;
};

// -----------------------------------------------------------------------------
// Agent
// -----------------------------------------------------------------------------
class Agent {
public:
    explicit Agent(AgentConfig cfg);
    ~Agent();

    // Opens the workspace, installs the memory budget and selects a brain.
    Error initialise();

    // Runs one task to completion (or to the step limit).
    Result<RunReport> run(const std::string& task);

    // Direct tool access, used by the CLI subcommands and the tests.
    Result<ToolResult> call_tool(const ToolCall& call);

    // Tool catalogue (names + specs) for the brains and for `--list-tools`.
    const std::vector<ToolSpec>& tools() const { return tools_; }

    const Workspace& workspace() const { return workspace_; }
    Workspace&       workspace()       { return workspace_; }
    ProcessSandbox&  sandbox()         { return sandbox_; }
    const std::vector<std::string>& changed_paths() const { return changed_paths_; }

    // Verification command chosen for this workspace (empty when none applies).
    std::string detect_verify_command() const;
    void        set_verify_command(const std::string& command) { cfg_.verify_command = command; }

private:
    Result<ToolResult> tool_project_map(const Json& args);
    Result<ToolResult> tool_fs_read(const Json& args);
    Result<ToolResult> tool_fs_write(const Json& args);
    Result<ToolResult> tool_fs_edit(const Json& args);
    Result<ToolResult> tool_fs_patch(const Json& args);
    Result<ToolResult> tool_fs_list(const Json& args);
    Result<ToolResult> tool_fs_glob(const Json& args);
    Result<ToolResult> tool_fs_grep(const Json& args);
    Result<ToolResult> tool_run_command(const Json& args);
    Result<ToolResult> tool_web_search(const Json& args);
    Result<ToolResult> tool_fetch_page(const Json& args);
    Result<ToolResult> tool_memory_report(const Json& args);

    Error run_verification(RunReport* report, int step_index, std::string* transcript_note);

    AgentConfig              cfg_;
    Workspace                workspace_;
    ProcessSandbox           sandbox_;
    std::unique_ptr<Brain>   brain_;
    std::vector<ToolSpec>    tools_;
    std::vector<std::string> changed_paths_;
    std::string              brain_note_;
    bool                     initialised_{false};
};

// -----------------------------------------------------------------------------
// Helpers shared with the CLI
// -----------------------------------------------------------------------------
// Chooses a verification command from the build files present in the workspace.
std::string infer_verify_command(const Workspace& ws);

// Renders a tool result for the transcript (bounded).
std::string render_tool_result(const ToolResult& result, size_t max_bytes);

}  // namespace lca

#endif  // LCA_AGENT_H
