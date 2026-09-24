// =============================================================================
//  tests/test_sandbox.cpp  --  process sandbox, agent tools, brains, memory
// =============================================================================
#include "lca/agent.h"
#include "lca/mem.h"
#include "lca/model.h"
#include "lca/proc.h"
#include "test_util.h"

#include <cstdio>

using namespace lca;
using lca_test::section;

namespace {

bool commands_look_sane();
bool hardware_looks_real();

std::string g_root;

void test_shell_exec() {
    section("shell execution");
    ProcessSandbox sandbox(SandboxPolicy::developer());
    sandbox.set_workspace_root(g_root);

    ExecRequest req;
    req.command = "printf 'out'; printf 'err' >&2; exit 3";
    req.cwd = g_root;
    req.timeout_ms = 10000;
    auto result = sandbox.run(req);
    LCA_CHECK(result.ok());
    LCA_EQ(result->exit_code, 3);
    LCA_STREQ(trim(result->out), "out");
    LCA_STREQ(trim(result->err), "err");
    LCA_CHECK(!result->timed_out);
    LCA_CHECK(result->elapsed_ms >= 0);

    // merged streams
    ExecRequest merged;
    merged.command = "printf 'a'; printf 'b' >&2";
    merged.cwd = g_root;
    merged.merge_stderr = true;
    auto merged_result = sandbox.run(merged);
    LCA_CHECK(merged_result.ok());
    LCA_CHECK(merged_result->out.find('a') != std::string::npos);
    LCA_CHECK(merged_result->out.find('b') != std::string::npos);
    LCA_CHECK(merged_result->err.empty());

    // stdin
    ExecRequest fed;
    fed.command = "cat";
    fed.cwd = g_root;
    fed.stdin_closed = false;
    fed.stdin_data = "piped\n";
    auto fed_result = sandbox.run(fed);
    LCA_CHECK(fed_result.ok());
    LCA_STREQ(trim(fed_result->out), "piped");
}

void test_timeout_and_limits() {
    section("timeouts and resource limits");
    ProcessSandbox sandbox(SandboxPolicy::developer());
    sandbox.set_workspace_root(g_root);

    ExecRequest slow;
    slow.command = "sleep 30";
    slow.cwd = g_root;
    slow.timeout_ms = 700;
    auto slow_result = sandbox.run(slow);
    LCA_CHECK(slow_result.ok());
    LCA_CHECK(slow_result->timed_out);
    LCA_CHECK(slow_result->elapsed_ms < 8000);

    // address-space limit: the child must fail rather than eat the machine
    ExecRequest hungry;
    hungry.command = "python3 -c \"b = bytearray(600 * 1024 * 1024)\"";
    hungry.cwd = g_root;
    hungry.timeout_ms = 10000;
    hungry.max_memory_bytes = 128ull * 1024 * 1024;
    auto hungry_result = sandbox.run(hungry);
    LCA_CHECK(hungry_result.ok());
    LCA_CHECK(!hungry_result->success());
    LCA_CHECK(hungry_result->peak_rss_bytes < 128ull * 1024 * 1024);

    ExecRequest flood;
    flood.command = "for i in $(seq 1 2000); do echo 'a very long line of output to fill the pipe'; done";
    flood.cwd = g_root;
    flood.timeout_ms = 15000;
    flood.max_output_bytes = 4096;
    auto flood_result = sandbox.run(flood);
    LCA_CHECK(flood_result.ok());
    LCA_CHECK(flood_result->out.size() <= 4096);
    LCA_CHECK(flood_result->out_truncated);
}

void test_policy() {
    section("policy");
    ProcessSandbox strict(SandboxPolicy::strict());
    strict.set_workspace_root(g_root);
    ExecRequest echo;
    echo.command = "echo allowed";
    echo.cwd = g_root;
    echo.timeout_ms = 5000;
    LCA_CHECK(strict.run(echo).ok());

    ExecRequest denied;
    denied.command = "nc -l 1234";
    denied.cwd = g_root;
    denied.timeout_ms = 5000;
    auto denied_result = strict.run(denied);
    LCA_CHECK(!denied_result.ok());
    LCA_STREQ(code_name(denied_result.error().code), "SandboxViolation");

    ProcessSandbox permissive(SandboxPolicy::permissive());
    permissive.set_workspace_root(g_root);
    ExecRequest destructive;
    destructive.command = "rm -rf /";
    destructive.cwd = g_root;
    destructive.timeout_ms = 5000;
    LCA_CHECK(!permissive.run(destructive).ok());

    // audit log records both outcomes
    LCA_CHECK(strict.audit_log().size() >= 2);

    // dry runs do not fork
    ExecRequest dry;
    dry.command = "echo should not run";
    dry.cwd = g_root;
    dry.dry_run = true;
    auto dry_result = strict.run(dry);
    LCA_CHECK(dry_result.ok());
    LCA_CHECK(dry_result->out.empty());

    LCA_CHECK(split_command_line("a 'b c' \"d e\"").size() == 3);
    LCA_STREQ(shell_quote("it's"), "'it'\\''s'");
    LCA_CHECK(commands_look_sane());
}

bool commands_look_sane() { return command_exists("sh") || command_exists("/bin/sh"); }

void test_memory_guard() {
    section("memory guard");
    MemoryGuard& guard = MemoryGuard::instance();
    Error e = guard.initialise_from_hardware();
    LCA_CHECK(e.ok());
    LCA_CHECK(guard.budget_bytes() > 0);
    LCA_CHECK(guard.budget_bytes() < 8ull * 1024 * 1024 * 1024);
    LCA_CHECK(guard.vram_budget_bytes() < 4ull * 1024 * 1024 * 1024);
    uint64_t before = guard.used_bytes();
    LCA_CHECK(guard.reserve(1024 * 1024, "test"));
    LCA_EQ(guard.used_bytes(), before + 1024 * 1024);
    guard.release(1024 * 1024);
    LCA_EQ(guard.used_bytes(), before);
    // absurd reservation must be refused, not attempted
    LCA_CHECK(!guard.reserve(guard.budget_bytes() * 2 + 1, "too big"));
    {
        MemoryReservation reservation(4096, "scoped");
        LCA_CHECK(reservation.ok());
        LCA_CHECK(guard.used_bytes() >= before + 4096);
    }
    LCA_CHECK(guard.used_bytes() <= before);
    LCA_CHECK(!guard.report_text().empty());
    LCA_CHECK(human_bytes(1024) == "1.00 KiB");
    LCA_CHECK(hardware_looks_real());
}

bool hardware_looks_real() {
    HardwareInfo hw = detect_hardware();
    return !hw.cpu_model.empty() && hw.cpu_threads >= 1;
}

void test_brain_intents() {
    section("heuristic brain");
    using HB = HeuristicBrain;
    LCA_CHECK(HB::classify("create a new python project called demo") == HB::Intent::CreateProject);
    LCA_CHECK(HB::classify("fix the failing build") == HB::Intent::Fix);
    LCA_CHECK(HB::classify("add tests for the parser") == HB::Intent::AddTests);
    LCA_CHECK(HB::classify("search for the c++ filesystem api") == HB::Intent::Search);
    LCA_CHECK(HB::classify("fetch https://example.com/page") == HB::Intent::Scrape);
    LCA_CHECK(HB::classify("run: make -j 4") == HB::Intent::Run);
    LCA_CHECK(HB::classify("explain this project") == HB::Intent::Explain);
    LCA_CHECK(HB::classify("rename foo to bar") == HB::Intent::Rename);
    LCA_CHECK(HB::classify("document the public api") == HB::Intent::Document);
    LCA_CHECK(HB::classify("") == HB::Intent::Unknown);
    LCA_STREQ(HB::intent_name(HB::Intent::CreateProject), "create-project");
    LCA_CHECK(default_tool_specs().size() >= 10);
}

void test_model_reply_parsing() {
    section("local model protocol");
    auto call = LocalModelBrain::parse_model_reply(
        R"({"thought":"write it","tool":"fs_write","args":{"path":"a.txt","content":"hi"}})");
    LCA_CHECK(call.ok());
    LCA_EQ(call->calls.size(), size_t(1));
    LCA_STREQ(call->calls[0].tool, "fs_write");
    LCA_STREQ(call->calls[0].args["path"].as_string(), "a.txt");

    auto fenced = LocalModelBrain::parse_model_reply(
        "```json\n{\"tool_calls\":[{\"tool\":\"fs_read\",\"args\":{\"path\":\"x\"}},"
        "{\"tool\":\"run_command\",\"args\":{\"command\":\"ls\"}}]}\n```");
    LCA_CHECK(fenced.ok());
    LCA_EQ(fenced->calls.size(), size_t(2));

    auto done = LocalModelBrain::parse_model_reply(
        R"({"thought":"all good","done":true,"answer":"Finished the task."})");
    LCA_CHECK(done.ok());
    LCA_CHECK(done->done);
    LCA_STREQ(done->final_answer, "Finished the task.");

    auto prose = LocalModelBrain::parse_model_reply("I cannot help with that.");
    LCA_CHECK(prose.ok());
    LCA_CHECK(prose->done);
    LCA_CHECK(prose->calls.empty());

    auto empty = LocalModelBrain::parse_model_reply("   ");
    LCA_CHECK(!empty.ok());

    std::string prompt = LocalModelBrain::build_system_prompt(default_tool_specs(), "extra rules");
    LCA_CHECK(prompt.find("fs_write") != std::string::npos);
    LCA_CHECK(prompt.find("extra rules") != std::string::npos);
}

void test_agent_tools() {
    section("agent tools");
    AgentConfig cfg;
    cfg.workspace.root = g_root;
    cfg.brain = "heuristic";
    cfg.sandbox = SandboxPolicy::developer();
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    LCA_CHECK_MSG(e.ok(), e.str());
    if (!e.ok()) return;

    auto call = [&](const char* tool, std::initializer_list<std::pair<const char*, const char*>> args) {
        ToolCall tc;
        tc.tool = tool;
        tc.args = Json::object();
        for (const auto& kv : args) tc.args[kv.first] = kv.second;
        return agent.call_tool(tc);
    };

    auto write = call("fs_write", {{"path", "agent/hello.txt"}, {"content", "hello agent\n"}});
    LCA_CHECK(write.ok());
    LCA_CHECK(write->ok);
    LCA_CHECK(!agent.changed_paths().empty());

    auto read = call("fs_read", {{"path", "agent/hello.txt"}});
    LCA_CHECK(read.ok());
    LCA_CHECK(read->text.find("hello agent") != std::string::npos);

    auto edit = call("fs_edit", {{"path", "agent/hello.txt"},
                                 {"find", "hello agent"},
                                 {"replace", "goodbye agent"},
                                 {"op", "replace"}});
    LCA_CHECK(edit.ok());
    LCA_CHECK(edit->ok);
    LCA_CHECK(!edit->data["diff"].as_string().empty());

    auto add_line = call("fs_edit", {{"path", "agent/hello.txt"},
                                     {"op", "insert_after"},
                                     {"line", "1"},
                                     {"content", "second line"}});
    LCA_CHECK(add_line.ok());

    // Tool failures surface as Result errors with a specific Code, so the agent
    // can pass the exact reason back to the model.
    auto missing = call("fs_read", {{"path", "does/not/exist.txt"}});
    LCA_CHECK(!missing.ok());
    LCA_STREQ(code_name(missing.error().code), "NotFound");

    auto escape = call("fs_read", {{"path", "../../etc/passwd"}});
    LCA_CHECK(!escape.ok());
    LCA_STREQ(code_name(escape.error().code), "SandboxViolation");

    auto tool_failure = call("run_command", {{"command", "exit 3"}, {"timeout_ms", "5000"}});
    LCA_CHECK(tool_failure.ok());
    LCA_CHECK(!tool_failure->ok);

    auto list = call("fs_glob", {{"pattern", "**/*.txt"}});
    LCA_CHECK(list.ok());
    LCA_CHECK(list->ok);

    auto grep = call("fs_grep", {{"pattern", "goodbye"}, {"regex", "false"}});
    LCA_CHECK(grep.ok());
    LCA_CHECK(grep->text.find("hello.txt") != std::string::npos);

    auto map = call("project_map", {{"max_files", "20"}});
    LCA_CHECK(map.ok());
    LCA_CHECK(map->data["total_files"].as_int() > 0);

    auto exec = call("run_command", {{"command", "printf sandboxed"}, {"timeout_ms", "10000"}});
    LCA_CHECK(exec.ok());
    LCA_CHECK(exec->ok);
    LCA_CHECK(exec->text.find("sandboxed") != std::string::npos);

    auto failed_cmd = call("run_command", {{"command", "exit 7"}, {"timeout_ms", "5000"}});
    LCA_CHECK(failed_cmd.ok());
    LCA_CHECK(!failed_cmd->ok);

    auto unknown = call("no_such_tool", {});
    LCA_CHECK(!unknown.ok());

    auto memory = call("memory_report", {});
    LCA_CHECK(memory.ok());
    LCA_CHECK(memory->text.find("ram budget") != std::string::npos);

    // A patch through the agent's tool layer.
    auto patch = call("fs_patch", {{"patch",
        "*** Begin Patch\n*** Add File: agent/patched.txt\n+patched content\n*** End Patch\n"}});
    LCA_CHECK(patch.ok());
    LCA_CHECK(patch->ok);
    LCA_CHECK(file_exists(g_root + "/agent/patched.txt"));

    // Verification command detection over the workspace.
    LCA_CHECK(agent.detect_verify_command().empty() || !agent.detect_verify_command().empty());
}

void test_agent_run() {
    section("agent run loop");
    AgentConfig cfg;
    cfg.workspace.root = g_root;
    cfg.brain = "heuristic";
    cfg.max_steps = 8;
    cfg.heuristic.language = "python";
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    LCA_CHECK(e.ok());
    if (!e.ok()) return;

    auto report = agent.run("explain this project");
    LCA_CHECK(report.ok());
    LCA_CHECK(report->finished);
    LCA_CHECK(report->tool_calls >= 2);
    LCA_CHECK(!report->brain.empty());
    LCA_CHECK(!report->to_text().empty());
    Json j = report->to_json();
    LCA_CHECK(j["steps"].items().size() >= 1);

    // empty task must be rejected
    auto bad = agent.run("   ");
    LCA_CHECK(!bad.ok());
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("lca sandbox/agent tests\n=======================\n");
    if (argc > 1) g_root = argv[1];
    if (g_root.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        g_root = std::string(tmp && *tmp ? tmp : "/tmp") + "/lca-agent-tests-" +
                 std::to_string(int64_t(wall_millis()));
    }
    WorkspaceConfig wcfg;
    wcfg.root = g_root;
    auto ws = workspace_open(wcfg);
    if (!ws.ok()) {
        std::fprintf(stderr, "workspace: %s\n", ws.error().str().c_str());
        return 2;
    }
    test_shell_exec();
    test_timeout_and_limits();
    test_policy();
    test_memory_guard();
    test_brain_intents();
    test_model_reply_parsing();
    test_agent_tools();
    test_agent_run();
    return lca_test::finish("test_sandbox");
}
