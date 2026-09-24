// =============================================================================
//  lca/proc.h  --  embedded execution sandbox
// -----------------------------------------------------------------------------
//  The agent compiles, tests and runs code the same way an engineer would: by
//  spawning real processes.  Those processes are constrained by the sandbox:
//
//    * rlimits (address space, CPU seconds, file size, process count, core dump)
//    * per-command allow/deny policy evaluated before fork()
//    * wall-clock deadline with SIGTERM -> SIGKILL escalation of the process
//      group (so a runaway child cannot outlive the agent)
//    * output capture with a hard byte cap (never buffers unbounded output)
//    * optional working-directory confinement to the workspace root
//
//  Everything is plain POSIX: fork / execve / pipe / poll / waitpid / setrlimit.
// =============================================================================
#ifndef LCA_PROC_H
#define LCA_PROC_H

#include "lca/common.h"
#include "lca/buf.h"
#include "lca/fs_engine.h"

#include <map>

namespace lca {

// -----------------------------------------------------------------------------
// Requests and results
// -----------------------------------------------------------------------------
struct ExecRequest {
    // Either `argv` (executed directly) or `command` (run through /bin/sh -c).
    std::vector<std::string> argv;
    std::string              command;
    std::string              cwd;                 // empty = the workspace root
    std::map<std::string, std::string> env;       // merged over the inherited env
    std::vector<std::string> env_remove;

    int      timeout_ms{60000};
    size_t   max_output_bytes{1u << 20};          // per stream, before truncation
    uint64_t max_memory_bytes{2ull * 1024 * 1024 * 1024};   // RLIMIT_AS for the child
    uint64_t max_cpu_seconds{0};                  // 0 = derived from timeout_ms
    uint64_t max_file_bytes{512ull * 1024 * 1024};// RLIMIT_FSIZE
    size_t   max_processes{256};                  // RLIMIT_NPROC
    bool     stdin_closed{true};
    std::string stdin_data;                       // fed to the child when non-empty
    bool     merge_stderr{false};                 // capture both streams together
    bool     dry_run{false};
};

struct ExecResult {
    int         exit_code{-1};
    int         signal{0};
    bool        timed_out{false};
    bool        spawn_failed{false};
    bool        out_truncated{false};
    bool        err_truncated{false};
    std::string out;
    std::string err;
    std::string command_line;
    int64_t     elapsed_ms{0};
    uint64_t    peak_rss_bytes{0};
    int64_t     user_us{0};
    int64_t     sys_us{0};
    int64_t     wall_deadline_ms{0};

    bool success() const { return exit_code == 0 && !timed_out && !spawn_failed; }
    std::string summary() const;
    Json        to_json() const;
};

// -----------------------------------------------------------------------------
// Policy
// -----------------------------------------------------------------------------
enum class CommandVerdict { Allow, Deny, NeedsApproval };

struct PolicyRule {
    std::string pattern;          // substring or glob (when it contains wildcards)
    CommandVerdict verdict{CommandVerdict::Allow};
    std::string note;
};

struct SandboxPolicy {
    // When true, only commands matching an Allow rule may run at all.
    bool        default_deny{false};
    bool        confine_to_workspace{true};
    bool        allow_network_commands{false};
    std::vector<PolicyRule> rules;
    std::vector<std::string> denylist;   // dangerous fragments: "rm -rf /", "mkfs", ...
    std::vector<std::string> allowlist;  // when non-empty, commands must contain one of these

    static SandboxPolicy permissive();   // everything allowed except the denylist
    static SandboxPolicy developer();    // compilers, tests, git, package managers
    static SandboxPolicy strict();       // only the allowlisted toolchains

    CommandVerdict evaluate(const std::string& command, std::string* reason = nullptr) const;
};

// -----------------------------------------------------------------------------
// ProcessSandbox
// -----------------------------------------------------------------------------
class ProcessSandbox {
public:
    ProcessSandbox() = default;
    explicit ProcessSandbox(SandboxPolicy policy) : policy_(std::move(policy)) {}

    // Executes the request, honouring the policy.  Never throws.
    Result<ExecResult> run(const ExecRequest& req);

    // Runs without consulting the policy (used by the CLI for explicit commands
    // after the operator confirmed them).
    Result<ExecResult> run_unchecked(const ExecRequest& req);

    const SandboxPolicy& policy() const { return policy_; }
    void set_policy(SandboxPolicy policy) { policy_ = std::move(policy); }

    // Audit log of every command the sandbox saw (allowed or denied).
    struct AuditEntry {
        int64_t     when_ms{0};
        std::string command;
        std::string verdict;
        std::string reason;
        int         exit_code{-1};
    };
    const std::vector<AuditEntry>& audit_log() const { return audit_; }
    void clear_audit_log() { audit_.clear(); }

    void set_workspace_root(const std::string& root) { workspace_root_ = root; }
    const std::string& workspace_root() const { return workspace_root_; }

private:
    SandboxPolicy            policy_{SandboxPolicy::developer()};
    std::string              workspace_root_;
    std::vector<AuditEntry>  audit_;
};

// -----------------------------------------------------------------------------
// Convenience wrappers
// -----------------------------------------------------------------------------
// Runs a shell command with the developer policy (used by the agent's tools).
Result<ExecResult> quick_exec(const std::string& command, const std::string& cwd = {},
                              int timeout_ms = 60000, size_t max_output = 1u << 20);

// True when the executable exists on PATH and is runnable.
bool command_exists(const std::string& program);

// Captures a short single-line command output (e.g. `cc --version`).
std::string capture_command(const std::string& command, int timeout_ms = 10000);

// Splits a command line into argv using POSIX shell quoting rules (no expansion).
std::vector<std::string> split_command_line(std::string_view line);

// Quotes a string so it survives a /bin/sh -c round trip.
std::string shell_quote(std::string_view s);

}  // namespace lca

#endif  // LCA_PROC_H
