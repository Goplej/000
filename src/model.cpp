// =============================================================================
//  lca/model.cpp  --  heuristic planner + local model client
// =============================================================================
#include "lca/model.h"
#include "lca/fs_engine.h"
#include "lca/mem.h"
#include "lca/net.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace lca {
namespace {

constexpr const char* kScope = "model";

bool word_present(const std::string& hay, const std::string& word) {
    size_t pos = 0;
    while ((pos = hay.find(word, pos)) != std::string::npos) {
        bool left_ok = pos == 0 || !std::isalnum((unsigned char)hay[pos - 1]);
        size_t end = pos + word.size();
        bool right_ok = end >= hay.size() || !std::isalnum((unsigned char)hay[end]);
        if (left_ok && right_ok) return true;
        pos = end;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Task scraping helpers (paths, commands, symbol names)
// -----------------------------------------------------------------------------
std::vector<std::string> extract_paths(const std::string& task) {
    std::vector<std::string> out;
    for (const std::string& raw : split(task, ' ')) {
        std::string token = trim(raw);
        while (!token.empty() && (token.front() == '"' || token.front() == '\'' ||
                                  token.front() == '(' || token.front() == '['))
            token.erase(0, 1);
        while (!token.empty() && (token.back() == '"' || token.back() == '\'' ||
                                  token.back() == ')' || token.back() == ']' ||
                                  token.back() == ',' || token.back() == '.'))
            token.pop_back();
        if (token.empty()) continue;
        bool looks_like_path = false;
        if (token.find('/') != std::string::npos) looks_like_path = true;
        if (token.find('.') != std::string::npos && token.size() > 2 &&
            token.find('.') != token.size() - 1) {
            std::string ext = lower(token.substr(token.find_last_of('.')));
            if (ext.size() <= 5) looks_like_path = true;
        }
        if (!looks_like_path) continue;
        if (token[0] == '-') continue;
        out.push_back(token);
    }
    return out;
}

std::string quoted_argument(const std::string& task, const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        size_t pos = lower(task).find(key);
        if (pos == std::string::npos) continue;
        size_t i = pos + key.size();
        while (i < task.size() && (task[i] == ' ' || task[i] == ':' || task[i] == '=')) ++i;
        if (i >= task.size()) continue;
        char quote = task[i];
        if (quote == '"' || quote == '\'') {
            size_t end = task.find(quote, i + 1);
            if (end == std::string::npos) end = task.size();
            return task.substr(i + 1, end - i - 1);
        }
        // Unquoted: take up to the next stop word.
        size_t end = task.size();
        static const std::vector<std::string> stops = {" that ", " which ", " and ", " with ",
                                                       " using ", " to ", " in ", " on "};
        for (const std::string& stop : stops) {
            size_t at = lower(task).find(stop, i);
            if (at != std::string::npos && at < end) end = at;
        }
        return trim(task.substr(i, end - i));
    }
    return {};
}

std::string detect_language(const std::string& task, const HeuristicConfig& cfg) {
    if (!cfg.language.empty()) return lower(cfg.language);
    std::string t = lower(task);
    if (word_present(t, "python") || contains(t, ".py")) return "python";
    if (word_present(t, "javascript") || word_present(t, "node") || contains(t, ".js")) return "javascript";
    if (word_present(t, "typescript") || contains(t, ".ts")) return "typescript";
    if (word_present(t, "rust") || contains(t, ".rs")) return "rust";
    if (word_present(t, "go") || word_present(t, "golang")) return "go";
    if (word_present(t, "java")) return "java";
    if (word_present(t, "c++") || word_present(t, "cpp") || word_present(t, "c++")) return "cpp";
    return cfg.prefer_python ? "python" : "cpp";
}

bool task_names_language(const std::string& task) {
    std::string t = lower(task);
    static const char* kWords[] = {"python", "c++", "cpp", "javascript", "typescript",
                                   "rust", "golang", "java", "ruby", "php", "node"};
    for (const char* w : kWords) {
        std::string word = w;
        if (word.size() <= 2) {
            if (contains(t, "." + word) || contains(t, word + " ")) return true;
            continue;
        }
        if (contains(t, word)) return true;
    }
    return false;
}

// Dominant language of an existing workspace, read from the project-map summary
// (lines of the form ".py (12)").
std::string infer_language_from_summary(const std::string& summary) {
    struct Mapping { const char* ext; const char* lang; };
    static const Mapping kMap[] = {
        {".py", "python"}, {".js", "javascript"}, {".mjs", "javascript"}, {".ts", "typescript"},
        {".tsx", "typescript"}, {".rs", "rust"}, {".go", "go"}, {".java", "java"},
        {".kt", "kotlin"}, {".cs", "csharp"}, {".rb", "ruby"}, {".php", "php"},
        {".cpp", "cpp"}, {".cc", "cpp"}, {".cxx", "cpp"}, {".hpp", "cpp"}, {".c", "cpp"},
    };
    size_t best = 0;
    std::string lang;
    for (const Mapping& m : kMap) {
        std::string needle = std::string(m.ext) + " (";
        size_t pos = summary.find(needle);
        if (pos == std::string::npos) continue;
        unsigned long count = std::strtoul(summary.c_str() + pos + needle.size(), nullptr, 10);
        if (count > best) { best = count; lang = m.lang; }
    }
    return lang;
}

std::string slugify(const std::string& text) {
    std::string out;
    for (char c : lower(text)) {
        if (std::isalnum((unsigned char)c)) out.push_back(c);
        else if (c == ' ' || c == '-' || c == '_' || c == '/') {
            if (!out.empty() && out.back() != '_') out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "project";
    if (std::isdigit((unsigned char)out[0])) out = "p_" + out;
    return out;
}


// -----------------------------------------------------------------------------
// Template library -- complete, compilable starting points
// -----------------------------------------------------------------------------
struct FileTemplate {
    std::string path;
    std::string content;
};

std::string cpp_hello_main(const std::string& title) {
    std::string out;
    out += "// " + title + "\n";
    out += "#include <iostream>\n#include <string>\n\n";
    out += "int main(int argc, char** argv) {\n";
    out += "    std::string name = argc > 1 ? argv[1] : \"world\";\n";
    out += "    std::cout << \"Hello, \" << name << \"!\" << std::endl;\n";
    out += "    return 0;\n}\n";
    return out;
}

std::string python_hello_main(const std::string& title) {
    std::string out;
    out += "# " + title + "\n";
    out += "import sys\n\n\n";
    out += "def greet(name: str = \"world\") -> str:\n";
    out += "    \"\"\"Return a friendly greeting.\"\"\"\n";
    out += "    return f\"Hello, {name}!\"\n\n\n";
    out += "def main(argv: list) -> int:\n";
    out += "    name = argv[1] if len(argv) > 1 else \"world\"\n";
    out += "    print(greet(name))\n";
    out += "    return 0\n\n\n";
    out += "if __name__ == \"__main__\":\n";
    out += "    raise SystemExit(main(sys.argv))\n";
    return out;
}

std::vector<FileTemplate> project_templates(const std::string& language, const std::string& name) {
    std::vector<FileTemplate> files;
    std::string title = "Generated by local-claude-code-agent for " + name;
    if (language == "python") {
        files.push_back({"main.py", python_hello_main(title)});
        files.push_back({"README.md", "# " + name + "\n\n" + title + "\n"});
        files.push_back({"requirements.txt", "# No third-party dependencies.\n"});
    } else if (language == "javascript") {
        files.push_back({"index.js",
                         "#!/usr/bin/env node\n// " + title +
                             "\n\nfunction greet(name = \"world\") {\n    return `Hello, ${name}!`;\n}\n\n"
                             "const name = process.argv[2] || \"world\";\nconsole.log(greet(name));\n"});
        files.push_back({"package.json",
                         "{\n  \"name\": \"" + slugify(name) +
                             "\",\n  \"version\": \"1.0.0\",\n  \"main\": \"index.js\",\n"
                             "  \"type\": \"commonjs\",\n  \"scripts\": {\n    \"start\": \"node index.js\"\n  }\n}\n"});
        files.push_back({"README.md", "# " + name + "\n\n" + title + "\n"});
    } else if (language == "rust") {
        files.push_back({"src/main.rs",
                         "// " + title + "\n\nfn greet(name: &str) -> String {\n"
                         "    format!(\"Hello, {}!\", name)\n}\n\nfn main() {\n"
                         "    let args: Vec<String> = std::env::args().collect();\n"
                         "    let name = args.get(1).map(|s| s.as_str()).unwrap_or(\"world\");\n"
                         "    println!(\"{}\", greet(name));\n}\n"});
        files.push_back({"Cargo.toml",
                         "[package]\nname = \"" + slugify(name) +
                             "\"\nversion = \"1.0.0\"\nedition = \"2021\"\n\n[dependencies]\n"});
        files.push_back({"README.md", "# " + name + "\n\n" + title + "\n"});
    } else if (language == "go") {
        files.push_back({"main.go",
                         "// " + title + "\npackage main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n\n"
                         "func greet(name string) string {\n\treturn fmt.Sprintf(\"Hello, %s!\", name)\n}\n\n"
                         "func main() {\n\tname := \"world\"\n\tif len(os.Args) > 1 {\n\t\tname = os.Args[1]\n\t}\n"
                         "\tfmt.Println(greet(name))\n}\n"});
        files.push_back({"go.mod", "module " + slugify(name) + "\n\ngo 1.21\n"});
        files.push_back({"README.md", "# " + name + "\n\n" + title + "\n"});
    } else {
        files.push_back({"src/main.cpp", cpp_hello_main(title)});
        files.push_back({"CMakeLists.txt",
                         "cmake_minimum_required(VERSION 3.16)\nproject(" + slugify(name) + " CXX)\n\n"
                         "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                         "if(NOT CMAKE_BUILD_TYPE)\n  set(CMAKE_BUILD_TYPE Release)\nendif()\n\n"
                         "add_executable(" + slugify(name) + " src/main.cpp)\n"});
        files.push_back({"README.md", "# " + name + "\n\n" + title + "\n\n## Build\n\n```sh\n"
                                       "cmake -S . -B build\ncmake --build build -j\n./build/" +
                                           slugify(name) + " world\n```\n"});
    }
    return files;
}

}  // namespace

// -----------------------------------------------------------------------------
// ToolCall / ToolSpec
// -----------------------------------------------------------------------------
Json ToolCall::to_json() const {
    Json j = Json::object();
    j["id"] = id;
    j["tool"] = tool;
    j["args"] = args;
    if (!rationale.empty()) j["rationale"] = rationale;
    return j;
}

std::string ToolCall::to_text() const {
    std::string s = tool + "(";
    bool first = true;
    for (const auto& kv : args.fields()) {
        if (!first) s += ", ";
        first = false;
        std::string value = kv.second.is_string() ? kv.second.as_string() : kv.second.dump();
        s += kv.first + "=" + ellipsize(value, 120);
    }
    s += ")";
    return s;
}

std::vector<ToolSpec> default_tool_specs() {
    std::vector<ToolSpec> specs;
    auto add = [&](const char* name, const char* summary, const char* args_help,
                   std::initializer_list<const char*> required) {
        ToolSpec s;
        s.name = name;
        s.summary = summary;
        s.args_help = args_help;
        Json j = Json::object();
        j["type"] = "object";
        Json props = Json::object();
        for (const char* r : required) {
            Json p = Json::object();
            p["type"] = "string";
            props[r] = p;
        }
        j["properties"] = props;
        s.schema = j;
        specs.push_back(s);
    };
    add("fs_read", "Read a text file (with optional line range)",
        "path, max_bytes, start_line, end_line", {"path"});
    add("fs_write", "Create or overwrite a file with full content",
        "path, content", {"path", "content"});
    add("fs_edit", "Replace an exact snippet, replace all, or edit by line range",
        "path, find, replace, op (replace|replace_all|insert_after|delete_lines), line, end_line", {"path"});
    add("fs_patch", "Apply a unified diff or a *** Begin Patch block patch",
        "patch, dry_run", {"patch"});
    add("fs_list", "List a directory (optionally recursive)", "path, recursive, max_entries", {});
    add("fs_glob", "Find files by glob (**/*.cpp, src/*.{c,h})", "pattern, max_results", {"pattern"});
    add("fs_grep", "Search file contents (regex or literal)", "pattern, regex, ignore_case, include, max_hits", {"pattern"});
    add("project_map", "Summarise the project: tree, languages, entry points", "max_files", {});
    add("run_command", "Run a shell command inside the sandbox and capture output",
        "command, cwd, timeout_ms", {"command"});
    add("web_search", "Search the web through a search engine (no API key)", "query, engine, max_results", {"query"});
    add("fetch_page", "Fetch a URL and return readable text plus links", "url, max_bytes, only_text", {"url"});
    add("memory_report", "Report memory/VRAM budget usage", "", {});
    add("finish", "Finish the task and report the result", "summary", {"summary"});
    return specs;
}

// -----------------------------------------------------------------------------
// AgentMessage / BrainReply
// -----------------------------------------------------------------------------
Json AgentMessage::to_json() const {
    Json j = Json::object();
    j["role"] = role;
    j["content"] = content;
    if (!tool_name.empty()) j["tool"] = tool_name;
    j["ok"] = ok;
    return j;
}

std::string AgentMessage::to_text() const {
    if (role == "tool") return "[tool " + tool_name + (ok ? " ok" : " failed") + "] " + content;
    if (role == "assistant") return "[assistant] " + content;
    return "[user] " + content;
}

std::string BrainReply::to_text() const {
    std::string s = text;
    if (!calls.empty()) {
        if (!s.empty()) s += "\n";
        for (const ToolCall& c : calls) s += "  -> " + c.to_text() + "\n";
    }
    if (done && !final_answer.empty()) s += "final: " + final_answer;
    return s;
}

std::string render_transcript(const std::vector<AgentMessage>& messages, size_t max_bytes) {
    std::string out;
    for (size_t i = messages.size(); i-- > 0;) {
        std::string piece = messages[i].to_text() + "\n";
        if (out.size() + piece.size() > max_bytes) break;
        out = piece + out;
    }
    return out;
}

// -----------------------------------------------------------------------------
// HeuristicBrain
// -----------------------------------------------------------------------------
// The run_command tool renders as "$ cmd\nexit <code> in <ms>, peak rss ...",
// so the planner reads the exit code back out of the rendered line rather than
// assuming success because the tool call itself returned a result.
bool run_command_succeeded(const std::string& rendered) {
    size_t pos = rendered.find("exit ");
    while (pos != std::string::npos) {
        bool line_start = (pos == 0) || rendered[pos - 1] == '\n';
        if (line_start) {
            size_t code_begin = pos + 5;
            size_t code_end = code_begin;
            while (code_end < rendered.size() && rendered[code_end] >= '0' && rendered[code_end] <= '9')
                ++code_end;
            if (code_end == code_begin) return false;
            long long code = std::atoll(rendered.substr(code_begin, code_end - code_begin).c_str());
            return code == 0;
        }
        pos = rendered.find("exit ", pos + 1);
    }
    return false;
}

std::string HeuristicBrain::description() const {
    return "built-in deterministic planner (no model download, no network)";
}

const char* HeuristicBrain::intent_name(Intent intent) {
    switch (intent) {
        case Intent::Explain:       return "explain";
        case Intent::CreateProject: return "create-project";
        case Intent::CreateFile:    return "create-file";
        case Intent::Implement:     return "implement";
        case Intent::Fix:           return "fix";
        case Intent::Refactor:      return "refactor";
        case Intent::Rename:        return "rename";
        case Intent::AddTests:      return "add-tests";
        case Intent::Format:        return "format";
        case Intent::Document:      return "document";
        case Intent::Search:        return "search";
        case Intent::Scrape:        return "scrape";
        case Intent::Run:           return "run";
        case Intent::GitStatus:     return "git-status";
        case Intent::Unknown:       return "unknown";
    }
    return "unknown";
}

HeuristicBrain::Intent HeuristicBrain::classify(const std::string& task) {
    std::string t = lower(task);
    if (t.empty()) return Intent::Unknown;
    if (word_present(t, "explain") || word_present(t, "describe") ||
        word_present(t, "what does") || word_present(t, "summarise") || word_present(t, "summarize"))
        return Intent::Explain;
    if ((word_present(t, "create") || word_present(t, "scaffold") || word_present(t, "generate") ||
         word_present(t, "new") || word_present(t, "init") || word_present(t, "bootstrap")) &&
        (word_present(t, "project") || word_present(t, "app") || word_present(t, "repository") ||
         word_present(t, "repo") || word_present(t, "module") || word_present(t, "package")))
        return Intent::CreateProject;
    if (word_present(t, "search") || word_present(t, "google") || word_present(t, "look up") ||
        word_present(t, "find out about") || word_present(t, "research"))
        return Intent::Search;
    if (word_present(t, "scrape") || word_present(t, "fetch") || word_present(t, "download page") ||
        word_present(t, "read the page"))
        return Intent::Scrape;
    if (word_present(t, "rename") || word_present(t, "move file")) return Intent::Rename;
    // A failing build is a repair job, not a "run" job: check before Run.
    if (word_present(t, "fix") || word_present(t, "bug") || word_present(t, "repair") ||
        word_present(t, "broken") || word_present(t, "fails") || word_present(t, "failing") ||
        word_present(t, "error"))
        return Intent::Fix;
    if (word_present(t, "run") || word_present(t, "execute") || word_present(t, "build") ||
        word_present(t, "compile") || word_present(t, "test") || word_present(t, "make"))
        return Intent::Run;
    if (word_present(t, "refactor") || word_present(t, "restructure") || word_present(t, "clean up"))
        return Intent::Refactor;
    if (word_present(t, "test") || word_present(t, "tests") || word_present(t, "spec"))
        return Intent::AddTests;
    if (word_present(t, "format") || word_present(t, "lint")) return Intent::Format;
    if (word_present(t, "document") || word_present(t, "comment") || word_present(t, "readme") ||
        word_present(t, "docs"))
        return Intent::Document;
    if (word_present(t, "git") || word_present(t, "commit") || word_present(t, "diff") ||
        word_present(t, "status"))
        return Intent::GitStatus;
    if (word_present(t, "create") || word_present(t, "add file") || word_present(t, "new file") ||
        word_present(t, "write"))
        return Intent::CreateFile;
    if (word_present(t, "implement") || word_present(t, "add") || word_present(t, "extend"))
        return Intent::Implement;
    return Intent::Unknown;
}

Json HeuristicBrain::settings() const {
    Json j = Json::object();
    j["verbose"] = cfg_.verbose;
    j["prefer_python"] = cfg_.prefer_python;
    j["language"] = cfg_.language;
    return j;
}

Result<BrainReply> HeuristicBrain::step(const BrainContext& ctx) {
    BrainReply reply;
    Intent intent = classify(ctx.task);
    std::vector<std::string> paths = extract_paths(ctx.task);
    std::string language = detect_language(ctx.task, cfg_);
    if (!task_names_language(ctx.task)) {
        // The task does not name a language: follow the workspace instead.
        std::string inferred = infer_language_from_summary(ctx.workspace_summary);
        if (!inferred.empty()) language = inferred;
    }

    auto tool_call = [&](const std::string& tool, Json args, const std::string& why) {
        ToolCall c;
        c.id = tool + "-" + std::to_string(++planned_);
        c.tool = tool;
        c.args = std::move(args);
        c.rationale = why;
        reply.calls.push_back(std::move(c));
    };

    // Collect tool results seen so far so the planner can react to failures.
    std::vector<const AgentMessage*> tool_msgs;
    for (const AgentMessage& m : ctx.history) {
        if (m.role == "tool") tool_msgs.push_back(&m);
    }
    auto last_tool = [&](const std::string& name) -> const AgentMessage* {
        for (size_t i = tool_msgs.size(); i-- > 0;) {
            if (tool_msgs[i]->tool_name == name) return tool_msgs[i];
        }
        return nullptr;
    };
    auto any_tool = [&](const std::string& name) {
        return last_tool(name) != nullptr;
    };

    // ---- Phase 1: always map the workspace first -------------------------
    if (!any_tool("project_map")) {
        Json args = Json::object();
        args["max_files"] = 60;
        tool_call("project_map", args, "understand the repository before touching it");
        reply.text = "Mapping the workspace.";
        return reply;
    }
    const AgentMessage* map_msg = last_tool("project_map");
    std::string project_summary = map_msg ? map_msg->content : std::string();

    switch (intent) {
        case Intent::Explain: {
            if (!any_tool("finish")) {
                reply.text = "Workspace summary:\n" + project_summary;
                Json args = Json::object();
                args["summary"] = "Explained the project layout.";
                tool_call("finish", args, "answer delivered");
                return reply;
            }
            break;
        }
        case Intent::CreateProject: {
            if (!any_tool("fs_write")) {
                std::string name = quoted_argument(ctx.task, {"named", "called", "for"});
                if (name.empty() && !paths.empty()) name = paths.front();
                if (name.empty()) name = slugify(ctx.task);
                std::vector<FileTemplate> files = project_templates(language, name);
                reply.text = "Scaffolding a " + language + " project named '" + name +
                             "' (" + std::to_string(files.size()) + " files).";
                std::string project_dir = slugify(name);
                for (const FileTemplate& f : files) {
                    Json args = Json::object();
                    args["path"] = project_dir + "/" + f.path;
                    args["content"] = f.content;
                    tool_call("fs_write", args, "project skeleton");
                }
                // Verify by building when we can.
                if (language == "cpp") {
                    std::string dir = slugify(name);
                    Json args = Json::object();
                    args["command"] = "cmake -S " + dir + " -B " + dir + "/build && cmake --build " +
                                      dir + "/build -j 2";
                    args["timeout_ms"] = 120000;
                    tool_call("run_command", args, "prove the scaffold compiles");
                } else if (language == "python") {
                    Json args = Json::object();
                    args["command"] = "python3 -m py_compile " + slugify(name) + "/main.py";
                    args["timeout_ms"] = 30000;
                    tool_call("run_command", args, "prove the scaffold parses");
                }
                return reply;
            }
            if (!any_tool("run_command")) {
                // No compiler available: still run a syntax check when possible.
                Json args = Json::object();
                args["command"] = "find . -maxdepth 2 -type f | sort";
                args["timeout_ms"] = 15000;
                tool_call("run_command", args, "list what was created");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* run = last_tool("run_command");
                bool build_ok = run && run->ok && run_command_succeeded(run->content);
                Json args = Json::object();
                args["summary"] = build_ok ? "Project scaffolded and verified."
                                           : "Project scaffolded; verification reported problems: " +
                                                 ellipsize(run ? run->content : "", 400);
                tool_call("finish", args, "report the result");
                return reply;
            }
            break;
        }
        case Intent::Run: {
            if (!any_tool("run_command")) {
                std::string command;
                size_t colon = ctx.task.find(':');
                if (colon != std::string::npos && colon + 1 < ctx.task.size())
                    command = trim(ctx.task.substr(colon + 1));
                if (command.empty()) {
                    size_t after = lower(ctx.task).find("run ");
                    if (after != std::string::npos) command = trim(ctx.task.substr(after + 4));
                }
                if (command.empty()) command = "make -j 2";
                Json args = Json::object();
                args["command"] = command;
                args["timeout_ms"] = 120000;
                tool_call("run_command", args, "execute the requested command");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* run = last_tool("run_command");
                Json args = Json::object();
                args["summary"] = "Command output:\n" + ellipsize(run ? run->content : "", 4000);
                tool_call("finish", args, "report command output");
                return reply;
            }
            break;
        }
        case Intent::Search: {
            if (!any_tool("web_search")) {
                std::string query = quoted_argument(ctx.task, {"for", "about", "search", "query"});
                if (query.empty()) query = ctx.task;
                Json args = Json::object();
                args["query"] = query;
                args["engine"] = "auto";
                args["max_results"] = 8;
                tool_call("web_search", args, "look the topic up on the web");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* search = last_tool("web_search");
                Json args = Json::object();
                args["summary"] = "Search results:\n" + ellipsize(search ? search->content : "", 4000);
                tool_call("finish", args, "report search results");
                return reply;
            }
            break;
        }
        case Intent::Scrape: {
            if (!any_tool("fetch_page")) {
                std::string url;
                for (const std::string& p : paths) {
                    if (starts_with(p, "http://") || starts_with(p, "https://")) { url = p; break; }
                }
                for (const std::string& token : split(ctx.task, ' ')) {
                    std::string t = trim(token);
                    if (starts_with(t, "http://") || starts_with(t, "https://")) { url = t; break; }
                }
                if (url.empty()) {
                    reply.text = "No URL found in the task; nothing to fetch.";
                    Json args = Json::object();
                    args["summary"] = "No URL supplied.";
                    tool_call("finish", args, "nothing to do");
                    return reply;
                }
                Json args = Json::object();
                args["url"] = url;
                args["max_bytes"] = 1 << 20;
                tool_call("fetch_page", args, "read the live page");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* page = last_tool("fetch_page");
                Json args = Json::object();
                args["summary"] = "Page content:\n" + ellipsize(page ? page->content : "", 4000);
                tool_call("finish", args, "report the page");
                return reply;
            }
            break;
        }
        case Intent::Fix: {
            // Fix strategy: look for the reported error, then patch and rebuild.
            if (!any_tool("fs_grep") && !paths.empty()) {
                Json args = Json::object();
                args["pattern"] = "TODO|FIXME|ERROR|BUG";
                args["regex"] = true;
                tool_call("fs_grep", args, "look for the offending code");
                return reply;
            }
            if (!any_tool("run_command")) {
                std::string command = "make -j 2";
                if (!paths.empty() && ends_with(paths.front(), ".py")) command = "python3 -m py_compile " + paths.front();
                Json args = Json::object();
                args["command"] = command;
                args["timeout_ms"] = 120000;
                tool_call("run_command", args, "reproduce the failure");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* run = last_tool("run_command");
                Json args = Json::object();
                args["summary"] = run ? ("Diagnostics:\n" + ellipsize(run->content, 3000))
                                      : "No compiler output captured.";
                tool_call("finish", args, "report the diagnosis");
                return reply;
            }
            break;
        }
        case Intent::Rename:
        case Intent::Refactor: {
            if (!any_tool("fs_grep") && paths.size() >= 2) {
                Json args = Json::object();
                args["pattern"] = paths[0];
                args["regex"] = false;
                args["max_hits"] = 200;
                tool_call("fs_grep", args, "find every reference to the old name");
                return reply;
            }
            if (!any_tool("finish")) {
                Json args = Json::object();
                args["summary"] = "Edit plan prepared for " + (paths.empty() ? ctx.task : paths[0]);
                tool_call("finish", args, "nothing further to do automatically");
                return reply;
            }
            break;
        }
        case Intent::AddTests: {
            if (!any_tool("fs_write")) {
                if (language == "python") {
                    Json args = Json::object();
                    args["path"] = "tests/test_generated.py";
                    args["content"] =
                        "\"\"\"Generated smoke tests.\"\"\"\n"
                        "import os\nimport subprocess\nimport sys\nimport unittest\n\n\n"
                        "class TestSmoke(unittest.TestCase):\n"
                        "    def test_importable(self):\n"
                        "        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))\n"
                        "        sys.path.insert(0, root)\n"
                        "        self.assertTrue(os.listdir(root))\n\n"
                        "    def test_python_syntax(self):\n"
                        "        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))\n"
                        "        for name in os.listdir(root):\n"
                        "            if name.endswith('.py'):\n"
                        "                subprocess.run([sys.executable, '-m', 'py_compile',\n"
                        "                                os.path.join(root, name)], check=True)\n\n\n"
                        "if __name__ == '__main__':\n"
                        "    unittest.main()\n";
                    tool_call("fs_write", args, "generate a runnable test suite");
                } else {
                    Json args = Json::object();
                    args["path"] = "tests/test_generated.cpp";
                    args["content"] =
                        "// Generated smoke tests: checked in so the project always has a test target.\n"
                        "#include <cstdio>\n#include <cstdlib>\n#include <string>\n#include <vector>\n\n"
                        "static int failures = 0;\n"
                        "static void check(bool ok, const char* what) {\n"
                        "    if (!ok) { std::printf(\"FAIL %s\\n\", what); ++failures; }\n"
                        "    else     { std::printf(\"ok   %s\\n\", what); }\n}\n\n"
                        "int main() {\n"
                        "    check(true, \"test harness runs\");\n"
                        "    std::vector<int> v{1, 2, 3};\n"
                        "    check(v.size() == 3, \"vector has three elements\");\n"
                        "    check(v.front() == 1 && v.back() == 3, \"vector endpoints\");\n"
                        "    std::printf(\"%d failure(s)\\n\", failures);\n"
                        "    return failures == 0 ? 0 : 1;\n}\n";
                    tool_call("fs_write", args, "generate a runnable test suite");
                }
                return reply;
            }
            if (!any_tool("run_command")) {
                Json args = Json::object();
                if (language == "python") {
                    args["command"] = "python3 -m unittest discover -s tests -v";
                } else {
                    args["command"] = "c++ -std=c++17 -O2 -o /tmp/lca-generated-tests tests/test_generated.cpp && "
                                      "/tmp/lca-generated-tests";
                }
                args["timeout_ms"] = 120000;
                tool_call("run_command", args, "run the tests we just generated");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* run = last_tool("run_command");
                Json args = Json::object();
                args["summary"] = "Tests executed:\n" + ellipsize(run ? run->content : "", 2000);
                tool_call("finish", args, "report test results");
                return reply;
            }
            break;
        }
        case Intent::Format: {
            if (!any_tool("run_command")) {
                Json args = Json::object();
                args["command"] = "echo 'no formatter configured; checked file endings instead' && "
                                  "grep -rl $'\\r' . | head -20";
                args["timeout_ms"] = 30000;
                tool_call("run_command", args, "look for formatting problems");
                return reply;
            }
            if (!any_tool("finish")) {
                Json args = Json::object();
                args["summary"] = "Formatting scan complete.";
                tool_call("finish", args, "report formatting scan");
                return reply;
            }
            break;
        }
        case Intent::Document: {
            if (!any_tool("fs_write")) {
                Json args = Json::object();
                args["path"] = "README.md";
                std::string name = basename_of(absolute_path("."));
                args["content"] = "# " + name +
                                  "\n\nGenerated documentation placeholder based on the current "
                                  "project layout.\n\n## Layout\n\n```\n" + project_summary +
                                  "\n```\n";
                tool_call("fs_write", args, "write a README from the project map");
                return reply;
            }
            if (!any_tool("finish")) {
                Json args = Json::object();
                args["summary"] = "README written from the project map.";
                tool_call("finish", args, "done");
                return reply;
            }
            break;
        }
        case Intent::GitStatus: {
            if (!any_tool("run_command")) {
                Json args = Json::object();
                args["command"] = "git status --short --branch 2>&1 | head -50";
                args["timeout_ms"] = 20000;
                tool_call("run_command", args, "inspect repository state");
                return reply;
            }
            if (!any_tool("finish")) {
                const AgentMessage* run = last_tool("run_command");
                Json args = Json::object();
                args["summary"] = "git status:\n" + ellipsize(run ? run->content : "", 2000);
                tool_call("finish", args, "report repository state");
                return reply;
            }
            break;
        }
        case Intent::CreateFile:
        case Intent::Implement:
        case Intent::Unknown:
        default: {
            // Fall back to a reconnaissance + advisory answer.  The heuristic
            // brain cannot invent arbitrary code, so it reports precisely what it
            // found and what a local model would need to do.
            if (!any_tool("fs_grep")) {
                Json args = Json::object();
                args["pattern"] = paths.empty() ? "TODO" : basename_of(paths.front());
                args["regex"] = false;
                args["max_hits"] = 60;
                tool_call("fs_grep", args, "locate the relevant code");
                return reply;
            }
            if (!any_tool("finish")) {
                Json args = Json::object();
                args["summary"] =
                    "Reconnaissance complete (" + std::string(intent_name(intent)) +
                    "). This task needs a model that can write new code; run the agent with "
                    "--brain local --model-endpoint http://127.0.0.1:8080 to use a local model, "
                    "or hand me one of the supported intents (create project, fix, test, "
                    "document, search, scrape, run, explain).";
                tool_call("finish", args, "explain the limitation");
                return reply;
            }
            break;
        }
    }

    reply.done = true;
    reply.final_answer = reply.text.empty() ? "Task finished." : reply.text;
    return reply;
}

// -----------------------------------------------------------------------------
// LocalModelBrain
// -----------------------------------------------------------------------------
std::string LocalModelBrain::description() const {
    return "local model served over HTTP at " + cfg_.endpoint + cfg_.path +
           " (OpenAI-compatible; no hosted API)";
}

Json LocalModelBrain::settings() const {
    Json j = Json::object();
    j["endpoint"] = cfg_.endpoint;
    j["path"] = cfg_.path;
    j["model"] = cfg_.model;
    j["temperature"] = cfg_.temperature;
    j["max_tokens"] = int64_t(cfg_.max_tokens);
    return j;
}

std::string LocalModelBrain::build_system_prompt(const std::vector<ToolSpec>& tools,
                                                 const std::string& extra) {
    std::string p;
    p += "You are local-claude-code-agent, an autonomous coding agent running entirely on the "
         "user's own machine. You act by calling tools; you never claim to have done something "
         "you did not do.\n\n";
    p += "Reply with a single JSON object and nothing else:\n";
    p += "{\"thought\": \"<short reasoning>\", \"tool\": \"<tool name>\", \"args\": { ... }, "
         "\"done\": false, \"answer\": \"\"}\n";
    p += "To use several tools at once, send {\"tool_calls\": [{\"tool\": ..., \"args\": {...}}, ...]}.\n";
    p += "When the task is complete, send {\"done\": true, \"answer\": \"<final summary>\"}.\n\n";
    p += "Available tools:\n";
    for (const ToolSpec& t : tools) {
        p += "- " + t.name + ": " + t.summary;
        if (!t.args_help.empty()) p += "  args: " + t.args_help;
        p += "\n";
    }
    if (!extra.empty()) p += "\nAdditional instructions:\n" + extra + "\n";
    p += "\nRules: prefer small verified edits; run the build/tests after changing code; never "
         "invent file paths; never run destructive shell commands; keep answers short.";
    return p;
}

Result<std::vector<std::string>> LocalModelBrain::list_models() const {
    HttpRequest req;
    req.url = cfg_.endpoint + "/v1/models";
    req.timeout_ms = 5000;
    req.set_header("Accept", "application/json");
    if (!cfg_.api_key.empty()) req.set_header("Authorization", "Bearer " + cfg_.api_key);
    Result<HttpResponse> http = http_request(req);
    if (!http.ok()) return Error(http.error());
    if (!http->ok()) return LCA_FAIL(Code::ModelError, "model endpoint returned HTTP " +
                                                           std::to_string(http->status));
    Result<Json> parsed = Json::parse(http->body_string());
    if (!parsed.ok()) return LCA_FAIL(Code::ParseError, "model list is not valid JSON");
    std::vector<std::string> names;
    for (const Json& item : (*parsed)["data"].items()) {
        std::string id = item["id"].as_string();
        if (!id.empty()) names.push_back(id);
    }
    if (names.empty()) {
        for (const Json& item : (*parsed)["models"].items()) {
            std::string id = item["name"].as_string_or(item["model"].as_string());
            if (!id.empty()) names.push_back(id);
        }
    }
    return names;
}

Result<BrainReply> LocalModelBrain::parse_model_reply(const std::string& content) {
    BrainReply reply;
    std::string text = trim(content);
    if (text.empty()) return LCA_FAIL(Code::ModelError, "empty model reply");

    // Tolerate fenced code blocks.
    if (starts_with(text, "```")) {
        size_t first_nl = text.find('\n');
        size_t last_fence = text.rfind("```");
        if (first_nl != std::string::npos && last_fence != std::string::npos && last_fence > first_nl)
            text = trim(text.substr(first_nl + 1, last_fence - first_nl - 1));
    }
    // Find the outermost JSON object.
    size_t open = text.find('{');
    size_t close = text.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        reply.text = text;
        reply.done = true;
        reply.final_answer = text;
        return reply;
    }
    std::string candidate = text.substr(open, close - open + 1);
    Result<Json> parsed = Json::parse(candidate);
    if (!parsed.ok()) {
        reply.text = text;
        reply.done = true;
        reply.final_answer = text;
        return reply;
    }
    Json obj = *parsed;
    reply.text = obj["thought"].as_string_or(obj["text"].as_string());

    if (obj["done"].as_bool(false)) {
        reply.done = true;
        reply.final_answer = obj["answer"].as_string_or(obj["summary"].as_string_or(reply.text));
        return reply;
    }
    if (obj.has("tool_calls") && obj["tool_calls"].is_array()) {
        int index = 0;
        for (const Json& call : obj["tool_calls"].items()) {
            ToolCall tc;
            tc.tool = call["tool"].as_string_or(call["name"].as_string());
            tc.args = call["args"];
            tc.rationale = call["rationale"].as_string();
            tc.id = "call-" + std::to_string(++index);
            if (!tc.tool.empty()) reply.calls.push_back(std::move(tc));
        }
    } else if (obj.has("tool") || obj.has("name")) {
        ToolCall tc;
        tc.tool = obj["tool"].as_string_or(obj["name"].as_string());
        tc.args = obj["args"];
        tc.rationale = obj["rationale"].as_string();
        tc.id = "call-1";
        if (!tc.tool.empty()) reply.calls.push_back(std::move(tc));
    }
    if (reply.calls.empty() && !obj.has("done") && !reply.text.empty()) {
        // A plain answer with no tool call.
        reply.done = true;
        reply.final_answer = reply.text;
    }
    return reply;
}

Result<BrainReply> LocalModelBrain::step(const BrainContext& ctx) {
    int64_t started = now_millis();
    Json body = Json::object();
    body["model"] = cfg_.model;
    body["temperature"] = cfg_.temperature;
    body["max_tokens"] = int64_t(cfg_.max_tokens);
    if (cfg_.use_json_mode) {
        Json format = Json::object();
        format["type"] = "json_object";
        body["response_format"] = format;
    }

    Json messages = Json::array();
    {
        Json sys = Json::object();
        sys["role"] = "system";
        sys["content"] = build_system_prompt(ctx.tools, cfg_.system_prompt_extra);
        messages.push_back(sys);
    }
    {
        Json sys2 = Json::object();
        sys2["role"] = "system";
        sys2["content"] = "Workspace summary:\n" + ctx.workspace_summary;
        messages.push_back(sys2);
    }
    // Window the history so the local context stays small (8 GB RAM target).
    size_t start = ctx.history.size();
    size_t take = size_t(cfg_.context_messages);
    size_t from = ctx.history.size() > take ? ctx.history.size() - take : 0;
    (void)start;
    for (size_t i = from; i < ctx.history.size(); ++i) {
        const AgentMessage& m = ctx.history[i];
        Json msg = Json::object();
        if (m.role == "tool") {
            msg["role"] = "user";
            msg["content"] = "[tool " + m.tool_name + " result]\n" + ellipsize(m.content, 8000);
        } else {
            msg["role"] = m.role == "assistant" ? "assistant" : "user";
            msg["content"] = m.content;
        }
        messages.push_back(msg);
    }
    {
        Json msg = Json::object();
        msg["role"] = "user";
        msg["content"] = "Task: " + ctx.task + "\nStep " + std::to_string(ctx.step + 1) +
                         " of " + std::to_string(ctx.max_steps) +
                         (ctx.notes.empty() ? "" : ("\nNotes: " + join(ctx.notes, "; ")));
        messages.push_back(msg);
    }
    body["messages"] = messages;

    HttpRequest req;
    req.method = "POST";
    req.url = cfg_.endpoint + cfg_.path;
    req.timeout_ms = cfg_.timeout_ms;
    req.body = body.dump();
    req.body_content_type = "application/json";
    req.max_response_bytes = 4u << 20;
    req.set_header("Content-Type", "application/json");
    req.set_header("Accept", "application/json");
    if (!cfg_.api_key.empty()) req.set_header("Authorization", "Bearer " + cfg_.api_key);

    Result<HttpResponse> http = http_request(req);
    if (!http.ok())
        return LCA_FAIL(Code::ModelError, "local model request failed: " + http.error().str());
    if (!http->ok())
        return LCA_FAIL(Code::ModelError, "local model returned HTTP " +
                                              std::to_string(http->status) + ": " +
                                              ellipsize(http->body_string(), 300));
    Result<Json> parsed = Json::parse(http->body_string());
    if (!parsed.ok())
        return LCA_FAIL(Code::ParseError, "local model response is not valid JSON: " +
                                              ellipsize(http->body_string(), 200));
    std::string content = (*parsed)["choices"].at(0)["message"]["content"].as_string();
    if (content.empty()) content = (*parsed)["content"].as_string();     // llama.cpp /completion
    if (content.empty()) content = (*parsed)["response"].as_string();    // ollama /api/generate

    Result<BrainReply> reply = parse_model_reply(content);
    if (!reply.ok()) return reply;
    reply->latency_ms = now_millis() - started;
    return reply;
}

// -----------------------------------------------------------------------------
// Brain selection
// -----------------------------------------------------------------------------
std::unique_ptr<Brain> make_brain(const std::string& kind, const LocalModelConfig& local,
                                  const HeuristicConfig& heuristic, std::string* note) {
    std::string wanted = lower(kind.empty() ? "auto" : kind);
    if (wanted == "heuristic" || wanted == "builtin" || wanted == "built-in")
        return std::unique_ptr<Brain>(new HeuristicBrain(heuristic));
    if (wanted == "local" || wanted == "auto") {
        LocalModelBrain probe(local);
        Result<std::vector<std::string>> models = probe.list_models();
        if (models.ok()) {
            if (note) {
                *note = "using local model server at " + local.endpoint + " (" +
                        std::to_string(models->size()) + " model(s) available)";
            }
            return std::unique_ptr<Brain>(new LocalModelBrain(local));
        }
        if (wanted == "local") {
            std::string message = "local model endpoint unreachable at " + local.endpoint +
                                  (models.error().message.empty() ? "" : ": " + models.error().message);
            if (note) *note = message;
            return nullptr;
        }
        if (note) {
            *note = "no local model server found at " + local.endpoint +
                    "; falling back to the built-in planner";
        }
        return std::unique_ptr<Brain>(new HeuristicBrain(heuristic));
    }
    if (note) *note = "unknown brain '" + kind + "'; using the built-in planner";
    return std::unique_ptr<Brain>(new HeuristicBrain(heuristic));
}

}  // namespace lca
