// =============================================================================
//  src/main.cpp  --  local-claude-code-agent command line interface
// -----------------------------------------------------------------------------
//  A single binary with subcommands.  Nothing here needs a network connection
//  except the `search` and `fetch` subcommands (and only when asked for).
// =============================================================================
#include "lca/agent.h"
#include "lca/buf.h"
#include "lca/fs_engine.h"
#include "lca/mem.h"
#include "lca/model.h"
#include "lca/proc.h"
#include "lca/search.h"
#include "lca/tls.h"
#include "lca/x509.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iostream>

namespace {

using namespace lca;

// -----------------------------------------------------------------------------
// Output helpers
// -----------------------------------------------------------------------------
bool g_json = false;
bool g_quiet = false;

void out(const std::string& text) {
    if (g_quiet) return;
    std::fputs(text.c_str(), stdout);
    if (!text.empty() && text.back() != '\n') std::fputc('\n', stdout);
    std::fflush(stdout);
}

void print_json(const Json& j) {
    std::fputs(j.dump(2).c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int fail(const Error& e) {
    std::fprintf(stderr, "%s: %s\n", kAppName, e.str().c_str());
    return 1;
}

// -----------------------------------------------------------------------------
// Size parsing: "512M", "2G", "1024"
// -----------------------------------------------------------------------------
uint64_t parse_size(const std::string& text, uint64_t fallback = 0) {
    std::string s = trim(text);
    if (s.empty()) return fallback;
    char* end = nullptr;
    double value = std::strtod(s.c_str(), &end);
    if (!end) return fallback;
    std::string suffix = lower(trim(std::string(end)));
    uint64_t mult = 1;
    if (suffix == "k" || suffix == "kb" || suffix == "kib") mult = 1024ull;
    else if (suffix == "m" || suffix == "mb" || suffix == "mib") mult = 1024ull * 1024;
    else if (suffix == "g" || suffix == "gb" || suffix == "gib") mult = 1024ull * 1024 * 1024;
    return uint64_t(value * double(mult));
}

// -----------------------------------------------------------------------------
// Option parsing
// -----------------------------------------------------------------------------
struct Options {
    std::string root{"."};
    std::string brain{"auto"};
    std::string model_endpoint{"http://127.0.0.1:8080"};
    std::string model_path{"/v1/chat/completions"};
    std::string model_name{"local-model"};
    std::string api_key;
    std::string verify_command;
    std::string policy{"developer"};
    std::string config_path;
    std::string engine{"auto"};
    std::string cwd;
    std::string language;
    uint64_t    max_ram{0};
    uint64_t    max_vram{0};
    int         max_steps{24};
    int         timeout_ms{120000};
    bool        verbose{false};
    bool        dry_run{false};
    bool        allow_network{true};
    bool        confirm{false};
    bool        only_text{false};
    bool        prefer_python{false};
    bool        follow_symlinks{false};
    bool        allow_outside_root{false};
    size_t      max_results{8};
    std::vector<std::string> positional;
};

// Very small config file reader: `key = value` lines, `#` comments.
void apply_config_file(const std::string& path, Options* opts) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (key == "root" || key == "workspace")         opts->root = value;
        else if (key == "brain")                          opts->brain = value;
        else if (key == "model_endpoint" || key == "endpoint") opts->model_endpoint = value;
        else if (key == "model_path")                     opts->model_path = value;
        else if (key == "model")                          opts->model_name = value;
        else if (key == "api_key")                        opts->api_key = value;
        else if (key == "max_steps")                      opts->max_steps = std::atoi(value.c_str());
        else if (key == "timeout_ms")                     opts->timeout_ms = std::atoi(value.c_str());
        else if (key == "max_ram")                        opts->max_ram = parse_size(value);
        else if (key == "max_vram")                       opts->max_vram = parse_size(value);
        else if (key == "policy")                         opts->policy = value;
        else if (key == "verify_command")                 opts->verify_command = value;
        else if (key == "network")                        opts->allow_network = value != "false" && value != "0";
        else if (key == "language")                       opts->language = value;
        else if (key == "search_engine")                  opts->engine = value;
    }
}

SandboxPolicy policy_from_name(const std::string& name) {
    std::string n = lower(name);
    if (n == "strict" || n == "locked") return SandboxPolicy::strict();
    if (n == "permissive" || n == "off" || n == "none") return SandboxPolicy::permissive();
    return SandboxPolicy::developer();
}

AgentConfig make_agent_config(const Options& o, bool need_workspace) {
    AgentConfig cfg;
    if (need_workspace) cfg.workspace.root = o.root;
    cfg.workspace.follow_symlinks = o.follow_symlinks;
    cfg.workspace.allow_outside_root = o.allow_outside_root;
    cfg.sandbox = policy_from_name(o.policy);
    cfg.brain = o.brain;
    cfg.local_model.endpoint = o.model_endpoint;
    cfg.local_model.path = o.model_path;
    cfg.local_model.model = o.model_name;
    cfg.local_model.api_key = o.api_key;
    cfg.local_model.timeout_ms = std::max(o.timeout_ms, 120000);
    cfg.heuristic.language = o.language;
    cfg.heuristic.prefer_python = o.prefer_python;
    cfg.heuristic.verbose = o.verbose;
    cfg.max_steps = o.max_steps;
    cfg.command_timeout_ms = o.timeout_ms;
    cfg.dry_run = o.dry_run;
    cfg.verbose = o.verbose;
    cfg.allow_network = o.allow_network;
    cfg.confirm_commands = o.confirm;
    cfg.verify_command = o.verify_command;
    return cfg;
}

Error prepare_memory(const Options& o) {
    MemoryLimits limits;
    limits.install_rlimits = false;   // the CLI installs them after parsing
    if (o.max_ram)  limits.ram_cap_bytes  = o.max_ram;
    if (o.max_vram) limits.vram_cap_bytes = o.max_vram;
    MemoryGuard& guard = MemoryGuard::instance();
    Error e = guard.initialise(limits);
    if (!e.ok()) return e;
    if (limits.ram_cap_bytes >= 8ull * 1024 * 1024 * 1024 ||
        limits.vram_cap_bytes >= 4ull * 1024 * 1024 * 1024) {
        return LCA_FAIL(Code::InvalidArgument, "memory caps must stay below 8 GiB RAM / 4 GiB VRAM");
    }
    return guard.enforce_rlimits(guard.budget_bytes() + guard.budget_bytes() / 8);
}

// -----------------------------------------------------------------------------
// Help
// -----------------------------------------------------------------------------
void print_help() {
    std::printf(
        "%s %s (%s) -- autonomous local coding agent\n\n"
        "USAGE\n"
        "  lca <command> [options] [arguments]\n\n"
        "COMMANDS\n"
        "  run \"<task>\"        Run the agent on a task in the workspace\n"
        "  chat                Interactive session (one task per line)\n"
        "  search \"<query>\"    Web search through a search engine (no API key)\n"
        "  fetch <url>         Fetch a page and print its readable text\n"
        "  exec \"<command>\"    Run a shell command inside the sandbox\n"
        "  read <path>         Print a file with line numbers\n"
        "  write <path>        Write stdin (or --content) to a file\n"
        "  edit <path>         Exact-snippet edit: --find ... --replace ...\n"
        "  patch <file>        Apply a unified diff / block patch\n"
        "  ls [path]           List the workspace (recursive with -r)\n"
        "  glob <pattern>      Find files by glob (**/*.cpp)\n"
        "  grep <pattern>      Search file contents (--regex, --include)\n"
        "  map                 Project map: tree, languages, entry points\n"
        "  doctor              Hardware, memory envelope and toolchain report\n"
        "  tools               List the agent's tools\n"
        "  version             Print the version\n\n"
        "COMMON OPTIONS\n"
        "  --root DIR          Workspace root (default: .)\n"
        "  --brain KIND        auto | heuristic | local        (default: auto)\n"
        "  --model-endpoint U  Local model server URL (default: http://127.0.0.1:8080)\n"
        "  --model NAME        Model name passed to the local server\n"
        "  --max-steps N       Agent step budget (default: 24)\n"
        "  --timeout MS        Per-command timeout (default: 120000)\n"
        "  --max-ram SIZE      Hard RAM cap, e.g. 4G (must stay below 8G)\n"
        "  --max-vram SIZE     Hard VRAM cap, e.g. 3G (must stay below 4G)\n"
        "  --policy NAME       developer | strict | permissive\n"
        "  --verify \"CMD\"      Verification command used after edits\n"
        "  --no-network        Disable web_search / fetch_page and network tools\n"
        "  --dry-run           Plan and patch without touching the filesystem\n"
        "  --confirm           Ask before every shell command\n"
        "  --json              Machine readable output\n"
        "  -v, --verbose       Verbose logging\n"
        "  -q, --quiet         Suppress normal output\n"
        "  --config FILE       Read defaults from a config file\n"
        "  -h, --help          This help\n",
        kAppName, kAppVersion, kAppCodename);
}

// -----------------------------------------------------------------------------
// Argument parsing
// -----------------------------------------------------------------------------
bool parse_options(int argc, char** argv, Options* o, std::string* command, Error* err) {
    // First pass: config file (so CLI flags win).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) apply_config_file(argv[i + 1], o);
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                *err = Error(Code::InvalidArgument, std::string("option ") + name + " needs a value");
                return {};
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { *command = "help"; return true; }
        if (a == "-v" || a == "--verbose") { o->verbose = true; continue; }
        if (a == "-q" || a == "--quiet") { g_quiet = true; continue; }
        if (a == "--json") { g_json = true; continue; }
        if (a == "--dry-run") { o->dry_run = true; continue; }
        if (a == "--confirm") { o->confirm = true; continue; }
        if (a == "--no-network") { o->allow_network = false; continue; }
        if (a == "--only-text") { o->only_text = true; continue; }
        if (a == "--prefer-python") { o->prefer_python = true; continue; }
        if (a == "-r" || a == "--recursive") { o->positional.push_back("--recursive"); continue; }
        if (a == "--regex") { o->positional.push_back("--regex"); continue; }
        if (a == "--follow-symlinks") { o->follow_symlinks = true; continue; }
        if (a == "--allow-outside-root") { o->allow_outside_root = true; continue; }
        if (a == "--root" || a == "--workspace") { o->root = need_value("--root"); continue; }
        if (a == "--brain")          { o->brain = need_value("--brain"); continue; }
        if (a == "--model-endpoint") { o->model_endpoint = need_value("--model-endpoint"); continue; }
        if (a == "--model-path")     { o->model_path = need_value("--model-path"); continue; }
        if (a == "--model")          { o->model_name = need_value("--model"); continue; }
        if (a == "--api-key")        { o->api_key = need_value("--api-key"); continue; }
        if (a == "--max-steps")      { o->max_steps = std::atoi(need_value("--max-steps").c_str()); continue; }
        if (a == "--timeout")        { o->timeout_ms = std::atoi(need_value("--timeout").c_str()); continue; }
        if (a == "--max-ram")        { o->max_ram = parse_size(need_value("--max-ram")); continue; }
        if (a == "--max-vram")       { o->max_vram = parse_size(need_value("--max-vram")); continue; }
        if (a == "--policy")         { o->policy = need_value("--policy"); continue; }
        if (a == "--verify")         { o->verify_command = need_value("--verify"); continue; }
        if (a == "--engine")         { o->engine = need_value("--engine"); continue; }
        if (a == "--cwd")            { o->cwd = need_value("--cwd"); continue; }
        if (a == "--language")       { o->language = need_value("--language"); continue; }
        if (a == "--max-results")    { o->max_results = size_t(std::atoi(need_value("--max-results").c_str())); continue; }
        if (a == "--content")        { o->positional.push_back("--content"); o->positional.push_back(need_value("--content")); continue; }
        if (a == "--find")           { o->positional.push_back("--find"); o->positional.push_back(need_value("--find")); continue; }
        if (a == "--replace")        { o->positional.push_back("--replace"); o->positional.push_back(need_value("--replace")); continue; }
        if (a == "--include")        { o->positional.push_back("--include"); o->positional.push_back(need_value("--include")); continue; }
        if (a == "--config")         { o->config_path = need_value("--config"); continue; }
        if (a == "--") { for (int j = i + 1; j < argc; ++j) o->positional.push_back(argv[j]); break; }
        if (!a.empty() && a[0] == '-' && a.size() > 1) {
            *err = Error(Code::InvalidArgument, "unknown option " + a);
            return false;
        }
        if (command->empty()) *command = a;
        else o->positional.push_back(a);
    }
    return true;
}

// -----------------------------------------------------------------------------
// Subcommands
// -----------------------------------------------------------------------------
int cmd_run(const Options& o, const std::string& task) {
    if (trim(task).empty()) {
        std::fprintf(stderr, "usage: %s run \"<task>\"\n", kAppName);
        return 2;
    }
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    Result<RunReport> report = agent.run(task);
    if (!report.ok()) return fail(report.error());
    if (g_json) print_json(report->to_json());
    else        out(report->to_text());
    return report->ok ? 0 : 3;
}

int cmd_chat(const Options& o, const std::vector<std::string>& seeds) {
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);

    auto run_one = [&](const std::string& task) -> int {
        if (trim(task).empty()) return 0;
        if (trim(task) == "exit" || trim(task) == "quit") return 1;
        Result<RunReport> report = agent.run(task);
        if (!report.ok()) {
            std::fprintf(stderr, "%s\n", report.error().str().c_str());
            return 0;
        }
        if (g_json) print_json(report->to_json());
        else        out(report->to_text(false));
        return 0;
    };

    if (!seeds.empty()) {
        int stop = run_one(join(seeds, " "));
        return stop == 1 ? 0 : 0;
    }
    out(std::string(kAppName) + " " + kAppVersion + " interactive session. " +
        "Type a task, 'help' for ideas, 'exit' to quit.");
    std::string line;
    while (true) {
        std::fputs("lca> ", stdout);
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;
        if (line == "help") {
            out("try: create a c++ project called hello | run: make -j 2 | "
                "search for c++ filesystem api | explain this project | "
                "add tests | map");
            continue;
        }
        int stop = run_one(line);
        if (stop == 1) break;
    }
    out("bye");
    return 0;
}

int cmd_search(const Options& o, const std::string& query) {
    SearchRequest req;
    req.query = query;
    req.engine = o.engine;
    req.max_results = int(o.max_results);
    req.fetch_snippets = false;
    Result<SearchResponse> response = web_search(req);
    if (!response.ok()) return fail(response.error());
    if (g_json) print_json(response->to_json());
    else        out(response->to_text());
    return response->results.empty() ? 4 : 0;
}

int cmd_fetch(const Options& o, const std::string& url) {
    FetchOptions opts;
    opts.only_text = o.only_text;
    opts.timeout_ms = std::max(o.timeout_ms, 20000);
    Result<PageContent> page = fetch_page(url, opts);
    if (!page.ok()) return fail(page.error());
    if (g_json) print_json(page->to_json());
    else        out(page->to_text());
    return page->status >= 200 && page->status < 400 ? 0 : 4;
}

int cmd_exec(const Options& o, const std::string& command) {
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "run_command";
    call.args = Json::object();
    call.args["command"] = command;
    call.args["cwd"] = o.cwd;
    call.args["timeout_ms"] = int64_t(o.timeout_ms);
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        out(result->text);
    return result->ok ? 0 : int(result->data["exit_code"].as_int(1));
}

int cmd_read(const Options& o, const std::string& path) {
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_read";
    call.args = Json::object();
    call.args["path"] = path;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        std::fputs(result->text.c_str(), stdout);
    return 0;
}

int cmd_write(const Options& o, const std::string& path) {
    std::string content;
    bool have_content = false;
    for (size_t i = 0; i < o.positional.size(); ++i) {
        if (o.positional[i] == "--content" && i + 1 < o.positional.size()) {
            content = o.positional[i + 1];
            have_content = true;
        }
    }
    if (!have_content) {
        // `write <path> <text>`: the second positional argument is the payload.
        // Only when neither --content nor a second argument is present do we read
        // the document from standard input so `write <path> < file` works.
        if (o.positional.size() > 1) {
            std::vector<std::string> tail(o.positional.begin() + 1, o.positional.end());
            content = join(tail, " ");
            have_content = true;
        }
    }
    if (!have_content) {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        content = buffer.str();
    }
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_write";
    call.args = Json::object();
    call.args["path"] = path;
    call.args["content"] = content;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        out(result->text);
    return result->ok ? 0 : 1;
}

int cmd_edit(const Options& o, const std::string& path) {
    std::string find, replace;
    bool dry = false;
    for (size_t i = 0; i < o.positional.size(); ++i) {
        if (o.positional[i] == "--find" && i + 1 < o.positional.size()) find = o.positional[i + 1];
        if (o.positional[i] == "--replace" && i + 1 < o.positional.size()) replace = o.positional[i + 1];
        if (o.positional[i] == "--dry-run") dry = true;
    }
    // `edit <path> <find> <replace>` shorthand.
    if (find.empty() && o.positional.size() > 1 && o.positional[1] != "--find")
        find = o.positional[1];
    if (replace.empty() && o.positional.size() > 2 && o.positional[1] != "--replace")
        replace = o.positional[2];
    if (find.empty()) {
        std::fprintf(stderr, "usage: %s edit <path> --find <text> --replace <text>\n", kAppName);
        return 2;
    }
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_edit";
    call.args = Json::object();
    call.args["path"] = path;
    call.args["find"] = find;
    call.args["replace"] = replace;
    call.args["op"] = "replace";
    call.args["dry_run"] = dry;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        out(result->text);
    return result->ok ? 0 : 1;
}

int cmd_patch(const Options& o, const std::string& file) {
    std::string patch;
    if (file == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        patch = buffer.str();
    } else {
        // The patch file is resolved against the workspace root first so that
        // `--root DIR patch fix.patch` behaves like every other subcommand,
        // then against the current working directory as a convenience.
        std::string resolved = file;
        std::ifstream f(resolved, std::ios::binary);
        if (!f && !o.root.empty()) {
            resolved = o.root + "/" + file;
            f.clear();
            f.open(resolved, std::ios::binary);
        }
        if (!f) return fail(LCA_FAIL(Code::NotFound, "cannot read patch file " + file));
        std::ostringstream buffer;
        buffer << f.rdbuf();
        patch = buffer.str();
    }
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_patch";
    call.args = Json::object();
    call.args["patch"] = patch;
    call.args["dry_run"] = o.dry_run;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        out(result->text);
    return result->ok ? 0 : 1;
}

int cmd_ls(const Options& o, const std::string& path) {
    bool recursive = false;
    for (const std::string& p : o.positional) if (p == "--recursive") recursive = true;
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_list";
    call.args = Json::object();
    call.args["path"] = path.empty() ? "." : path;
    call.args["recursive"] = recursive;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        std::fputs(result->text.c_str(), stdout);
    return 0;
}

int cmd_glob(const Options& o, const std::string& pattern) {
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_glob";
    call.args = Json::object();
    call.args["pattern"] = pattern;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        std::fputs(result->text.c_str(), stdout);
    return 0;
}

int cmd_grep(const Options& o, const std::string& pattern) {
    bool regex = false;
    std::string include;
    for (size_t i = 0; i < o.positional.size(); ++i) {
        if (o.positional[i] == "--regex") regex = true;
        if (o.positional[i] == "--include" && i + 1 < o.positional.size()) include = o.positional[i + 1];
    }
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "fs_grep";
    call.args = Json::object();
    call.args["pattern"] = pattern;
    call.args["regex"] = regex;
    if (!include.empty()) call.args["include"] = include;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        std::fputs(result->text.c_str(), stdout);
    return 0;
}

int cmd_map(const Options& o) {
    AgentConfig cfg = make_agent_config(o, true);
    Agent agent(std::move(cfg));
    Error e = agent.initialise();
    if (!e.ok()) return fail(e);
    ToolCall call;
    call.tool = "project_map";
    call.args = Json::object();
    call.args["max_files"] = 200;
    Result<ToolResult> result = agent.call_tool(call);
    if (!result.ok()) return fail(result.error());
    if (g_json) print_json(result->data);
    else        std::fputs(result->text.c_str(), stdout);
    return 0;
}

int cmd_doctor(const Options& o) {
    HardwareInfo hw = detect_hardware();
    MemoryGuard& guard = MemoryGuard::instance();
    Error mem = prepare_memory(o);
    if (!mem.ok()) std::fprintf(stderr, "memory guard: %s\n", mem.str().c_str());

    Json report = Json::object();
    report["version"] = kAppVersion;
    report["hardware"] = hw.to_json();
    report["memory"] = guard.report_json();
    Json checks = Json::array();

    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        Json c = Json::object();
        c["check"] = name;
        c["ok"] = ok;
        c["detail"] = detail;
        checks.push_back(c);
    };
    check("ram budget below 8 GiB",
          guard.budget_bytes() < 8ull * 1024 * 1024 * 1024,
          human_bytes(guard.budget_bytes()));
    check("vram budget below 4 GiB",
          guard.vram_budget_bytes() < 4ull * 1024 * 1024 * 1024,
          human_bytes(guard.vram_budget_bytes()));
    check("C++ compiler", command_exists("c++") || command_exists("g++") || command_exists("clang++"),
          capture_command("c++ --version 2>/dev/null | head -1"));
    check("make", command_exists("make"), capture_command("make --version 2>/dev/null | head -1"));
    check("cmake", command_exists("cmake"), capture_command("cmake --version 2>/dev/null | head -1"));
    check("python3", command_exists("python3"), capture_command("python3 --version 2>&1 | head -1"));
    check("git", command_exists("git"), capture_command("git --version 2>&1 | head -1"));
    {
        std::string ca_path;
        for (const std::string& candidate : TrustStore::default_bundle_paths()) {
            std::ifstream probe(candidate);
            if (probe.good()) { ca_path = candidate; break; }
        }
        check("ca bundle", !ca_path.empty(), ca_path.empty() ? "no system trust store found" : ca_path);
    }
    check("local model endpoint", !o.model_endpoint.empty(), o.model_endpoint);
    report["checks"] = checks;

    if (g_json) {
        print_json(report);
        return 0;
    }
    out(std::string(kAppName) + " " + kAppVersion + " (" + kAppCodename + ")");
    out("");
    // report_text() already starts with the hardware block, so it is printed once.
    out("memory envelope\n---------------");
    out(guard.report_text());
    out("toolchain\n---------");
    for (const Json& c : checks.items()) {
        std::string line = std::string(c["ok"].as_bool() ? "ok   " : "warn ") +
                           c["check"].as_string();
        std::string detail = c["detail"].as_string();
        if (!detail.empty()) line += "  --  " + ellipsize(detail, 80);
        out(line);
    }
    return 0;
}

int cmd_tools() {
    std::vector<ToolSpec> tools = default_tool_specs();
    if (g_json) {
        Json arr = Json::array();
        for (const ToolSpec& t : tools) {
            Json j = Json::object();
            j["name"] = t.name;
            j["summary"] = t.summary;
            j["args"] = t.args_help;
            arr.push_back(j);
        }
        print_json(arr);
        return 0;
    }
    out("available tools");
    out("---------------");
    for (const ToolSpec& t : tools) out("  " + t.name + "  --  " + t.summary);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

    Options options;
    std::string command;
    Error parse_error;
    if (!parse_options(argc, argv, &options, &command, &parse_error)) return fail(parse_error);
    if (command.empty()) {
        print_help();
        return 2;
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_help();
        return 0;
    }
    if (command == "version") {
        if (g_json) {
            Json j = Json::object();
            j["name"] = kAppName;
            j["version"] = kAppVersion;
            j["codename"] = kAppCodename;
            print_json(j);
        } else {
            std::printf("%s %s (%s)\n", kAppName, kAppVersion, kAppCodename);
        }
        return 0;
    }

    log_set_level(options.verbose ? LogLevel::Debug : LogLevel::Info);

    if (command == "doctor") return cmd_doctor(options);
    if (command == "tools")  return cmd_tools();

    // Everything else needs a workspace (and therefore the memory envelope).
    Error mem = prepare_memory(options);
    if (!mem.ok()) return fail(mem);

    std::string first = options.positional.empty() ? std::string() : options.positional.front();
    std::string rest = options.positional.size() > 1
                           ? join(std::vector<std::string>(options.positional.begin() + 1,
                                                           options.positional.end()), " ")
                           : std::string();

    if (command == "run")    return cmd_run(options, first);
    if (command == "chat")   return cmd_chat(options, options.positional);
    if (command == "search") return cmd_search(options, first);
    if (command == "fetch")  return cmd_fetch(options, first);
    if (command == "exec")   return cmd_exec(options, first);
    if (command == "read")   return cmd_read(options, first);
    if (command == "write")  return cmd_write(options, first);
    if (command == "edit")   return cmd_edit(options, first);
    if (command == "patch")  return cmd_patch(options, first.empty() ? "-" : first);
    if (command == "ls")     return cmd_ls(options, first);
    if (command == "glob")   return cmd_glob(options, first);
    if (command == "grep")   return cmd_grep(options, first);
    if (command == "map")    return cmd_map(options);

    std::fprintf(stderr, "%s: unknown command '%s'\n", kAppName, command.c_str());
    print_help();
    (void)rest;
    return 2;
}
