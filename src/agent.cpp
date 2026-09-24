// =============================================================================
//  lca/agent.cpp  --  tool implementations + the autonomous loop
// =============================================================================
#include "lca/agent.h"
#include "lca/buf.h"

#include <algorithm>
#include <set>

namespace lca {
namespace {

constexpr const char* kScope = "agent";

std::string arg_string(const Json& args, const char* key, const std::string& fallback = {}) {
    if (!args.has(key)) return fallback;
    const Json& v = args[key];
    if (v.is_string()) return v.as_string();
    if (v.is_number()) return std::to_string(v.as_int());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    return fallback;
}

int64_t arg_int(const Json& args, const char* key, int64_t fallback) {
    if (!args.has(key)) return fallback;
    const Json& v = args[key];
    if (v.is_number()) return v.as_int();
    if (v.is_string()) return std::strtoll(v.as_string().c_str(), nullptr, 10);
    return fallback;
}

bool arg_bool(const Json& args, const char* key, bool fallback) {
    if (!args.has(key)) return fallback;
    const Json& v = args[key];
    if (v.is_bool()) return v.as_bool();
    if (v.is_string()) return v.as_string() == "true" || v.as_string() == "1" ||
                               v.as_string() == "yes";
    if (v.is_number()) return v.as_int() != 0;
    return fallback;
}

// Numbered-file preview used by fs_read and fetch_page results.
std::string numbered_lines(const std::string& text, size_t start_line = 1, size_t end_line = 0) {
    std::vector<std::string> lines = split_lines(text);
    size_t total = lines.size();
    size_t from = start_line > 0 ? start_line - 1 : 0;
    size_t to = end_line > 0 && end_line <= total ? end_line : total;
    if (from > total) from = total;
    std::string out;
    for (size_t i = from; i < to; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%5zu| ", i + 1);
        out += buf;
        out += lines[i];
        out += "\n";
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// ToolResult / RunReport formatting
// -----------------------------------------------------------------------------
Json ToolResult::to_json() const {
    Json j = Json::object();
    j["ok"] = ok;
    j["text"] = text;
    j["data"] = data;
    j["elapsed_ms"] = int64_t(elapsed_ms);
    if (!error.empty()) j["error"] = error;
    return j;
}

Json RunReport::to_json() const {
    Json j = Json::object();
    j["task"] = task;
    j["brain"] = brain;
    j["ok"] = ok;
    j["finished"] = finished;
    j["summary"] = summary;
    j["final_answer"] = final_answer;
    j["elapsed_ms"] = int64_t(elapsed_ms);
    j["tool_calls"] = int64_t(tool_calls);
    j["failures"] = int64_t(failures);
    j["verified"] = verified;
    j["verification_output"] = verification_output;
    j["peak_ram_bytes"] = int64_t(peak_ram_bytes);
    j["peak_vram_bytes"] = int64_t(peak_vram_bytes);
    Json changed = Json::array();
    for (const std::string& p : changed_paths) changed.push_back(Json(p));
    j["changed_paths"] = changed;
    Json cmds = Json::array();
    for (const std::string& c : commands_run) cmds.push_back(Json(c));
    j["commands_run"] = cmds;
    Json steps = Json::array();
    for (const AgentStep& s : this->steps) {
        Json sj = Json::object();
        sj["index"] = int64_t(s.index);
        sj["tool"] = s.call.tool;
        sj["args"] = s.call.args;
        sj["rationale"] = s.call.rationale;
        sj["ok"] = s.result.ok;
        sj["output"] = s.result.text;
        sj["elapsed_ms"] = int64_t(s.result.elapsed_ms);
        sj["verification"] = s.verification;
        steps.push_back(sj);
    }
    j["steps"] = steps;
    return j;
}

std::string RunReport::to_text(bool include_steps) const {
    std::string out;
    out += "task      : " + task + "\n";
    out += "brain     : " + brain + "\n";
    out += "result    : " + std::string(ok ? "ok" : "incomplete") +
           (finished ? " (finished)" : " (step limit reached)") + "\n";
    out += "tools     : " + std::to_string(tool_calls) + " called, " +
           std::to_string(failures) + " failed\n";
    out += "elapsed   : " + human_duration(elapsed_ms) + "\n";
    out += "memory    : ram " + human_bytes(peak_ram_bytes) + " / vram " +
           human_bytes(peak_vram_bytes) + " peak\n";
    if (!changed_paths.empty()) out += "changed   : " + join(changed_paths, ", ") + "\n";
    if (verified) out += "verified  : yes\n";
    if (include_steps && !steps.empty()) {
        out += "\nsteps\n-----\n";
        for (const AgentStep& s : steps) {
            out += "[" + std::to_string(s.index) + "] " + s.call.tool +
                   (s.verification ? " (verification)" : "") + " -> " +
                   (s.result.ok ? "ok" : "failed") + " in " +
                   human_duration(s.result.elapsed_ms) + "\n";
            if (!s.call.rationale.empty()) out += "      why: " + s.call.rationale + "\n";
            std::string preview = ellipsize(trim(s.result.text), 400);
            if (!preview.empty()) out += "      " + replace_all(preview, "\n", "\n      ") + "\n";
        }
    }
    if (!verification_output.empty())
        out += "\nverification\n------------\n" + ellipsize(verification_output, 2000) + "\n";
    if (!summary.empty()) out += "\nsummary\n-------\n" + summary + "\n";
    if (!final_answer.empty()) out += "\nanswer\n------\n" + final_answer + "\n";
    return out;
}

std::string RunReport::diff_summary() const {
    std::string out;
    for (const AgentStep& s : steps) {
        if (s.call.tool != "fs_write" && s.call.tool != "fs_edit" && s.call.tool != "fs_patch")
            continue;
        if (!s.result.data.has("diff")) continue;
        std::string diff = s.result.data["diff"].as_string();
        if (!diff.empty()) out += diff;
    }
    return out;
}

// -----------------------------------------------------------------------------
// Verification command detection
// -----------------------------------------------------------------------------
std::string infer_verify_command(const Workspace& ws) {
    // Prefer the project's own build system, in order of specificity.
    struct Candidate {
        const char* probe;
        const char* command;
    };
    static const Candidate kCandidates[] = {
        {"CMakeLists.txt", "cmake -S . -B build-verify -DCMAKE_BUILD_TYPE=Release && "
                           "cmake --build build-verify -j 2 && "
                           "ctest --test-dir build-verify --output-on-failure || "
                           "cmake --build build-verify -j 2"},
        {"package.json", "npm test --silent --if-present || npm run build --silent --if-present"},
        {"Cargo.toml", "cargo test --quiet || cargo build --quiet"},
        {"go.mod", "go build ./... && go test ./... 2>&1 | tail -40"},
        {"pyproject.toml", "python3 -m pytest -q 2>&1 | tail -30 || python3 -m compileall -q ."},
        {"setup.py", "python3 -m pytest -q 2>&1 | tail -30 || python3 setup.py --quiet build"},
        {"Makefile", "make -j 2"},
        {"meson.build", "meson setup build-verify >/dev/null && ninja -C build-verify"},
        {"pom.xml", "mvn -q -DskipTests package"},
        {"build.gradle", "gradle build -q"},
    };
    for (const Candidate& c : kCandidates) {
        Result<FileInfo> info = fs_stat(ws, c.probe);
        if (info.ok()) return c.command;
    }
    // Fall back to a syntax sweep of the dominant language.
    Result<ProjectMap> map = fs_project_map(ws, 40);
    if (map.ok()) {
        bool has_cpp = false, has_py = false, has_js = false;
        for (const auto& kv : map->ext_counts) {
            if (kv.first == ".cpp" || kv.first == ".cc" || kv.first == ".c") has_cpp = true;
            if (kv.first == ".py") has_py = true;
            if (kv.first == ".js" || kv.first == ".ts") has_js = true;
        }
        if (has_py) return "python3 -m compileall -q . 2>&1 | tail -20";
        if (has_cpp) return "for f in $(find . -name '*.cpp' -o -name '*.cc' | head -40); do "
                            "c++ -std=c++17 -fsyntax-only -Iinclude -I. \"$f\" || exit 1; done";
        if (has_js) return "node --check $(find . -name '*.js' | head -5) 2>&1 | tail -20";
    }
    return {};
}

std::string render_tool_result(const ToolResult& result, size_t max_bytes) {
    std::string body = result.text;
    if (body.size() > max_bytes) body = ellipsize(body, max_bytes);
    return body;
}

// -----------------------------------------------------------------------------
// Agent lifecycle
// -----------------------------------------------------------------------------
Agent::Agent(AgentConfig cfg) : cfg_(std::move(cfg)) {
    tools_ = default_tool_specs();
}

Agent::~Agent() = default;

Error Agent::initialise() {
    if (initialised_) return {};

    // Memory budget first: everything downstream is accountable to it.
    Error mem_err = MemoryGuard::instance().initialise_from_hardware();
    if (!mem_err.ok()) LCA_LOG_WARN(kScope, "memory guard: " + mem_err.str());

    Result<Workspace> ws = workspace_open(cfg_.workspace);
    if (!ws.ok()) return Error(ws.error());
    workspace_ = *ws;

    sandbox_.set_policy(cfg_.sandbox);
    sandbox_.set_workspace_root(workspace_.root_abs);

    std::string note;
    brain_ = make_brain(cfg_.brain, cfg_.local_model, cfg_.heuristic, &note);
    brain_note_ = note;
    if (!brain_) {
        return LCA_FAIL(Code::ModelError, note.empty() ? "no brain available" : note);
    }
    if (!note.empty()) LCA_LOG_INFO(kScope, note);

    initialised_ = true;
    LCA_LOG_DEBUG(kScope, "agent ready: brain=" + brain_->name() + ", root=" + workspace_.root_abs);
    return {};
}

// -----------------------------------------------------------------------------
// Tool dispatch
// -----------------------------------------------------------------------------
Result<ToolResult> Agent::call_tool(const ToolCall& call) {
    if (!initialised_) return LCA_FAIL(Code::Internal, "agent not initialised");
    int64_t started = now_millis();
    Result<ToolResult> result = [&]() -> Result<ToolResult> {
        if (call.tool == "fs_read")        return tool_fs_read(call.args);
        if (call.tool == "fs_write")       return tool_fs_write(call.args);
        if (call.tool == "fs_edit")        return tool_fs_edit(call.args);
        if (call.tool == "fs_patch")       return tool_fs_patch(call.args);
        if (call.tool == "fs_list")        return tool_fs_list(call.args);
        if (call.tool == "fs_glob")        return tool_fs_glob(call.args);
        if (call.tool == "fs_grep")        return tool_fs_grep(call.args);
        if (call.tool == "project_map")    return tool_project_map(call.args);
        if (call.tool == "run_command")    return tool_run_command(call.args);
        if (call.tool == "web_search")     return tool_web_search(call.args);
        if (call.tool == "fetch_page")     return tool_fetch_page(call.args);
        if (call.tool == "memory_report")  return tool_memory_report(call.args);
        if (call.tool == "finish") {
            ToolResult r;
            r.ok = true;
            r.data = Json::object();
            r.data["kind"] = "finish";
            r.text = arg_string(call.args, "summary", "Task finished.");
            return r;
        }
        return LCA_FAIL(Code::NotFound, "unknown tool '" + call.tool + "'");
    }();
    if (result.ok()) result->elapsed_ms = now_millis() - started;
    return result;
}

// -----------------------------------------------------------------------------
// Individual tools
// -----------------------------------------------------------------------------
Result<ToolResult> Agent::tool_project_map(const Json& args) {
    size_t max_files = size_t(arg_int(args, "max_files", 80));
    Result<ProjectMap> map = fs_project_map(workspace_, max_files);
    if (!map.ok()) return Error(map.error());
    ToolResult r;
    r.ok = true;
    r.text = map->summary_text(max_files);
    r.data = map->to_json();
    return r;
}

Result<ToolResult> Agent::tool_fs_read(const Json& args) {
    std::string path = arg_string(args, "path");
    if (path.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_read requires 'path'");
    size_t max_bytes = size_t(arg_int(args, "max_bytes", 512 * 1024));
    Result<ReadResult> file = fs_read(workspace_, path, max_bytes);
    if (!file.ok()) return Error(file.error());
    size_t start_line = size_t(std::max<int64_t>(1, arg_int(args, "start_line", 1)));
    size_t end_line = size_t(std::max<int64_t>(0, arg_int(args, "end_line", 0)));
    ToolResult r;
    r.ok = true;
    r.text = "// " + file->info.rel + " (" + std::to_string(file->line_count) + " lines, " +
             human_bytes(file->info.size) + (file->truncated ? ", truncated" : "") + ")\n" +
             numbered_lines(file->text, start_line, end_line);
    r.data = Json::object();
    r.data["path"] = file->info.rel;
    r.data["lines"] = int64_t(file->line_count);
    r.data["bytes"] = int64_t(file->info.size);
    r.data["language"] = file->language_hint;
    r.data["content"] = file->text;
    return r;
}

Result<ToolResult> Agent::tool_fs_write(const Json& args) {
    std::string path = arg_string(args, "path");
    if (path.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_write requires 'path'");
    if (!args.has("content")) return LCA_FAIL(Code::InvalidArgument, "fs_write requires 'content'");
    std::string content = args["content"].is_string() ? args["content"].as_string()
                                                      : args["content"].dump();
    if (cfg_.dry_run) {
        ToolResult r;
        r.ok = true;
        r.text = "dry run: would write " + std::to_string(content.size()) + " bytes to " + path;
        return r;
    }
    Result<WriteResult> w = fs_write(workspace_, path, content, true);
    if (!w.ok()) return Error(w.error());
    ToolResult r;
    r.ok = true;
    r.text = (w->created ? "created " : "updated ") + fs_relative(workspace_, w->path) + " (" +
             std::to_string(w->bytes_written) + " bytes)" +
             (w->backup_path.empty() ? "" : ", backup: " + basename_of(w->backup_path));
    if (!w->diff.empty()) r.text += "\n" + ellipsize(w->diff, 4000);
    r.data = Json::object();
    r.data["path"] = fs_relative(workspace_, w->path);
    r.data["created"] = w->created;
    r.data["bytes"] = int64_t(w->bytes_written);
    r.data["diff"] = w->diff;
    changed_paths_.push_back(fs_relative(workspace_, w->path));
    return r;
}

Result<ToolResult> Agent::tool_fs_edit(const Json& args) {
    std::string path = arg_string(args, "path");
    if (path.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_edit requires 'path'");
    std::string op = lower(arg_string(args, "op", "replace"));
    EditRequest req;
    req.path = path;
    req.find = arg_string(args, "find");
    req.replace = arg_string(args, "replace");
    req.content = arg_string(args, "content", req.replace);
    req.line = size_t(std::max<int64_t>(0, arg_int(args, "line", 0)));
    req.end_line = size_t(std::max<int64_t>(0, arg_int(args, "end_line", 0)));
    req.dry_run = cfg_.dry_run || arg_bool(args, "dry_run", false);
    if (op == "replace" || op == "replace_exact")      req.op = EditRequest::Op::ReplaceExact;
    else if (op == "replace_all")                      req.op = EditRequest::Op::ReplaceAll;
    else if (op == "replace_lines")                    req.op = EditRequest::Op::ReplaceLines;
    else if (op == "insert_before")                    req.op = EditRequest::Op::InsertBeforeLine;
    else if (op == "insert_after")                     req.op = EditRequest::Op::InsertAfterLine;
    else if (op == "delete_lines")                     req.op = EditRequest::Op::DeleteLines;
    else if (op == "create")                           req.op = EditRequest::Op::CreateFile;
    else if (op == "delete")                           req.op = EditRequest::Op::DeleteFile;
    else return LCA_FAIL(Code::InvalidArgument, "unknown fs_edit op '" + op + "'");

    Result<EditOutcome> outcome = fs_edit(workspace_, req);
    if (!outcome.ok()) return Error(outcome.error());
    ToolResult r;
    r.ok = true;
    r.text = std::string(req.dry_run ? "dry run: " : "") + "edited " + outcome->path + " (" +
             std::to_string(outcome->replacements) + " change(s), " +
             std::to_string(outcome->bytes_before) + " -> " + std::to_string(outcome->bytes_after) +
             " bytes)";
    if (!outcome->diff.empty()) r.text += "\n" + ellipsize(outcome->diff, 4000);
    r.data = Json::object();
    r.data["path"] = outcome->path;
    r.data["replacements"] = int64_t(outcome->replacements);
    r.data["diff"] = outcome->diff;
    if (!req.dry_run) changed_paths_.push_back(outcome->path);
    return r;
}

Result<ToolResult> Agent::tool_fs_patch(const Json& args) {
    std::string patch = arg_string(args, "patch");
    if (patch.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_patch requires 'patch'");
    bool dry_run = arg_bool(args, "dry_run", false) || cfg_.dry_run;
    Result<PatchOutcome> outcome = fs_apply_patch(workspace_, patch, dry_run);
    if (!outcome.ok()) return Error(outcome.error());
    ToolResult r;
    r.ok = outcome->failures.empty();
    r.text = std::string(dry_run ? "dry run: " : "") + "patch touched " +
             std::to_string(outcome->files_changed.size()) + " file(s), +" +
             std::to_string(outcome->added_lines) + "/-" + std::to_string(outcome->removed_lines);
    if (!outcome->failures.empty()) r.text += "\nfailures:\n  " + join(outcome->failures, "\n  ");
    r.data = Json::object();
    Json files = Json::array();
    for (const std::string& f : outcome->files_changed) {
        files.push_back(Json(f));
        if (!dry_run) changed_paths_.push_back(f);
    }
    r.data["files"] = files;
    r.data["added_lines"] = int64_t(outcome->added_lines);
    r.data["removed_lines"] = int64_t(outcome->removed_lines);
    if (!outcome->failures.empty()) {
        Json fails = Json::array();
        for (const std::string& f : outcome->failures) fails.push_back(Json(f));
        r.data["failures"] = fails;
    }
    return r;
}

Result<ToolResult> Agent::tool_fs_list(const Json& args) {
    std::string path = arg_string(args, "path", ".");
    bool recursive = arg_bool(args, "recursive", false);
    size_t max_entries = size_t(arg_int(args, "max_entries", 400));
    Result<std::vector<FileInfo>> entries = fs_list(workspace_, path, recursive, max_entries);
    if (!entries.ok()) return Error(entries.error());
    std::string text;
    Json arr = Json::array();
    for (const FileInfo& e : *entries) {
        text += e.rel + (e.is_dir ? "/" : "") + (e.is_dir ? "" : "  (" + human_bytes(e.size) + ")") + "\n";
        Json j = Json::object();
        j["path"] = e.rel;
        j["dir"] = e.is_dir;
        j["size"] = int64_t(e.size);
        arr.push_back(j);
    }
    if (text.empty()) text = "(empty)\n";
    ToolResult r;
    r.ok = true;
    r.text = text;
    r.data = Json::object();
    r.data["entries"] = arr;
    r.data["count"] = int64_t(entries->size());
    return r;
}

Result<ToolResult> Agent::tool_fs_glob(const Json& args) {
    std::string pattern = arg_string(args, "pattern");
    if (pattern.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_glob requires 'pattern'");
    size_t max_results = size_t(arg_int(args, "max_results", 200));
    Result<std::vector<FileInfo>> matches = fs_glob(workspace_, pattern, max_results);
    if (!matches.ok()) return Error(matches.error());
    std::string text;
    Json arr = Json::array();
    for (const FileInfo& e : *matches) {
        text += e.rel + (e.is_dir ? "/" : "") + "\n";
        arr.push_back(Json(e.rel));
    }
    if (text.empty()) text = "no files match '" + pattern + "'\n";
    ToolResult r;
    r.ok = true;
    r.text = text;
    r.data = Json::object();
    r.data["matches"] = arr;
    r.data["count"] = int64_t(matches->size());
    return r;
}

Result<ToolResult> Agent::tool_fs_grep(const Json& args) {
    std::string pattern = arg_string(args, "pattern");
    if (pattern.empty()) return LCA_FAIL(Code::InvalidArgument, "fs_grep requires 'pattern'");
    GrepOptions opts;
    opts.regex = arg_bool(args, "regex", false);
    opts.ignore_case = arg_bool(args, "ignore_case", true);
    opts.include_glob = arg_string(args, "include", "*");
    opts.exclude_glob = arg_string(args, "exclude", "");
    opts.max_hits = size_t(arg_int(args, "max_hits", 200));
    opts.context_lines = int(arg_int(args, "context", 0));
    Result<GrepSummary> summary = fs_grep(workspace_, pattern, opts);
    if (!summary.ok()) return Error(summary.error());
    std::string text;
    std::string last_file;
    for (const GrepHit& hit : summary->hits) {
        if (hit.rel != last_file) {
            text += "\n" + hit.rel + "\n";
            last_file = hit.rel;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%5zu| ", hit.line);
        text += std::string(hit.is_match ? "" : "  ") + buf + trim(hit.text) + "\n";
    }
    if (text.empty()) text = "no matches for '" + pattern + "'\n";
    ToolResult r;
    r.ok = true;
    r.text = "searched " + std::to_string(summary->files_scanned) + " files, " +
             std::to_string(summary->files_matched) + " matched, " +
             std::to_string(summary->hits.size()) + " hits" +
             (summary->truncated ? " (truncated)" : "") + "\n" + text;
    r.data = Json::object();
    r.data["files_scanned"] = int64_t(summary->files_scanned);
    r.data["files_matched"] = int64_t(summary->files_matched);
    r.data["hits"] = int64_t(summary->hits.size());
    return r;
}

Result<ToolResult> Agent::tool_run_command(const Json& args) {
    std::string command = arg_string(args, "command");
    if (command.empty()) return LCA_FAIL(Code::InvalidArgument, "run_command requires 'command'");
    ExecRequest req;
    req.command = command;
    req.cwd = arg_string(args, "cwd", workspace_.root_abs);
    req.timeout_ms = int(arg_int(args, "timeout_ms", cfg_.command_timeout_ms));
    req.max_output_bytes = cfg_.max_tool_output;
    req.max_memory_bytes = std::max<uint64_t>(256ull * 1024 * 1024,
                                              MemoryGuard::instance().budget_bytes() / 3);
    req.dry_run = cfg_.dry_run;
    if (!cfg_.allow_network) {
        // Best-effort: refuse common network clients when networking is off.
        for (const char* net_tool : {"curl", "wget", "nc", "ssh", "scp", "ftp", "telnet"}) {
            if (contains(command, net_tool)) {
                return LCA_FAIL(Code::SandboxViolation,
                                std::string("network command '") + net_tool +
                                    "' is disabled (--no-network)");
            }
        }
    }
    if (cfg_.confirm_commands) {
        std::fprintf(stderr, "\n[lca] run this command? %s\n      [y/N] ", command.c_str());
        std::fflush(stderr);
        char answer[16] = {0};
        if (!std::fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y'))
            return LCA_FAIL(Code::Cancelled, "command declined by the operator");
    }
    Result<ExecResult> res = sandbox_.run(req);
    if (!res.ok()) return Error(res.error());
    ToolResult r;
    r.ok = res->success();
    r.text = std::string("$ ") + command + "\n" + res->summary() + "\n";
    if (!res->out.empty()) r.text += "--- stdout ---\n" + ellipsize(res->out, cfg_.max_tool_output);
    if (!res->err.empty()) r.text += "--- stderr ---\n" + ellipsize(res->err, cfg_.max_tool_output);
    if (res->out.empty() && res->err.empty()) r.text += "(no output)\n";
    if (res->out_truncated || res->err_truncated) r.text += "(output truncated)\n";
    r.data = res->to_json();
    return r;
}

Result<ToolResult> Agent::tool_web_search(const Json& args) {
    if (!cfg_.allow_network)
        return LCA_FAIL(Code::PermissionDenied, "network access is disabled (--no-network)");
    SearchRequest req;
    req.query = arg_string(args, "query");
    if (req.query.empty()) return LCA_FAIL(Code::InvalidArgument, "web_search requires 'query'");
    req.engine = arg_string(args, "engine", "auto");
    req.max_results = int(arg_int(args, "max_results", 8));
    req.timeout_ms = int(arg_int(args, "timeout_ms", 20000));
    req.fetch_snippets = arg_bool(args, "fetch_snippets", false);
    Result<SearchResponse> response = web_search(req);
    if (!response.ok()) return Error(response.error());
    ToolResult r;
    r.ok = !response->results.empty();
    r.text = response->to_text();
    r.data = response->to_json();
    return r;
}

Result<ToolResult> Agent::tool_fetch_page(const Json& args) {
    if (!cfg_.allow_network)
        return LCA_FAIL(Code::PermissionDenied, "network access is disabled (--no-network)");
    std::string url = arg_string(args, "url");
    if (url.empty()) return LCA_FAIL(Code::InvalidArgument, "fetch_page requires 'url'");
    FetchOptions opts;
    opts.timeout_ms = int(arg_int(args, "timeout_ms", 20000));
    opts.max_bytes = size_t(arg_int(args, "max_bytes", 1 << 20));
    opts.only_text = arg_bool(args, "only_text", false);
    opts.check_robots = arg_bool(args, "check_robots", true);
    Result<PageContent> page = fetch_page(url, opts);
    if (!page.ok()) return Error(page.error());
    ToolResult r;
    r.ok = page->status >= 200 && page->status < 300;
    r.text = page->to_text(size_t(arg_int(args, "max_chars", 20000)));
    r.data = page->to_json();
    return r;
}

Result<ToolResult> Agent::tool_memory_report(const Json&) {
    MemoryGuard& guard = MemoryGuard::instance();
    ToolResult r;
    r.ok = true;
    r.text = guard.report_text();
    r.data = guard.report_json();
    return r;
}

// -----------------------------------------------------------------------------
// Verification
// -----------------------------------------------------------------------------
Error Agent::run_verification(RunReport* report, int step_index, std::string* transcript_note) {
    std::string command = cfg_.verify_command.empty() ? detect_verify_command() : cfg_.verify_command;
    if (command.empty()) {
        if (transcript_note)
            *transcript_note = "no build system detected; skipping automatic verification";
        return {};
    }
    ToolCall call;
    call.id = "verify-" + std::to_string(step_index);
    call.tool = "run_command";
    call.args = Json::object();
    call.args["command"] = command;
    call.args["timeout_ms"] = int64_t(std::max(cfg_.command_timeout_ms, 180000));
    call.rationale = "verify the changes compile and pass";
    Result<ToolResult> result = call_tool(call);
    if (!result.ok()) {
        report->failures++;
        if (transcript_note) *transcript_note = "verification failed to run: " + result.error().str();
        return {};
    }
    AgentStep step;
    step.index = step_index;
    step.call = call;
    step.result = *result;
    step.when_ms = wall_millis();
    step.verification = true;
    report->steps.push_back(step);
    report->tool_calls++;
    report->commands_run.push_back(command);
    report->verification_output = result->text;
    report->verified = result->ok;
    if (!result->ok) report->failures++;
    if (transcript_note) {
        *transcript_note = std::string("verification ") + (result->ok ? "passed" : "FAILED") +
                           ":\n" + ellipsize(result->text, 3000);
    }
    return {};
}

std::string Agent::detect_verify_command() const {
    if (!cfg_.verify_command.empty()) return cfg_.verify_command;
    return infer_verify_command(workspace_);
}

// -----------------------------------------------------------------------------
// The loop
// -----------------------------------------------------------------------------
Result<RunReport> Agent::run(const std::string& task) {
    if (!initialised_) {
        Error e = initialise();
        if (!e.ok()) return Error(e);
    }
    if (trim(task).empty()) return LCA_FAIL(Code::InvalidArgument, "empty task");

    RunReport report;
    report.task = task;
    report.brain = brain_->name();
    int64_t started = now_millis();

    // Workspace summary for the brain's context.
    std::string workspace_summary;
    {
        Result<ProjectMap> map = fs_project_map(workspace_, 60);
        if (map.ok()) workspace_summary = map->summary_text(60);
    }
    std::string verify = detect_verify_command();
    std::vector<std::string> notes;
    if (!verify.empty()) notes.push_back("verification command: " + verify);

    std::vector<AgentMessage> history;
    {
        AgentMessage m;
        m.role = "user";
        m.content = task;
        m.when_ms = wall_millis();
        history.push_back(m);
    }

    std::string last_tool;
    int repeats = 0;
    bool edited = false;
    bool verified_once = false;
    int finish_step = 0;

    for (int step = 0; step < cfg_.max_steps; ++step) {
        BrainContext ctx;
        ctx.task = task;
        ctx.workspace_summary = workspace_summary;
        ctx.history = history;
        ctx.tools = tools_;
        ctx.step = step;
        ctx.max_steps = cfg_.max_steps;
        ctx.notes = notes;

        Result<BrainReply> reply = brain_->step(ctx);
        if (!reply.ok()) {
            report.summary = "brain error: " + reply.error().str();
            report.ok = false;
            break;
        }
        if (!reply->text.empty()) {
            AgentMessage m;
            m.role = "assistant";
            m.content = reply->text;
            m.when_ms = wall_millis();
            history.push_back(m);
            if (cfg_.verbose) LCA_LOG_INFO(kScope, "brain: " + ellipsize(reply->text, 400));
        }
        if (reply->calls.empty()) {
            report.finished = true;
            report.final_answer = reply->final_answer.empty() ? reply->text : reply->final_answer;
            report.ok = true;
            break;
        }

        bool saw_finish = false;
        bool saw_edit = false;
        for (const ToolCall& call : reply->calls) {
            report.tool_calls++;
            if (call.tool == "finish") {
                saw_finish = true;
                finish_step = step;
            }
            Result<ToolResult> result = call_tool(call);
            AgentStep recorded;
            recorded.index = int(report.steps.size()) + 1;
            recorded.call = call;
            recorded.when_ms = wall_millis();
            if (result.ok()) {
                recorded.result = *result;
            } else {
                recorded.result.ok = false;
                recorded.result.error = result.error().str();
                recorded.result.text = "error: " + result.error().str();
                report.failures++;
            }
            report.steps.push_back(recorded);
            if (call.tool == "run_command") {
                report.commands_run.push_back(arg_string(call.args, "command"));
            }
            if (!recorded.result.ok && call.tool != "finish") {
                // Feed failures back so the brain can self-correct.
                AgentMessage m;
                m.role = "tool";
                m.tool_name = call.tool;
                m.ok = false;
                m.content = recorded.result.text;
                m.when_ms = wall_millis();
                history.push_back(m);
                brain_->observe(call, recorded.result.data, false);
                continue;
            }
            if (call.tool == "fs_write" || call.tool == "fs_edit" || call.tool == "fs_patch") {
                saw_edit = true;
                edited = true;
            }
            AgentMessage m;
            m.role = "tool";
            m.tool_name = call.tool;
            m.ok = recorded.result.ok;
            m.content = render_tool_result(recorded.result, cfg_.max_tool_output);
            m.when_ms = wall_millis();
            history.push_back(m);
            brain_->observe(call, recorded.result.data, recorded.result.ok);

            if (call.tool == "finish") {
                report.finished = true;
                report.final_answer = arg_string(call.args, "summary", recorded.result.text);
            }
        }

        // Loop protection: the same tool with the same args twice in a row.
        std::string fingerprint = reply->calls.front().tool + reply->calls.front().args.dump();
        if (fingerprint == last_tool) ++repeats; else repeats = 0;
        last_tool = fingerprint;
        if (repeats >= 3) {
            report.summary = "stopped: the planner repeated the same action three times";
            break;
        }

        // History window.
        if (history.size() > cfg_.max_history_messages) {
            history.erase(history.begin() + 1, history.begin() + ptrdiff_t(history.size() - cfg_.max_history_messages));
        }

        // Automatic verification after the first edit, once.
        if (cfg_.auto_verify && edited && !verified_once && !saw_edit) {
            std::string note;
            run_verification(&report, int(report.steps.size()) + 1, &note);
            verified_once = true;
            if (!note.empty()) {
                notes.push_back(note);
                AgentMessage m;
                m.role = "tool";
                m.tool_name = "verify";
                m.ok = report.verified;
                m.content = note;
                m.when_ms = wall_millis();
                history.push_back(m);
            }
        }
        if (saw_finish) {
            if (cfg_.auto_verify && edited && !verified_once) {
                std::string note;
                run_verification(&report, int(report.steps.size()) + 1, &note);
                verified_once = true;
                if (!note.empty()) notes.push_back(note);
            }
            break;
        }
    }

    // A run that never verified, or whose verification failed, is not a success.
    if (report.failures > 0 && !report.verified) report.ok = report.ok && report.failures == 0;
    if (!report.finished && report.summary.empty())
        report.summary = "step limit reached (" + std::to_string(cfg_.max_steps) + " steps)";
    if (!report.finished && report.ok) report.ok = false;
    if (report.finished) report.ok = report.failures == 0 || report.verified;
    if (report.final_answer.empty() && !report.summary.empty()) report.final_answer = report.summary;
    if (report.summary.empty()) {
        report.summary = report.ok ? "task completed"
                               : ("task finished with " + std::to_string(report.failures) +
                                  " failed step(s)");
    }
    report.changed_paths = changed_paths_;
    {
        std::set<std::string> unique(report.changed_paths.begin(), report.changed_paths.end());
        report.changed_paths.assign(unique.begin(), unique.end());
    }
    report.elapsed_ms = now_millis() - started;
    report.peak_ram_bytes = MemoryGuard::instance().peak_bytes() +
                            MemoryGuard::instance().process_peak_rss_bytes();
    report.peak_vram_bytes = MemoryGuard::instance().vram_used_bytes();
    if (!report.changed_paths.empty() && report.verification_output.empty()) {
        report.summary += " (no verification command available)";
    }
    (void)finish_step;
    return report;
}

}  // namespace lca
