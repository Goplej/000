// =============================================================================
//  lca/proc.cpp  --  POSIX process sandbox implementation
// =============================================================================
#include "lca/proc.h"
#include "lca/buf.h"
#include "lca/mem.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace lca {
namespace {

constexpr const char* kScope = "proc";

std::string errno_text() { return std::strerror(errno); }

// -----------------------------------------------------------------------------
// Environment construction
// -----------------------------------------------------------------------------
std::vector<std::string> build_environment(const ExecRequest& req, const std::string& cwd,
                                           const std::string& extra_path) {
    std::vector<std::string> env;
    std::map<std::string, std::string> merged;
    for (char** e = environ; e && *e; ++e) {
        std::string kv(*e);
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        merged[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const std::string& key : req.env_remove) merged.erase(key);
    for (const auto& kv : req.env) merged[kv.first] = kv.second;
    // Sensible defaults for build tooling.
    merged["PATH"] = merged["PATH"] + extra_path;
    merged["TERM"] = merged.count("TERM") ? merged["TERM"] : "dumb";
    merged["LC_ALL"] = "C";
    merged["PWD"] = cwd;
    env.reserve(merged.size());
    for (const auto& kv : merged) env.push_back(kv.first + "=" + kv.second);
    return env;
}

// -----------------------------------------------------------------------------
// Child-side setup
// -----------------------------------------------------------------------------
void child_setup(const ExecRequest& req, const std::string& cwd, uint64_t cpu_seconds) {
    // New process group so we can signal the whole tree.
    ::setpgid(0, 0);

    struct rlimit rl {};
    if (req.max_memory_bytes) {
        rl.rlim_cur = rlim_t(req.max_memory_bytes);
        rl.rlim_max = rlim_t(req.max_memory_bytes);
        ::setrlimit(RLIMIT_AS, &rl);
    }
    if (req.max_file_bytes) {
        rl.rlim_cur = rlim_t(req.max_file_bytes);
        rl.rlim_max = rlim_t(req.max_file_bytes);
        ::setrlimit(RLIMIT_FSIZE, &rl);
    }
    if (cpu_seconds) {
        rl.rlim_cur = rlim_t(cpu_seconds);
        rl.rlim_max = rlim_t(cpu_seconds + 5);
        ::setrlimit(RLIMIT_CPU, &rl);
    }
    if (req.max_processes) {
        struct rlimit cur {};
        if (::getrlimit(RLIMIT_NPROC, &cur) == 0) {
            rlim_t want = rlim_t(std::min<uint64_t>(req.max_processes, cur.rlim_max));
            if (cur.rlim_cur == RLIM_INFINITY || cur.rlim_cur > want) {
                cur.rlim_cur = want;
                ::setrlimit(RLIMIT_NPROC, &cur);
            }
        }
    }
    // No core dumps from a sandboxed child.
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &rl);

    if (!cwd.empty()) {
        if (::chdir(cwd.c_str()) != 0) _exit(126);
    }
}

// -----------------------------------------------------------------------------
// Deadline helpers
// -----------------------------------------------------------------------------
void kill_tree(pid_t pid, int sig) {
    if (pid <= 0) return;
    // Negative pid signals the whole process group (child called setpgid(0,0)).
    ::kill(-pid, sig);
    ::kill(pid, sig);
}

}  // namespace

// -----------------------------------------------------------------------------
// ExecResult formatting
// -----------------------------------------------------------------------------
std::string ExecResult::summary() const {
    std::string s;
    if (spawn_failed) s += "failed to start";
    else if (timed_out) s += "timed out";
    else if (signal) s += "killed by signal " + std::to_string(signal);
    else s += "exit " + std::to_string(exit_code);
    s += " in " + human_duration(elapsed_ms);
    if (peak_rss_bytes) s += ", peak rss " + human_bytes(peak_rss_bytes);
    return s;
}

Json ExecResult::to_json() const {
    Json j = Json::object();
    j["command"] = command_line;
    j["exit_code"] = int64_t(exit_code);
    j["signal"] = int64_t(signal);
    j["timed_out"] = timed_out;
    j["spawn_failed"] = spawn_failed;
    j["elapsed_ms"] = int64_t(elapsed_ms);
    j["peak_rss_bytes"] = int64_t(peak_rss_bytes);
    j["stdout"] = out;
    j["stderr"] = err;
    j["stdout_truncated"] = out_truncated;
    j["stderr_truncated"] = err_truncated;
    return j;
}

// -----------------------------------------------------------------------------
// Policies
// -----------------------------------------------------------------------------
SandboxPolicy SandboxPolicy::permissive() {
    SandboxPolicy p;
    p.default_deny = false;
    p.confine_to_workspace = true;
    p.allow_network_commands = true;
    p.denylist = {
        "rm -rf /", "rm -rf /*", ":(){", "mkfs", "dd if=/dev/zero of=/dev/",
        "shutdown", "reboot", "halt", "poweroff", "> /dev/sda", "chmod -R 777 /",
    };
    return p;
}

SandboxPolicy SandboxPolicy::developer() {
    SandboxPolicy p = permissive();
    p.rules = {
        {"sudo", CommandVerdict::Deny, "privilege escalation is never needed"},
        {"curl * | sh", CommandVerdict::Deny, "piping a download into a shell"},
        {"chown -R /", CommandVerdict::Deny, "recursive ownership change at the root"},
    };
    return p;
}

SandboxPolicy SandboxPolicy::strict() {
    SandboxPolicy p;
    p.default_deny = true;
    p.confine_to_workspace = true;
    p.allow_network_commands = false;
    p.allowlist = {
        "cc", "gcc", "g++", "clang", "clang++", "make", "cmake", "ninja", "ld", "ar",
        "python3", "python", "pytest", "node", "npm", "npx", "tsc", "go", "cargo",
        "rustc", "java", "javac", "mvn", "gradle", "dotnet", "ruby", "rake", "php",
        "git", "diff", "patch", "file", "wc", "head", "tail", "cat", "ls", "find",
        "grep", "sed", "awk", "sort", "uniq", "cut", "tr", "tee", "mkdir", "touch",
        "cp", "mv", "rm", "chmod", "tar", "zip", "unzip", "true", "false", "echo",
    };
    p.rules = {
        {"sudo", CommandVerdict::Deny, "privilege escalation"},
        {"rm -rf /", CommandVerdict::Deny, "destructive"},
    };
    return p;
}

CommandVerdict SandboxPolicy::evaluate(const std::string& command, std::string* reason) const {
    std::string cmd = trim(command);
    if (cmd.empty()) {
        if (reason) *reason = "empty command";
        return CommandVerdict::Deny;
    }
    for (const std::string& bad : denylist) {
        if (!bad.empty() && contains(cmd, bad)) {
            if (reason) *reason = "matches denylist entry '" + bad + "'";
            return CommandVerdict::Deny;
        }
    }
    for (const PolicyRule& rule : rules) {
        if (!rule.pattern.empty() && glob_match(rule.pattern, cmd)) {
            if (reason && !rule.note.empty()) *reason = rule.note;
            return rule.verdict;
        }
    }
    if (!allowlist.empty()) {
        std::vector<std::string> words = split_command_line(cmd);
        bool ok = false;
        for (const std::string& w : words) {
            std::string base = basename_of(w);
            for (const std::string& allowed : allowlist) {
                if (base == allowed) { ok = true; break; }
            }
            if (ok) break;
        }
        if (!ok && default_deny) {
            if (reason) *reason = "command is not in the sandbox allowlist";
            return CommandVerdict::Deny;
        }
    }
    if (default_deny && allowlist.empty()) {
        if (reason) *reason = "default deny policy with an empty allowlist";
        return CommandVerdict::Deny;
    }
    return CommandVerdict::Allow;
}

// -----------------------------------------------------------------------------
// ProcessSandbox::run
// -----------------------------------------------------------------------------
Result<ExecResult> ProcessSandbox::run(const ExecRequest& req) {
    std::string command_line = req.command;
    if (command_line.empty()) command_line = join(req.argv, " ");
    AuditEntry entry;
    entry.when_ms = wall_millis();
    entry.command = command_line;

    std::string reason;
    CommandVerdict verdict = policy_.evaluate(command_line, &reason);
    entry.verdict = verdict == CommandVerdict::Allow ? "allow"
                  : verdict == CommandVerdict::Deny  ? "deny"
                                                     : "needs-approval";
    entry.reason = reason;
    if (verdict != CommandVerdict::Allow) {
        audit_.push_back(entry);
        return LCA_FAIL(verdict == CommandVerdict::Deny ? Code::SandboxViolation : Code::PermissionDenied,
                        "sandbox refused command '" + ellipsize(command_line, 120) + "'" +
                            (reason.empty() ? "" : ": " + reason));
    }
    if (req.dry_run) {
        ExecResult res;
        res.command_line = command_line;
        res.exit_code = 0;
        audit_.push_back(entry);
        return res;
    }
    Result<ExecResult> result = run_unchecked(req);
    entry.exit_code = result.ok() ? result->exit_code : -1;
    audit_.push_back(entry);
    return result;
}

// -----------------------------------------------------------------------------
// ProcessSandbox::run_unchecked -- fork/exec with pipes, deadline and rlimits
// -----------------------------------------------------------------------------
Result<ExecResult> ProcessSandbox::run_unchecked(const ExecRequest& req) {
    ExecResult result;
    result.command_line = req.command.empty() ? join(req.argv, " ") : req.command;

    if (!req.command.empty() && req.argv.empty() && trim(req.command).empty())
        return LCA_FAIL(Code::InvalidArgument, "empty command");
    if (req.argv.empty() && req.command.empty())
        return LCA_FAIL(Code::InvalidArgument, "no command given");

    std::string cwd = req.cwd;
    if (cwd.empty()) cwd = workspace_root_;
    if (cwd.empty()) cwd = absolute_path(".");
    if (!is_directory(cwd)) return LCA_FAIL(Code::NotFound, "working directory does not exist: " + cwd);
    if (policy_.confine_to_workspace && !workspace_root_.empty() &&
        !starts_with(absolute_path(cwd), workspace_root_)) {
        return LCA_FAIL(Code::SandboxViolation, "working directory is outside the workspace: " + cwd);
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int in_pipe[2]  = {-1, -1};
    if (::pipe(out_pipe) != 0) return LCA_FAIL(Code::IoError, "pipe(): " + errno_text());
    bool merged = req.merge_stderr;
    if (!merged) {
        if (::pipe(err_pipe) != 0) {
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            return LCA_FAIL(Code::IoError, "pipe(): " + errno_text());
        }
    }
    bool have_stdin = !req.stdin_data.empty() || !req.stdin_closed;
    if (have_stdin) {
        if (::pipe(in_pipe) != 0) {
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            if (!merged) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
            return LCA_FAIL(Code::IoError, "pipe(): " + errno_text());
        }
    }

    uint64_t cpu_seconds = req.max_cpu_seconds;
    if (cpu_seconds == 0 && req.timeout_ms > 0)
        cpu_seconds = uint64_t(req.timeout_ms / 1000) + 5;

    std::string extra_path = "";
    std::vector<std::string> env_strings = build_environment(req, cwd, extra_path);

    int64_t started = now_millis();
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (!merged) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
        if (have_stdin) { ::close(in_pipe[0]); ::close(in_pipe[1]); }
        return LCA_FAIL(Code::IoError, "fork(): " + errno_text());
    }

    if (pid == 0) {
        // ---- child ------------------------------------------------------
        child_setup(req, cwd, cpu_seconds);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        if (merged) ::dup2(out_pipe[1], STDERR_FILENO);
        else        ::dup2(err_pipe[1], STDERR_FILENO);
        if (have_stdin) ::dup2(in_pipe[0], STDIN_FILENO);
        else            ::dup2(::open("/dev/null", O_RDONLY), STDIN_FILENO);

        ::close(out_pipe[0]); ::close(out_pipe[1]);
        if (!merged) { ::close(err_pipe[0]); ::close(err_pipe[1]); }
        if (have_stdin) { ::close(in_pipe[0]); ::close(in_pipe[1]); }

        std::vector<char*> argv;
        std::vector<char*> envp;
        std::vector<std::string> owned;
        if (!req.argv.empty()) {
            for (const std::string& a : req.argv) owned.push_back(a);
        } else {
            owned.push_back("/bin/sh");
            owned.push_back("-c");
            owned.push_back(req.command);
        }
        for (std::string& s : owned) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        for (std::string& s : env_strings) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    // ---- parent ---------------------------------------------------------
    ::close(out_pipe[1]);
    if (!merged) ::close(err_pipe[1]);
    if (have_stdin) ::close(in_pipe[0]);

    int out_fd = out_pipe[0];
    int err_fd = merged ? -1 : err_pipe[0];
    int in_fd  = have_stdin ? in_pipe[1] : -1;
    auto set_nonblock = [](int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    };
    set_nonblock(out_fd);
    if (err_fd >= 0) set_nonblock(err_fd);
    if (in_fd >= 0) set_nonblock(in_fd);

    struct rusage child_rusage {};
    int64_t deadline = req.timeout_ms > 0 ? started + req.timeout_ms : 0;
    bool sent_term = false;
    int64_t killed_at = 0;
    size_t stdin_off = 0;
    bool stdin_open = have_stdin;

    while (out_fd >= 0 || err_fd >= 0 || stdin_open) {
        struct pollfd fds[3];
        int nfds = 0;
        int out_idx = -1, err_idx = -1, in_idx = -1;
        if (out_fd >= 0) { fds[nfds] = {out_fd, POLLIN, 0}; out_idx = nfds++; }
        if (err_fd >= 0) { fds[nfds] = {err_fd, POLLIN, 0}; err_idx = nfds++; }
        if (stdin_open)  { fds[nfds] = {in_fd, POLLOUT, 0};  in_idx = nfds++; }

        int wait_ms = 200;
        if (deadline) {
            int64_t left = deadline - now_millis();
            if (left <= 0 && !sent_term) {
                kill_tree(pid, SIGTERM);
                sent_term = true;
                killed_at = now_millis();
                left = 500;
            } else if (sent_term && now_millis() - killed_at > 500) {
                kill_tree(pid, SIGKILL);
            }
            if (left > 0 && left < wait_ms) wait_ms = int(left);
        }
        if (wait_ms < 0) wait_ms = 0;

        int ready = ::poll(fds, nfds, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            // Nothing ready: check whether the child exited.
            int status = 0;
            struct rusage ru_now {};
            pid_t r = ::wait4(pid, &status, WNOHANG, &ru_now);
            if (r == pid) {
                child_rusage = ru_now;
                // Drain whatever is left, then finish.
                if (out_idx >= 0) {
                    char buf[8192];
                    for (;;) {
                        ssize_t n = ::read(out_fd, buf, sizeof(buf));
                        if (n <= 0) break;
                        if (result.out.size() < req.max_output_bytes) {
                            result.out.append(buf, size_t(std::min<ssize_t>(n, ssize_t(req.max_output_bytes - result.out.size()))));
                        } else {
                            result.out_truncated = true;
                        }
                    }
                    ::close(out_fd); out_fd = -1;
                }
                if (err_fd >= 0) {
                    char buf[8192];
                    for (;;) {
                        ssize_t n = ::read(err_fd, buf, sizeof(buf));
                        if (n <= 0) break;
                        if (result.err.size() < req.max_output_bytes) {
                            result.err.append(buf, size_t(std::min<ssize_t>(n, ssize_t(req.max_output_bytes - result.err.size()))));
                        } else {
                            result.err_truncated = true;
                        }
                    }
                    ::close(err_fd); err_fd = -1;
                }
                // Record exit status.
                if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) { result.signal = WTERMSIG(status); result.exit_code = 128 + result.signal; }
                struct rusage ru = child_rusage;
                result.peak_rss_bytes = uint64_t(ru.ru_maxrss) * 1024ull;
                result.user_us = int64_t(ru.ru_utime.tv_sec) * 1000000 + ru.ru_utime.tv_usec;
                result.sys_us  = int64_t(ru.ru_stime.tv_sec) * 1000000 + ru.ru_stime.tv_usec;
                result.elapsed_ms = now_millis() - started;
                result.timed_out = sent_term;
                if (stdin_open && in_fd >= 0) { ::close(in_fd); in_fd = -1; stdin_open = false; }
                return result;
            }
            continue;
        }

        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[16384];
            for (;;) {
                ssize_t n = ::read(out_fd, buf, sizeof(buf));
                if (n > 0) {
                    size_t room = req.max_output_bytes > result.out.size()
                                      ? req.max_output_bytes - result.out.size() : 0;
                    if (room) result.out.append(buf, size_t(std::min<ssize_t>(n, ssize_t(room))));
                    if (size_t(n) > room) result.out_truncated = true;
                    continue;
                }
                if (n == 0) {
                    ::close(out_fd); out_fd = -1;
                }
                break;   // EAGAIN
            }
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[16384];
            for (;;) {
                ssize_t n = ::read(err_fd, buf, sizeof(buf));
                if (n > 0) {
                    size_t room = req.max_output_bytes > result.err.size()
                                      ? req.max_output_bytes - result.err.size() : 0;
                    if (room) result.err.append(buf, size_t(std::min<ssize_t>(n, ssize_t(room))));
                    if (size_t(n) > room) result.err_truncated = true;
                    continue;
                }
                if (n == 0) {
                    ::close(err_fd); err_fd = -1;
                }
                break;
            }
        }
        if (in_idx >= 0 && (fds[in_idx].revents & (POLLOUT | POLLHUP | POLLERR))) {
            ssize_t n = ::write(in_fd, req.stdin_data.data() + stdin_off,
                                req.stdin_data.size() - stdin_off);
            if (n > 0) {
                stdin_off += size_t(n);
                if (stdin_off >= req.stdin_data.size()) {
                    ::close(in_fd);
                    in_fd = -1;
                    stdin_open = false;
                }
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                ::close(in_fd);
                in_fd = -1;
                stdin_open = false;
            } else if (n == 0) {
                ::close(in_fd);
                in_fd = -1;
                stdin_open = false;
            }
        }
    }

    // Wait for the child (bounded).
    int status = 0;
    int64_t wait_deadline = now_millis() + 5000;
    for (;;) {
        struct rusage ru_now {};
        pid_t r = ::wait4(pid, &status, WNOHANG, &ru_now);
        if (r == pid) { child_rusage = ru_now; break; }
        if (r < 0 && errno != EINTR) break;
        if (now_millis() > wait_deadline) {
            kill_tree(pid, SIGKILL);
            struct rusage ru_kill {};
            ::wait4(pid, &status, 0, &ru_kill);
            child_rusage = ru_kill;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (out_fd >= 0) ::close(out_fd);
    if (err_fd >= 0) ::close(err_fd);
    if (in_fd >= 0) ::close(in_fd);

    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.exit_code = 128 + result.signal;
    }
    result.peak_rss_bytes = uint64_t(child_rusage.ru_maxrss) * 1024ull;
    result.user_us = int64_t(child_rusage.ru_utime.tv_sec) * 1000000 + child_rusage.ru_utime.tv_usec;
    result.sys_us  = int64_t(child_rusage.ru_stime.tv_sec) * 1000000 + child_rusage.ru_stime.tv_usec;
    result.elapsed_ms = now_millis() - started;
    result.timed_out = sent_term;
    if (result.signal == SIGKILL && sent_term) result.timed_out = true;
    return result;
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
Result<ExecResult> quick_exec(const std::string& command, const std::string& cwd,
                              int timeout_ms, size_t max_output) {
    ProcessSandbox sandbox(SandboxPolicy::developer());
    sandbox.set_workspace_root(cwd.empty() ? absolute_path(".") : absolute_path(cwd));
    ExecRequest req;
    req.command = command;
    req.cwd = cwd;
    req.timeout_ms = timeout_ms;
    req.max_output_bytes = max_output;
    req.max_memory_bytes = std::max<uint64_t>(256ull * 1024 * 1024,
                                              MemoryGuard::instance().budget_bytes() / 4);
    return sandbox.run_unchecked(req);
}

bool command_exists(const std::string& program) {
    if (program.empty()) return false;
    if (program.find('/') != std::string::npos) {
        return ::access(program.c_str(), X_OK) == 0;
    }
    const char* path = ::getenv("PATH");
    if (!path) return false;
    for (const std::string& dir : split(path, ':')) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/" + program;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::string capture_command(const std::string& command, int timeout_ms) {
    ProcessSandbox sandbox(SandboxPolicy::permissive());
    ExecRequest req;
    req.command = command;
    req.timeout_ms = timeout_ms;
    req.max_output_bytes = 64 * 1024;
    Result<ExecResult> res = sandbox.run_unchecked(req);
    if (!res.ok()) return {};
    std::string out = trim(res->out);
    if (out.empty()) out = trim(res->err);
    return out;
}

std::vector<std::string> split_command_line(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_single = false, in_double = false, escape = false;
    for (char c : line) {
        if (escape) { cur.push_back(c); escape = false; continue; }
        if (c == '\\' && !in_single) { escape = true; continue; }
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single)  { in_double = !in_double; continue; }
        if (!in_single && !in_double && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string shell_quote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

}  // namespace lca
