// =============================================================================
//  lca/fs_engine.cpp  --  sandboxed file engine implementation
// =============================================================================
#include "lca/fs_engine.h"
#include "lca/buf.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace lca {
namespace {

constexpr const char* kScope = "fs";

std::string errno_text() { return std::strerror(errno); }

// -----------------------------------------------------------------------------
// POSIX wrappers
// -----------------------------------------------------------------------------
bool stat_path(const std::string& path, struct stat* st) {
    return ::lstat(path.c_str(), st) == 0;
}

bool is_dir_mode(mode_t mode) { return S_ISDIR(mode) != 0; }

// Follows each component manually so that the result is independent of the
// caller's current directory and of symlinks (unless allowed).
Result<std::string> canonicalise(const std::string& path, bool follow_symlinks) {
    if (path.empty()) return LCA_FAIL(Code::InvalidArgument, "empty path");
    std::string abs = path[0] == '/' ? path : (absolute_path(path));
    std::vector<std::string> parts;
    for (const std::string& piece : split(abs, '/')) {
        if (piece.empty() || piece == ".") continue;
        if (piece == "..") {
            if (parts.empty()) return LCA_FAIL(Code::InvalidArgument, "path escapes the filesystem root");
            parts.pop_back();
            continue;
        }
        parts.push_back(piece);
    }
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    if (out.empty()) out = "/";

    // Resolve symlinks component by component (up to a sane depth).
    if (follow_symlinks) {
        int guard = 0;
        std::string built;
        for (size_t i = 0; i < parts.size(); ++i) {
            built += "/" + parts[i];
            struct stat st {};
            if (::lstat(built.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
                char target[4096];
                ssize_t n = ::readlink(built.c_str(), target, sizeof(target) - 1);
                if (n <= 0) break;
                target[n] = '\0';
                std::string rest;
                for (size_t j = i + 1; j < parts.size(); ++j) rest += "/" + parts[j];
                std::string next = std::string(target) + rest;
                if (++guard > 32) return LCA_FAIL(Code::InvalidArgument, "too many symlink hops");
                return canonicalise(next[0] == '/' ? next : (directory_of(built) + "/" + next), true);
            }
        }
    }
    return out;
}

bool path_is_inside(const std::string& root, const std::string& candidate) {
    if (root.empty()) return false;
    if (candidate == root) return true;
    if (candidate.size() <= root.size()) return false;
    if (candidate.compare(0, root.size(), root) != 0) return false;
    return candidate[root.size()] == '/';
}

bool dir_exists(const std::string& path) {
    struct stat st {};
    return stat_path(path, &st) && is_dir_mode(st.st_mode);
}

Error make_dirs(const std::string& path) {
    if (path.empty() || dir_exists(path)) return {};
    std::vector<std::string> parts = split(path, '/');
    std::string cur;
    for (const std::string& p : parts) {
        if (p.empty()) { cur = ""; continue; }
        cur += "/" + p;
        if (dir_exists(cur)) continue;
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
            return LCA_FAIL(Code::IoError, "mkdir(" + cur + "): " + errno_text());
    }
    return {};
}

// Atomic write: write to a sibling temp file, fsync, rename over the target.
Error atomic_write(const std::string& path, std::string_view content) {
    Error dir_err = make_dirs(directory_of(path));
    if (!dir_err.ok()) return dir_err;
    std::string tmp = path + ".lca-tmp-" + std::to_string(int64_t(::getpid())) + "-" +
                      std::to_string(wall_millis());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return LCA_FAIL(Code::IoError, "open(" + tmp + "): " + errno_text());
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return LCA_FAIL(Code::IoError, "write(" + tmp + "): " + errno_text());
        }
        off += size_t(n);
    }
    if (::fsync(fd) != 0) { /* best effort; not fatal on all filesystems */ }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        std::string why = errno_text();
        ::unlink(tmp.c_str());
        return LCA_FAIL(Code::IoError, "rename(" + tmp + " -> " + path + "): " + why);
    }
    return {};
}

Result<std::string> read_whole_file(const std::string& path, size_t max_bytes, bool* truncated) {
    if (truncated) *truncated = false;
    struct stat st {};
    if (!stat_path(path, &st)) return LCA_FAIL(Code::NotFound, "no such file: " + path);
    if (is_dir_mode(st.st_mode)) return LCA_FAIL(Code::InvalidArgument, "path is a directory: " + path);
    size_t want = size_t(st.st_size);
    bool cut = false;
    if (max_bytes && want > max_bytes) { want = max_bytes; cut = true; }
    if (truncated) *truncated = cut;

    uint64_t budget = clamp_val<uint64_t>(want + 4096, 4096, bulk_operation_budget(0.25));
    MemoryReservation res(budget, "fs.read");
    if (!res.ok()) return LCA_FAIL(Code::ResourceExhausted, "not enough memory budget to read " + path);
    (void)budget;

    std::string out;
    out.resize(want);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return LCA_FAIL(Code::IoError, "open(" + path + "): " + errno_text());
    size_t off = 0;
    while (off < want) {
        ssize_t n = ::read(fd, &out[off], want - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return LCA_FAIL(Code::IoError, "read(" + path + "): " + errno_text());
        }
        if (n == 0) break;
        off += size_t(n);
    }
    ::close(fd);
    out.resize(off);
    return out;
}

bool looks_binary(std::string_view data) {
    size_t checked = std::min<size_t>(data.size(), 8192);
    size_t nul = 0;
    for (size_t i = 0; i < checked; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0) ++nul;
    }
    return checked > 0 && (nul * 100 / checked) > 2;
}


// -----------------------------------------------------------------------------
// Recursive walk
// -----------------------------------------------------------------------------
struct WalkState {
    std::vector<FileInfo>* out{nullptr};
    size_t                 limit{0};
    bool                   truncated{false};
    bool                   recursive{false};
    const Workspace*       ws{nullptr};
    std::set<std::string>  visited_dirs;
};

bool is_ignored_file(const Workspace& ws, const std::string& name) {
    for (const std::string& suffix : ws.cfg.ignore_suffixes) {
        if (!suffix.empty() && name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
        // Backups are "<file>.lca-backup-<millis>": compare against the stem too.
        size_t at = name.find(suffix);
        if (!suffix.empty() && at != std::string::npos) return true;
    }
    return false;
}

bool is_ignored(const Workspace& ws, const std::string& name) {
    for (const std::string& ig : ws.cfg.ignore_dirs) {
        if (name == ig) return true;
    }
    return is_ignored_file(ws, name);
}

void walk_dir(WalkState& st, const std::string& abs_dir, int depth);

void consider_entry(WalkState& st, const std::string& abs_path, const std::string& name, int depth) {
    struct stat lst {};
    if (!stat_path(abs_path, &lst)) return;
    FileInfo info;
    info.path = abs_path;
    info.rel = fs_relative(*st.ws, abs_path);
    info.name = name;
    info.is_symlink = S_ISLNK(lst.st_mode) != 0;
    if (info.is_symlink && !st.ws->cfg.follow_symlinks) {
        // Report the link itself but do not descend into it.
        info.is_dir = false;
        info.size = uint64_t(lst.st_size);
        info.mtime_ms = int64_t(lst.st_mtime) * 1000;
        if (st.out) st.out->push_back(info);
        return;
    }
    info.is_dir = is_dir_mode(lst.st_mode);
    info.size = info.is_dir ? 0 : uint64_t(lst.st_size);
    info.mtime_ms = int64_t(lst.st_mtime) * 1000;
    info.is_text = !info.is_dir && !language_for_path(abs_path).empty();
    info.extension();
    if (st.out) {
        st.out->push_back(info);
        if (st.limit && st.out->size() >= st.limit) st.truncated = true;
    }
    if (info.is_dir && depth < 64) walk_dir(st, abs_path, depth + 1);
}

void walk_dir(WalkState& st, const std::string& abs_dir, int depth) {
    if (st.limit && st.out && st.out->size() >= st.limit) { st.truncated = true; return; }
    if (st.visited_dirs.count(abs_dir)) return;          // symlink loop guard
    if (st.visited_dirs.size() > 4096) { st.truncated = true; return; }
    st.visited_dirs.insert(abs_dir);

    DIR* dir = ::opendir(abs_dir.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (struct dirent* ent = ::readdir(dir)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        if (is_ignored(*st.ws, name)) continue;
        std::string child = abs_dir + "/" + name;
        struct stat lst {};
        if (!stat_path(child, &lst)) continue;
        bool dir = is_dir_mode(lst.st_mode) && !(S_ISLNK(lst.st_mode) && !st.ws->cfg.follow_symlinks);
        if (dir && !st.recursive) {
            FileInfo info;
            info.path = child;
            info.rel = fs_relative(*st.ws, child);
            info.name = name;
            info.is_dir = true;
            info.mtime_ms = int64_t(lst.st_mtime) * 1000;
            if (st.out) st.out->push_back(info);
            continue;
        }
        consider_entry(st, child, name, depth);
        if (st.limit && st.out && st.out->size() >= st.limit) { st.truncated = true; return; }
    }
}

// -----------------------------------------------------------------------------
// Diff (bounded LCS)
// -----------------------------------------------------------------------------
struct DiffOp {
    char        kind;   // ' ', '-', '+'
    size_t      a_line; // 1-based in `a` (0 when added)
    size_t      b_line; // 1-based in `b` (0 when removed)
    std::string text;
};

std::vector<std::string> to_lines(std::string_view text, bool* had_final_newline) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    if (had_final_newline) *had_final_newline = !(!text.empty() && text.back() == '\n');
    if (!cur.empty()) lines.push_back(cur);
    // Guard against pathological inputs: cap the number of lines we diff.
    constexpr size_t kMaxDiffLines = 20000;
    if (lines.size() > kMaxDiffLines) lines.resize(kMaxDiffLines);
    return lines;
}

// Classic LCS dynamic programming, bounded so memory stays small.  Files above
// the bound fall back to a whole-file "replace" hunk, which is still correct.
std::vector<DiffOp> diff_lines(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<DiffOp> ops;
    const size_t n = a.size(), m = b.size();
    const size_t kLimit = 2000;   // 2000x2000 uint32 table = 16 MB worst case
    if (n > kLimit || m > kLimit) {
        for (size_t i = 0; i < n; ++i) ops.push_back({'-', i + 1, 0, a[i]});
        for (size_t j = 0; j < m; ++j) ops.push_back({'+', 0, j + 1, b[j]});
        return ops;
    }
    uint64_t budget = uint64_t(n + 1) * uint64_t(m + 1) * sizeof(uint32_t) + 65536;
    MemoryReservation res(budget, "fs.diff");
    if (!res.ok()) {
        for (size_t i = 0; i < n; ++i) ops.push_back({'-', i + 1, 0, a[i]});
        for (size_t j = 0; j < m; ++j) ops.push_back({'+', 0, j + 1, b[j]});
        return ops;
    }
    std::vector<uint32_t> table((n + 1) * (m + 1), 0);
    auto cell = [&](size_t i, size_t j) -> uint32_t& { return table[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            if (a[i] == b[j]) cell(i, j) = uint32_t(cell(i + 1, j + 1) + 1);
            else              cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
        }
    }
    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) { ops.push_back({' ', i + 1, j + 1, a[i]}); ++i; ++j; }
        else if (cell(i + 1, j) >= cell(i, j + 1)) { ops.push_back({'-', i + 1, 0, a[i]}); ++i; }
        else { ops.push_back({'+', 0, j + 1, b[j]}); ++j; }
    }
    while (i < n) { ops.push_back({'-', i + 1, 0, a[i]}); ++i; }
    while (j < m) { ops.push_back({'+', 0, j + 1, b[j]}); ++j; }
    return ops;
}

// -----------------------------------------------------------------------------
// Patch parsing helpers
// -----------------------------------------------------------------------------
struct BlockEdit {
    enum class Kind { Update, Add, Delete } kind{Kind::Update};
    std::string path;
    std::vector<DiffOp> lines;      // for Update
    std::string content;            // for Add
    size_t context_lines{3};
};

std::vector<BlockEdit> parse_block_patch(std::string_view patch) {
    std::vector<BlockEdit> edits;
    auto lines = split_lines(patch);
    BlockEdit* current = nullptr;
    for (const std::string& raw : lines) {
        std::string line = raw;
        if (starts_with(line, "*** Begin Patch") || starts_with(line, "*** End Patch")) {
            current = nullptr;
            continue;
        }
        if (starts_with(line, "*** Update File:")) {
            BlockEdit e;
            e.kind = BlockEdit::Kind::Update;
            e.path = trim(line.substr(std::strlen("*** Update File:")));
            edits.push_back(e);
            current = &edits.back();
            continue;
        }
        if (starts_with(line, "*** Add File:")) {
            BlockEdit e;
            e.kind = BlockEdit::Kind::Add;
            e.path = trim(line.substr(std::strlen("*** Add File:")));
            edits.push_back(e);
            current = &edits.back();
            continue;
        }
        if (starts_with(line, "*** Delete File:")) {
            BlockEdit e;
            e.kind = BlockEdit::Kind::Delete;
            e.path = trim(line.substr(std::strlen("*** Delete File:")));
            edits.push_back(e);
            current = &edits.back();
            continue;
        }
        if (!current) continue;
        if (current->kind == BlockEdit::Kind::Add) {
            std::string body = line;
            if (!body.empty() && body[0] == '+') body.erase(0, 1);
            current->content += body;
            current->content += "\n";
            continue;
        }
        if (current->kind == BlockEdit::Kind::Update) {
            if (starts_with(line, "@@")) continue;     // hunk header (context is inferred)
            if (line.empty()) { current->lines.push_back({' ', 0, 0, ""}); continue; }
            char c = line[0];
            if (c == ' ' || c == '-' || c == '+' || c == '\\') {
                if (c == '\\') continue;               // "\ No newline at end of file"
                current->lines.push_back({c, 0, 0, line.substr(1)});
            } else {
                current->lines.push_back({' ', 0, 0, line});
            }
        }
    }
    return edits;
}

// Applies a block-level update: the '-'/' ' lines describe the region to find,
// the '+'/' ' lines the replacement.
Result<size_t> apply_block_lines(std::string& text, const std::vector<DiffOp>& ops, std::string* detail) {
    std::vector<std::string> old_block, new_block;
    for (const DiffOp& op : ops) {
        if (op.kind == ' ') { old_block.push_back(op.text); new_block.push_back(op.text); }
        else if (op.kind == '-') old_block.push_back(op.text);
        else if (op.kind == '+') new_block.push_back(op.text);
    }
    if (old_block.empty() && new_block.empty()) return 0;

    auto file_lines = split_lines(text);
    auto matches = [](const std::vector<std::string>& hay, size_t at,
                      const std::vector<std::string>& needle) {
        if (at + needle.size() > hay.size()) return false;
        for (size_t i = 0; i < needle.size(); ++i)
            if (trim(hay[at + i]) != trim(needle[i])) return false;
        return true;
    };
    auto find_block = [&](const std::vector<std::string>& needle, size_t* at) {
        for (size_t start = 0; start + needle.size() <= file_lines.size(); ++start) {
            if (matches(file_lines, start, needle)) { *at = start; return true; }
        }
        return false;
    };

    if (old_block.empty()) {
        // Pure insertion: append at the end.
        for (const std::string& l : new_block) file_lines.push_back(l);
        text = join(file_lines, "\n");
        if (!text.empty()) text += "\n";
        return new_block.size();
    }
    size_t at = 0;
    if (!find_block(old_block, &at)) {
        if (detail) *detail = "block not found (" + std::to_string(old_block.size()) + " lines)";
        return Error(Code::NotFound, *detail);
    }
    std::vector<std::string> result;
    result.insert(result.end(), file_lines.begin(), file_lines.begin() + ptrdiff_t(at));
    result.insert(result.end(), new_block.begin(), new_block.end());
    result.insert(result.end(), file_lines.begin() + ptrdiff_t(at + old_block.size()), file_lines.end());
    text = join(result, "\n");
    if (!text.empty()) text += "\n";
    return new_block.size();
}

// -----------------------------------------------------------------------------
// Unified diff parsing / application
// -----------------------------------------------------------------------------
struct HunkLine { char kind; std::string text; };
struct Hunk { size_t a_start{0}, a_count{0}, b_start{0}, b_count{0}; std::vector<HunkLine> lines; };
struct UnifiedFile { std::string old_path, new_path; std::vector<Hunk> hunks; };

bool parse_hunk_header(const std::string& line, Hunk* out) {
    // @@ -a[,c] +b[,d] @@
    int a_start = 0, a_count = 1, b_start = 0, b_count = 1;
    if (std::sscanf(line.c_str(), "@@ -%d,%d +%d,%d @@", &a_start, &a_count, &b_start, &b_count) == 4) {
        // ok
    } else if (std::sscanf(line.c_str(), "@@ -%d +%d,%d @@", &a_start, &b_start, &b_count) == 3) {
        a_count = 1;
    } else if (std::sscanf(line.c_str(), "@@ -%d,%d +%d @@", &a_start, &a_count, &b_start) == 3) {
        b_count = 1;
    } else if (std::sscanf(line.c_str(), "@@ -%d +%d @@", &a_start, &b_start) == 2) {
        a_count = b_count = 1;
    } else {
        return false;
    }
    out->a_start = size_t(std::max(0, a_start));
    out->a_count = size_t(std::max(0, a_count));
    out->b_start = size_t(std::max(0, b_start));
    out->b_count = size_t(std::max(0, b_count));
    return true;
}

std::vector<UnifiedFile> parse_unified_diff(std::string_view patch) {
    std::vector<UnifiedFile> files;
    UnifiedFile* current = nullptr;
    Hunk* hunk = nullptr;
    for (const std::string& raw : split_lines(patch)) {
        std::string line = raw;
        if (starts_with(line, "--- ")) {
            UnifiedFile f;
            f.old_path = trim(line.substr(4));
            // strip a/ b/ prefixes and trailing timestamps
            size_t tab = f.old_path.find('\t');
            if (tab != std::string::npos) f.old_path.resize(tab);
            files.push_back(f);
            current = &files.back();
            hunk = nullptr;
            continue;
        }
        if (starts_with(line, "+++ ") && current) {
            current->new_path = trim(line.substr(4));
            size_t tab = current->new_path.find('\t');
            if (tab != std::string::npos) current->new_path.resize(tab);
            continue;
        }
        if (starts_with(line, "@@") && current) {
            Hunk h;
            if (parse_hunk_header(line, &h)) {
                current->hunks.push_back(h);
                hunk = &current->hunks.back();
            } else {
                hunk = nullptr;
            }
            continue;
        }
        if (!current || !hunk) continue;
        if (line == "\\ No newline at end of file") continue;
        if (line.empty()) { hunk->lines.push_back({' ', ""}); continue; }
        char c = line[0];
        if (c == ' ' || c == '-' || c == '+') hunk->lines.push_back({c, line.substr(1)});
        else if (c == '\r') continue;
    }
    return files;
}

std::string strip_ab_prefix(const std::string& p) {
    std::string s = p;
    if (!s.empty() && s[0] == '/') return s;      // absolute paths kept as-is
    if (starts_with(s, "a/") || starts_with(s, "b/")) s = s.substr(2);
    return s;
}

bool apply_hunks(std::string& text, const std::vector<Hunk>& hunks, int fuzz, std::string* why) {
    std::vector<std::string> lines = split_lines(text);
    bool had_newline = !text.empty() && text.back() == '\n';
    // Process hunks from the bottom up so earlier line numbers stay valid.
    for (size_t hi = hunks.size(); hi-- > 0;) {
        const Hunk& h = hunks[hi];
        size_t a_start = h.a_start;
        std::vector<std::string> old_lines, new_lines;
        for (const HunkLine& hl : h.lines) {
            if (hl.kind == ' ') { old_lines.push_back(hl.text); new_lines.push_back(hl.text); }
            else if (hl.kind == '-') old_lines.push_back(hl.text);
            else if (hl.kind == '+') new_lines.push_back(hl.text);
        }
        auto matches_at = [&](size_t at, int f) {
            (void)f;
            if (at > lines.size()) return false;
            if (at + old_lines.size() > lines.size()) return false;
            for (size_t i = 0; i < old_lines.size(); ++i)
                if (trim(lines[at + i]) != trim(old_lines[i])) return false;
            return true;
        };
        size_t index = lines.size();
        size_t start = a_start > 0 ? a_start - 1 : 0;
        bool found = false;
        for (int f = 0; f <= fuzz && !found; ++f) {
            if (matches_at(start, f)) { index = start; found = true; break; }
            // search nearby
            for (size_t delta = 1; delta < 200 && !found; ++delta) {
                if (start >= delta && matches_at(start - delta, f)) { index = start - delta; found = true; break; }
                if (matches_at(start + delta, f)) { index = start + delta; found = true; break; }
            }
        }
        if (!found) {
            if (why) *why = "hunk at line " + std::to_string(a_start) + " does not match";
            return false;
        }
        std::vector<std::string> rebuilt;
        rebuilt.insert(rebuilt.end(), lines.begin(), lines.begin() + ptrdiff_t(index));
        rebuilt.insert(rebuilt.end(), new_lines.begin(), new_lines.end());
        rebuilt.insert(rebuilt.end(),
                       lines.begin() + ptrdiff_t(index + old_lines.size()), lines.end());
        lines.swap(rebuilt);
    }
    text = join(lines, "\n");
    if (had_newline && !text.empty() && text.back() != '\n') text += "\n";
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// FileInfo / helpers
// -----------------------------------------------------------------------------
std::string FileInfo::extension() const {
    std::string base = name.empty() ? path : name;
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    return lower(base.substr(dot));
}

bool file_exists(const std::string& abs_path) {
    struct stat st {};
    return stat_path(abs_path, &st);
}
bool is_directory(const std::string& abs_path) {
    struct stat st {};
    return stat_path(abs_path, &st) && is_dir_mode(st.st_mode);
}
int64_t file_mtime_ms(const std::string& abs_path) {
    struct stat st {};
    if (!stat_path(abs_path, &st)) return 0;
    return int64_t(st.st_mtime) * 1000;
}

std::string absolute_path(const std::string& path) {
    if (!path.empty() && path[0] == '/') return normalise_path(path);
    char cwd[4096];
    if (!::getcwd(cwd, sizeof(cwd))) return normalise_path(path);
    return normalise_path(std::string(cwd) + "/" + path);
}
std::string directory_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}
std::string basename_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string parent_of(const std::string& path) { return directory_of(path); }

std::string language_for_path(std::string_view path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of('/');
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash))
        ext = lower(std::string(path.substr(dot)));
    std::string name = lower(std::string(basename_of(std::string(path))));
    if (name == "makefile" || name == "gnumakefile") return "make";
    if (name == "cmakelists.txt") return "cmake";
    if (name == "dockerfile") return "docker";
    if (name == "readme" || name == "readme.md") return "markdown";
    if (ext == ".c") return "c";
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") return "cpp";
    if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".ipp" || ext == ".inc") return "cpp";
    if (ext == ".m" || ext == ".mm") return "objective-c";
    if (ext == ".rs") return "rust";
    if (ext == ".go") return "go";
    if (ext == ".py" || ext == ".pyi") return "python";
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return "javascript";
    if (ext == ".ts" || ext == ".tsx") return "typescript";
    if (ext == ".jsx") return "javascript";
    if (ext == ".java") return "java";
    if (ext == ".kt" || ext == ".kts") return "kotlin";
    if (ext == ".scala") return "scala";
    if (ext == ".cs") return "csharp";
    if (ext == ".rb") return "ruby";
    if (ext == ".php") return "php";
    if (ext == ".pl") return "perl";
    if (ext == ".lua") return "lua";
    if (ext == ".sh" || ext == ".bash" || ext == ".zsh" || ext == ".fish") return "shell";
    if (ext == ".ps1") return "powershell";
    if (ext == ".sql") return "sql";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".css" || ext == ".scss" || ext == ".sass" || ext == ".less") return "css";
    if (ext == ".xml") return "xml";
    if (ext == ".json" || ext == ".jsonc") return "json";
    if (ext == ".yaml" || ext == ".yml") return "yaml";
    if (ext == ".toml") return "toml";
    if (ext == ".ini" || ext == ".cfg" || ext == ".conf") return "ini";
    if (ext == ".md" || ext == ".markdown" || ext == ".rst") return "markdown";
    if (ext == ".txt" || ext == ".log") return "text";
    if (ext == ".cmake") return "cmake";
    if (ext == ".mk") return "make";
    if (ext == ".sh") return "shell";
    if (ext == ".proto") return "protobuf";
    if (ext == ".tf" || ext == ".hcl") return "hcl";
    return {};
}

bool looks_like_entry_point(std::string_view rel) {
    std::string name = lower(basename_of(std::string(rel)));
    static const std::vector<std::string> names = {
        "main.cpp", "main.c", "main.cc", "main.rs", "main.go", "main.py", "main.js",
        "main.ts", "main.java", "index.js", "index.ts", "index.tsx", "app.js", "app.py",
        "server.js", "server.py", "cli.py", "cli.cpp", "__main__.py", "manage.py",
        "program.cs", "application.rb", "mod.rs", "lib.rs", "cmd"};
    for (const std::string& candidate : names)
        if (name == candidate) return true;
    if (ends_with(rel, ".sh") && (contains(rel, "bin/") || contains(rel, "scripts/"))) return true;
    return false;
}

bool looks_like_build_file(std::string_view rel) {
    std::string name = lower(basename_of(std::string(rel)));
    static const std::vector<std::string> names = {
        "cmakelists.txt", "makefile", "gnumakefile", "build.gradle", "pom.xml",
        "package.json", "cargo.toml", "pyproject.toml", "setup.py", "setup.cfg",
        "requirements.txt", "go.mod", "build.sh", "configure", "meson.build",
        "dockerfile", "docker-compose.yml", "docker-compose.yaml", ".gitlab-ci.yml"};
    for (const std::string& candidate : names)
        if (name == candidate) return true;
    if (ends_with(rel, ".sln") || ends_with(rel, ".vcxproj") || ends_with(rel, ".cbp")) return true;
    return false;
}

// -----------------------------------------------------------------------------
// Workspace
// -----------------------------------------------------------------------------
Result<Workspace> workspace_open(const WorkspaceConfig& cfg) {
    Workspace ws;
    ws.cfg = cfg;
    Result<std::string> abs = canonicalise(absolute_path(cfg.root), true);
    if (!abs.ok()) return Error(abs.error());
    ws.root_abs = *abs;
    if (!file_exists(ws.root_abs)) {
        Error e = make_dirs(ws.root_abs);
        if (!e.ok()) return Error(e);
    }
    if (!is_directory(ws.root_abs))
        return LCA_FAIL(Code::InvalidArgument, "workspace root is not a directory: " + ws.root_abs);
    LCA_LOG_DEBUG(kScope, "workspace root = " + ws.root_abs);
    return ws;
}

Result<std::string> fs_resolve(const Workspace& ws, std::string_view path) {
    if (path.empty()) return LCA_FAIL(Code::InvalidArgument, "empty path");
    std::string text(path);
    if (text == "." || text == "./") return ws.root_abs;
    if (text.find('\0') != std::string::npos) return LCA_FAIL(Code::InvalidArgument, "path contains NUL");
    // Relative paths are always taken from the workspace root, never from the
    // process working directory, so a tool call means the same thing wherever
    // the agent was started from.
    std::string combined = (!text.empty() && text[0] == '/') ? text : (ws.root_abs + "/" + text);
    Result<std::string> canon = canonicalise(combined, ws.cfg.follow_symlinks);
    if (!canon.ok()) return Error(canon.error());
    std::string abs = *canon;
    if (!ws.cfg.allow_outside_root && !path_is_inside(ws.root_abs, abs))
        return LCA_FAIL(Code::SandboxViolation, "path escapes the workspace: " + text);
    return abs;
}

std::string fs_relative(const Workspace& ws, const std::string& abs_path) {
    if (abs_path == ws.root_abs) return ".";
    if (path_is_inside(ws.root_abs, abs_path)) return abs_path.substr(ws.root_abs.size() + 1);
    return abs_path;
}

// -----------------------------------------------------------------------------
// Stats / listing / glob
// -----------------------------------------------------------------------------
Result<FileInfo> fs_stat(const Workspace& ws, std::string_view path) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    struct stat st {};
    if (::lstat(abs->c_str(), &st) != 0)
        return LCA_FAIL(Code::NotFound, "no such file or directory: " + std::string(path));
    FileInfo info;
    info.path = *abs;
    info.rel = fs_relative(ws, *abs);
    info.name = basename_of(*abs);
    info.is_symlink = S_ISLNK(st.st_mode) != 0;
    info.is_dir = is_dir_mode(st.st_mode);
    info.size = info.is_dir ? 0 : uint64_t(st.st_size);
    info.mtime_ms = int64_t(st.st_mtime) * 1000;
    info.is_text = !info.is_dir && !language_for_path(info.name).empty();
    return info;
}

Result<std::vector<FileInfo>> fs_list(const Workspace& ws, std::string_view dir,
                                      bool recursive, size_t max_entries) {
    Result<std::string> abs = fs_resolve(ws, dir);
    if (!abs.ok()) return Error(abs.error());
    if (!is_directory(*abs))
        return LCA_FAIL(Code::InvalidArgument, "not a directory: " + std::string(dir));
    std::vector<FileInfo> out;
    size_t limit = max_entries ? max_entries : ws.cfg.max_entries;
    WalkState st;
    st.out = &out;
    st.limit = limit;
    st.recursive = recursive;
    st.ws = &ws;
    walk_dir(st, *abs, 0);
    return out;
}

// -----------------------------------------------------------------------------
// glob_match -- supports *, ?, **, [set], [!set] and {a,b}
// -----------------------------------------------------------------------------
namespace {

bool glob_match_impl(std::string_view pat, size_t pi, std::string_view text, size_t ti, int depth);

bool match_class(std::string_view pat, size_t pi, char c, size_t* next) {
    // pi points just after '['
    bool negate = false;
    size_t i = pi;
    if (i < pat.size() && (pat[i] == '!' || pat[i] == '^')) { negate = true; ++i; }
    bool matched = false;
    bool first = true;
    while (i < pat.size() && (pat[i] != ']' || first)) {
        first = false;
        char lo = pat[i];
        if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']') {
            char hi = pat[i + 2];
            if (c >= lo && c <= hi) matched = true;
            i += 3;
        } else {
            if (c == lo) matched = true;
            ++i;
        }
    }
    if (i >= pat.size()) return false;     // unterminated class
    *next = i + 1;                          // consume ']'
    return matched != negate;
}

bool glob_match_impl(std::string_view pat, size_t pi, std::string_view text, size_t ti, int depth) {
    if (depth > 64) return false;
    while (pi < pat.size()) {
        char pc = pat[pi];
        if (pc == '*') {
            // "**" spans directory separators; "*" does not.
            bool globstar = (pi + 1 < pat.size() && pat[pi + 1] == '*');
            if (globstar) {
                ++pi;
                while (pi < pat.size() && pat[pi] == '*') ++pi;
                if (pi < pat.size() && pat[pi] == '/') ++pi;
                if (pi >= pat.size()) return true;
                for (size_t k = ti; k <= text.size(); ++k)
                    if (glob_match_impl(pat, pi, text, k, depth + 1)) return true;
                return false;
            }
            ++pi;
            for (size_t k = ti; k <= text.size(); ++k) {
                if (k > ti && text[k - 1] == '/') return false;
                if (glob_match_impl(pat, pi, text, k, depth + 1)) return true;
            }
            return false;
        }
        if (pc == '?') {
            if (ti >= text.size() || text[ti] == '/') return false;
            ++pi; ++ti;
            continue;
        }
        if (pc == '[') {
            if (ti >= text.size()) return false;
            size_t next = pi;
            if (!match_class(pat, pi + 1, text[ti], &next)) return false;
            pi = next; ++ti;
            continue;
        }
        if (pc == '{') {
            // Try each alternative in the brace group.
            int depth_braces = 0;
            size_t end = std::string_view::npos;
            for (size_t i = pi; i < pat.size(); ++i) {
                if (pat[i] == '{') ++depth_braces;
                else if (pat[i] == '}') {
                    if (--depth_braces == 0) { end = i; break; }
                }
            }
            if (end == std::string_view::npos) return false;   // literal '{'
            std::string_view inner = pat.substr(pi + 1, end - pi - 1);
            int nest = 0;
            size_t start = 0;
            std::vector<std::string_view> alts;
            for (size_t i = 0; i <= inner.size(); ++i) {
                if (i == inner.size() || (inner[i] == ',' && nest == 0)) {
                    alts.push_back(inner.substr(start, i - start));
                    start = i + 1;
                    continue;
                }
                if (inner[i] == '{') ++nest;
                else if (inner[i] == '}') --nest;
            }
            std::string_view tail = pat.substr(end + 1);
            for (std::string_view alt : alts) {
                std::string combined(alt);
                combined += std::string(tail);
                if (glob_match_impl(combined, 0, text, ti, depth + 1)) return true;
            }
            return false;
        }
        if (ti >= text.size() || pc != text[ti]) return false;
        ++pi; ++ti;
    }
    return ti == text.size();
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    return glob_match_impl(pattern, 0, text, 0, 0);
}

Result<std::vector<FileInfo>> fs_glob(const Workspace& ws, std::string_view pattern,
                                      size_t max_results) {
    std::string pat(pattern);
    if (starts_with(pat, "./")) pat = pat.substr(2);
    std::vector<FileInfo> out;
    size_t limit = max_results ? max_results : ws.cfg.max_entries;
    WalkState st;
    st.out = &out;
    st.limit = limit;
    st.recursive = true;
    st.ws = &ws;
    walk_dir(st, ws.root_abs, 0);
    std::vector<FileInfo> matched;
    for (const FileInfo& info : out) {
        if (glob_match(pat, info.rel) || glob_match(pat, info.name)) matched.push_back(info);
        if (matched.size() >= limit) break;
    }
    return matched;
}

// -----------------------------------------------------------------------------
// Read / write
// -----------------------------------------------------------------------------
Result<ReadResult> fs_read(const Workspace& ws, std::string_view path, size_t max_bytes) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    size_t cap = max_bytes ? max_bytes : size_t(ws.cfg.max_read_bytes);
    if (cap == 0) cap = size_t(ws.cfg.max_read_bytes);
    bool truncated = false;
    Result<std::string> content = read_whole_file(*abs, cap, &truncated);
    if (!content.ok()) return Error(content.error());
    if (looks_binary(*content))
        return LCA_FAIL(Code::Unsupported, "refusing to treat a binary file as text: " + std::string(path));
    ReadResult rr;
    rr.text = *content;
    rr.truncated = truncated;
    rr.total_bytes = size_t(file_exists(*abs) ? fs_stat(ws, path).value_or(FileInfo{}).size : rr.text.size());
    rr.shown_bytes = rr.text.size();
    rr.line_count = count_lines(rr.text);
    rr.language_hint = language_for_path(*abs);
    Result<FileInfo> info = fs_stat(ws, path);
    if (info.ok()) rr.info = *info;
    return rr;
}

Result<WriteResult> fs_write(const Workspace& ws, std::string_view path, std::string_view content,
                             bool create_parents) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    if (!create_parents) {
        std::string dir = directory_of(*abs);
        if (!is_directory(dir))
            return LCA_FAIL(Code::NotFound, "parent directory does not exist: " + dir);
    }
    MemoryReservation res(content.size() + 4096, "fs.write");
    if (!res.ok()) return LCA_FAIL(Code::ResourceExhausted, "write exceeds the memory budget");

    WriteResult out;
    out.path = *abs;
    out.created = !file_exists(*abs);

    std::string before;
    if (!out.created) {
        bool truncated = false;
        Result<std::string> existing = read_whole_file(*abs, size_t(ws.cfg.max_read_bytes) * 4, &truncated);
        if (existing.ok()) before = *existing;
        if (ws.cfg.create_backups) {
            Result<std::string> backup = fs_backup(ws, path);
            if (backup.ok()) out.backup_path = *backup;
        }
    }
    Error e = atomic_write(*abs, content);
    if (!e.ok()) return Error(e);
    out.bytes_written = content.size();
    if (!out.created && before != content)
        out.diff = unified_diff(before, content, fs_relative(ws, *abs));
    return out;
}

Result<WriteResult> fs_append(const Workspace& ws, std::string_view path, std::string_view content,
                              bool create_parents) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    if (!file_exists(*abs)) return fs_write(ws, path, content, create_parents);
    bool truncated = false;
    Result<std::string> existing = read_whole_file(*abs, size_t(ws.cfg.max_read_bytes) * 4, &truncated);
    if (!existing.ok()) return Error(existing.error());
    std::string merged = *existing;
    merged += std::string(content);
    return fs_write(ws, path, merged, create_parents);
}

Result<bool> fs_mkdirs(const Workspace& ws, std::string_view path) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    Error e = make_dirs(*abs);
    if (!e.ok()) return Error(e);
    return true;
}

Result<bool> fs_delete(const Workspace& ws, std::string_view path, bool recursive) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    if (*abs == ws.root_abs)
        return LCA_FAIL(Code::SandboxViolation, "refusing to delete the workspace root");
    struct stat st {};
    if (::lstat(abs->c_str(), &st) != 0)
        return LCA_FAIL(Code::NotFound, "no such path: " + std::string(path));
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            // Only allow removing an empty directory without the recursive flag.
            DIR* dir = ::opendir(abs->c_str());
            if (!dir) return LCA_FAIL(Code::IoError, "opendir failed: " + errno_text());
            bool empty = true;
            while (struct dirent* ent = ::readdir(dir)) {
                std::string n = ent->d_name;
                if (n != "." && n != "..") { empty = false; break; }
            }
            ::closedir(dir);
            if (!empty) return LCA_FAIL(Code::InvalidArgument, "directory is not empty: " + std::string(path));
            if (::rmdir(abs->c_str()) != 0)
                return LCA_FAIL(Code::IoError, "rmdir: " + errno_text());
            return true;
        }
        Result<std::vector<FileInfo>> entries = fs_list(ws, path, true, ws.cfg.max_entries);
        if (!entries.ok()) return Error(entries.error());
        // Delete deepest-first.
        std::vector<FileInfo> sorted = *entries;
        std::sort(sorted.begin(), sorted.end(), [](const FileInfo& x, const FileInfo& y) {
            return x.path.size() > y.path.size();
        });
        for (const FileInfo& e : sorted) {
            Result<std::string> target = fs_resolve(ws, e.path);
            if (!target.ok()) continue;
            if (e.is_dir) ::rmdir(target->c_str());
            else          ::unlink(target->c_str());
        }
        if (::rmdir(abs->c_str()) != 0 && errno != ENOTEMPTY && errno != ENOENT)
            return LCA_FAIL(Code::IoError, "rmdir: " + errno_text());
        return true;
    }
    if (ws.cfg.create_backups) fs_backup(ws, path);
    if (::unlink(abs->c_str()) != 0)
        return LCA_FAIL(Code::IoError, "unlink: " + errno_text());
    return true;
}

Result<bool> fs_copy(const Workspace& ws, std::string_view from, std::string_view to) {
    Result<std::string> src = fs_resolve(ws, from);
    if (!src.ok()) return Error(src.error());
    Result<std::string> dst = fs_resolve(ws, to);
    if (!dst.ok()) return Error(dst.error());
    struct stat st {};
    if (::lstat(src->c_str(), &st) != 0) return LCA_FAIL(Code::NotFound, "no such file: " + std::string(from));
    if (is_dir_mode(st.st_mode)) {
        Error e = make_dirs(*dst);
        if (!e.ok()) return Error(e);
        Result<std::vector<FileInfo>> entries = fs_list(ws, from, true, ws.cfg.max_entries);
        if (!entries.ok()) return Error(entries.error());
        for (const FileInfo& e : *entries) {
            std::string rel = fs_relative(ws, e.path);
            std::string rel_from_root = fs_relative(ws, *src);
            std::string target = rel.substr(rel_from_root.size());
            if (e.is_dir) {
                Result<bool> mk = fs_mkdirs(ws, *dst + target);
                if (!mk.ok()) return Error(mk.error());
                continue;
            }
            bool truncated = false;
            Result<std::string> data = read_whole_file(e.path, 1u << 30, &truncated);
            if (!data.ok()) return Error(data.error());
            Error e2 = make_dirs(directory_of(*dst + target));
            if (!e2.ok()) return Error(e2);
            Error e3 = atomic_write(*dst + target, *data);
            if (!e3.ok()) return Error(e3);
        }
        return true;
    }
    bool truncated = false;
    Result<std::string> data = read_whole_file(*src, 1u << 30, &truncated);
    if (!data.ok()) return Error(data.error());
    Error e = atomic_write(*dst, *data);
    if (!e.ok()) return Error(e);
    return true;
}

Result<bool> fs_move(const Workspace& ws, std::string_view from, std::string_view to) {
    Result<std::string> src = fs_resolve(ws, from);
    if (!src.ok()) return Error(src.error());
    Result<std::string> dst = fs_resolve(ws, to);
    if (!dst.ok()) return Error(dst.error());
    Error e = make_dirs(directory_of(*dst));
    if (!e.ok()) return Error(e);
    if (::rename(src->c_str(), dst->c_str()) == 0) return true;
    // Cross-device: copy + delete.
    Result<bool> copied = fs_copy(ws, from, to);
    if (!copied.ok()) return Error(copied.error());
    return fs_delete(ws, from, true);
}

// -----------------------------------------------------------------------------
// Backups
// -----------------------------------------------------------------------------
Result<std::string> fs_backup(const Workspace& ws, std::string_view path) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    if (!file_exists(*abs)) return LCA_FAIL(Code::NotFound, "nothing to back up: " + std::string(path));
    bool truncated = false;
    Result<std::string> data = read_whole_file(*abs, 1u << 30, &truncated);
    if (!data.ok()) return Error(data.error());
    std::string target = *abs + ".lca-backup-" + std::to_string(wall_millis());
    Error e = atomic_write(target, *data);
    if (!e.ok()) return Error(e);
    return target;
}

Result<bool> fs_restore(const Workspace& ws, std::string_view path, std::string_view backup_path) {
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return Error(abs.error());
    Result<std::string> backup = fs_resolve(ws, backup_path);
    if (!backup.ok()) return Error(backup.error());
    bool truncated = false;
    Result<std::string> data = read_whole_file(*backup, 1u << 30, &truncated);
    if (!data.ok()) return Error(data.error());
    Error e = atomic_write(*abs, *data);
    if (!e.ok()) return Error(e);
    return true;
}

std::vector<std::string> fs_list_backups(const Workspace& ws, std::string_view path) {
    std::vector<std::string> out;
    Result<std::string> abs = fs_resolve(ws, path);
    if (!abs.ok()) return out;
    std::string dir = directory_of(*abs);
    std::string base = basename_of(*abs) + ".lca-backup-";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* ent = ::readdir(d)) {
        std::string name = ent->d_name;
        if (starts_with(name, base)) out.push_back(dir + "/" + name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// -----------------------------------------------------------------------------
// Grep
// -----------------------------------------------------------------------------
Result<GrepSummary> fs_grep(const Workspace& ws, std::string_view pattern, const GrepOptions& opts) {
    if (pattern.empty()) return LCA_FAIL(Code::InvalidArgument, "empty search pattern");
    GrepSummary summary;
    std::regex re;
    if (opts.regex) {
        try {
            re = std::regex(std::string(pattern),
                            std::regex::ECMAScript | (opts.ignore_case ? std::regex::icase
                                                                      : std::regex::flag_type{}));
        } catch (const std::regex_error& e) {
            return LCA_FAIL(Code::ParseError, std::string("invalid regular expression: ") + e.what());
        }
    }
    std::string needle = opts.ignore_case ? lower(std::string(pattern)) : std::string(pattern);

    auto matches_line = [&](const std::string& line) {
        if (opts.regex) return std::regex_search(line, re);
        const std::string hay = opts.ignore_case ? lower(line) : line;
        return hay.find(needle) != std::string::npos;
    };

    Result<std::vector<FileInfo>> files = fs_list(ws, ".", true, ws.cfg.max_entries * 4);
    if (!files.ok()) return Error(files.error());

    for (const FileInfo& info : *files) {
        if (info.is_dir) continue;
        if (!opts.include_glob.empty() && opts.include_glob != "*" &&
            !glob_match(opts.include_glob, info.rel) && !glob_match(opts.include_glob, info.name))
            continue;
        if (!opts.exclude_glob.empty() &&
            (glob_match(opts.exclude_glob, info.rel) || glob_match(opts.exclude_glob, info.name)))
            continue;
        if (info.size > opts.max_file_bytes) continue;
        bool truncated = false;
        Result<std::string> content = read_whole_file(info.path, opts.max_file_bytes, &truncated);
        if (!content.ok()) continue;
        ++summary.files_scanned;
        if (!opts.include_binary && looks_binary(*content)) continue;
        std::vector<std::string> lines = split_lines(*content);
        bool file_matched = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!matches_line(lines[i])) continue;
            file_matched = true;
            size_t first = i >= size_t(opts.context_lines) ? i - size_t(opts.context_lines) : 0;
            for (size_t k = first; k <= i; ++k) {
                if (k == i) continue;
                summary.hits.push_back({info.rel, k + 1, lines[k], false});
            }
            summary.hits.push_back({info.rel, i + 1, lines[i], true});
            for (int c = 1; c <= opts.context_lines; ++c) {
                size_t k = i + size_t(c);
                if (k < lines.size()) summary.hits.push_back({info.rel, k + 1, lines[k], false});
            }
            if (summary.hits.size() >= opts.max_hits) {
                summary.truncated = true;
                summary.files_matched += file_matched ? 1 : 0;
                return summary;
            }
        }
        if (file_matched) ++summary.files_matched;
    }
    return summary;
}

// -----------------------------------------------------------------------------
// Structured edits
// -----------------------------------------------------------------------------
Result<EditOutcome> fs_edit(const Workspace& ws, const EditRequest& req) {
    EditOutcome outcome;
    Result<std::string> abs = fs_resolve(ws, req.path);
    if (!abs.ok()) return Error(abs.error());
    outcome.path = fs_relative(ws, *abs);

    std::string original;
    bool exists = file_exists(*abs);
    if (exists) {
        bool truncated = false;
        Result<std::string> data = read_whole_file(*abs, size_t(ws.cfg.max_read_bytes) * 4, &truncated);
        if (!data.ok()) return Error(data.error());
        original = *data;
    }
    outcome.bytes_before = original.size();
    std::string updated = original;
    std::string failure;

    switch (req.op) {
        case EditRequest::Op::CreateFile: {
            if (exists && !original.empty() && !req.replace.empty()) { /* replace below */ }
            updated = req.content;
            outcome.replacements = 1;
            break;
        }
        case EditRequest::Op::DeleteFile: {
            if (!req.dry_run) {
                Result<bool> del = fs_delete(ws, req.path, true);
                if (!del.ok()) return Error(del.error());
            }
            outcome.applied = !req.dry_run;
            outcome.diff = unified_diff(original, "", outcome.path);
            return outcome;
        }
        case EditRequest::Op::ReplaceExact:
        case EditRequest::Op::ReplaceAll: {
            if (req.find.empty()) return LCA_FAIL(Code::InvalidArgument, "empty search text");
            if (!exists) return LCA_FAIL(Code::NotFound, "no such file: " + req.path);
            // Count first: an exact replacement must be unambiguous, and nothing
            // may be written when it is not.
            size_t occurrences = 0, scan = 0;
            while ((scan = updated.find(req.find, scan)) != std::string::npos) {
                ++occurrences;
                scan += req.find.size();
            }
            if (occurrences == 0) {
                failure = "'" + ellipsize(req.find, 60) + "' not found in " + req.path;
                return LCA_FAIL(Code::NotFound, failure);
            }
            if (req.op == EditRequest::Op::ReplaceExact && occurrences > 1)
                return LCA_FAIL(Code::InvalidArgument,
                                "search text occurs " + std::to_string(occurrences) +
                                    " times; use ReplaceAll or a longer excerpt");
            size_t count = 0, pos = 0;
            std::string built;
            while (true) {
                size_t at = updated.find(req.find, pos);
                if (at == std::string::npos) break;
                built.append(updated, pos, at - pos);
                built.append(req.replace);
                pos = at + req.find.size();
                ++count;
                if (req.op == EditRequest::Op::ReplaceExact) break;
            }
            built.append(updated, pos, std::string::npos);
            updated = built;
            outcome.replacements = count;
            break;
        }
        case EditRequest::Op::ReplaceLines:
        case EditRequest::Op::InsertBeforeLine:
        case EditRequest::Op::InsertAfterLine:
        case EditRequest::Op::DeleteLines: {
            if (!exists) return LCA_FAIL(Code::NotFound, "no such file: " + req.path);
            std::vector<std::string> lines = split_lines(original);
            bool had_newline = !original.empty() && original.back() == '\n';
            size_t start = req.line > 0 ? req.line - 1 : 0;
            size_t end = req.end_line > 0 ? req.end_line - 1 : start;
            if (start > lines.size()) start = lines.size();
            if (end >= lines.size()) end = lines.empty() ? 0 : lines.size() - 1;
            if (req.op == EditRequest::Op::InsertBeforeLine || req.op == EditRequest::Op::InsertAfterLine) {
                size_t at = req.op == EditRequest::Op::InsertBeforeLine ? start : std::min(start + 1, lines.size());
                std::vector<std::string> added = split_lines(req.content);
                lines.insert(lines.begin() + ptrdiff_t(at), added.begin(), added.end());
                outcome.replacements = added.size();
            } else if (req.op == EditRequest::Op::ReplaceLines) {
                std::vector<std::string> replacement = split_lines(req.content);
                if (end + 1 <= lines.size())
                    lines.erase(lines.begin() + ptrdiff_t(start), lines.begin() + ptrdiff_t(end + 1));
                lines.insert(lines.begin() + ptrdiff_t(start), replacement.begin(), replacement.end());
                outcome.replacements = replacement.size();
            } else {   // DeleteLines
                if (end + 1 <= lines.size()) {
                    lines.erase(lines.begin() + ptrdiff_t(start), lines.begin() + ptrdiff_t(end + 1));
                    outcome.replacements = end - start + 1;
                }
            }
            updated = join(lines, "\n");
            if (had_newline && !updated.empty()) updated += "\n";
            break;
        }
    }

    if (req.dry_run) {
        outcome.applied = false;
        outcome.bytes_after = updated.size();
        outcome.diff = unified_diff(original, updated, outcome.path);
        return outcome;
    }
    Result<WriteResult> w = fs_write(ws, req.path, updated, true);
    if (!w.ok()) return Error(w.error());
    outcome.applied = true;
    outcome.bytes_after = updated.size();
    outcome.diff = unified_diff(original, updated, outcome.path);
    return outcome;
}

// -----------------------------------------------------------------------------
// Diff
// -----------------------------------------------------------------------------
std::string unified_diff(std::string_view before, std::string_view after,
                         const std::string& label, int context) {
    if (before == after) return {};
    bool before_nl = true, after_nl = true;
    std::vector<std::string> a = to_lines(before, &before_nl);
    std::vector<std::string> b = to_lines(after, &after_nl);
    std::vector<DiffOp> ops = diff_lines(a, b);

    std::string out;
    out += "--- a/" + label + "\n";
    out += "+++ b/" + label + "\n";

    size_t i = 0;
    while (i < ops.size()) {
        if (ops[i].kind == ' ') { ++i; continue; }
        size_t start = i;
        while (start > 0 && ops[start - 1].kind == ' ') --start;
        size_t lead_context = i - start;
        if (int(lead_context) > context) {
            start = i - size_t(context);
            lead_context = size_t(context);
        }
        // extend forward
        size_t end = i;
        size_t trailing_context = 0;
        while (end < ops.size()) {
            if (ops[end].kind == ' ') {
                size_t run = 0;
                while (end + run < ops.size() && ops[end + run].kind == ' ') ++run;
                if (run > size_t(context) * 2 && end + run < ops.size()) {
                    trailing_context = size_t(context);
                    end += size_t(context);
                    break;
                }
                if (end + run == ops.size()) { trailing_context = run; end += run; break; }
                end += run;
                continue;
            }
            ++end;
        }
        size_t a_start = 0, b_start = 0, a_count = 0, b_count = 0;
        bool a_seen = false, b_seen = false;
        for (size_t k = start; k < end && k < ops.size(); ++k) {
            const DiffOp& op = ops[k];
            if (op.kind != '+') {
                if (!a_seen) { a_start = op.a_line; a_seen = true; }
                ++a_count;
            }
            if (op.kind != '-') {
                if (!b_seen) { b_start = op.b_line; b_seen = true; }
                ++b_count;
            }
        }
        if (!a_seen) a_start = 1;
        if (!b_seen) b_start = 1;
        char header[128];
        std::snprintf(header, sizeof(header), "@@ -%zu,%zu +%zu,%zu @@\n", a_start, a_count, b_start, b_count);
        out += header;
        for (size_t k = start; k < end && k < ops.size(); ++k) {
            out += ops[k].kind;
            out += ops[k].text;
            out += "\n";
        }
        i = end > i ? end : i + 1;
        (void)trailing_context;
    }
    if (!before_nl || !after_nl) out += "\\ No newline at end of file\n";
    return out;
}

size_t diff_line_count(std::string_view text) {
    return count_lines(text);
}

// -----------------------------------------------------------------------------
// Patch application
// -----------------------------------------------------------------------------
Result<PatchOutcome> fs_apply_patch(const Workspace& ws, std::string_view patch, bool dry_run) {
    PatchOutcome outcome;
    bool is_block = contains(patch, "*** Begin Patch") || contains(patch, "*** Update File:") ||
                    contains(patch, "*** Add File:") || contains(patch, "*** Delete File:");

    auto finish = [&](void) -> Result<PatchOutcome> {
        if (!outcome.failures.empty()) {
            // Roll back nothing: individual edits are applied atomically, and the
            // caller sees exactly which files failed.
            outcome.applied = false;
        } else {
            outcome.applied = true;
        }
        return outcome;
    };

    if (is_block) {
        std::vector<BlockEdit> edits = parse_block_patch(patch);
        if (edits.empty()) return LCA_FAIL(Code::ParseError, "no file operations found in patch");
        for (const BlockEdit& edit : edits) {
            Result<std::string> abs = fs_resolve(ws, edit.path);
            if (!abs.ok()) { outcome.failures.push_back(edit.path + ": " + abs.error().message); continue; }
            if (edit.kind == BlockEdit::Kind::Add) {
                if (file_exists(*abs)) {
                    outcome.failures.push_back(edit.path + ": file already exists");
                    continue;
                }
                if (!dry_run) {
                    Result<WriteResult> w = fs_write(ws, edit.path, edit.content, true);
                    if (!w.ok()) { outcome.failures.push_back(edit.path + ": " + w.error().message); continue; }
                }
                outcome.files_changed.push_back(edit.path);
                outcome.added_lines += count_lines(edit.content);
                continue;
            }
            if (edit.kind == BlockEdit::Kind::Delete) {
                if (!file_exists(*abs)) {
                    outcome.failures.push_back(edit.path + ": no such file");
                    continue;
                }
                bool truncated = false;
                Result<std::string> data = read_whole_file(*abs, size_t(1u << 30), &truncated);
                if (!dry_run) {
                    Result<bool> del = fs_delete(ws, edit.path, true);
                    if (!del.ok()) { outcome.failures.push_back(edit.path + ": " + del.error().message); continue; }
                }
                outcome.files_changed.push_back(edit.path);
                if (data.ok()) outcome.removed_lines += count_lines(*data);
                continue;
            }
            // Update
            if (!file_exists(*abs)) {
                outcome.failures.push_back(edit.path + ": no such file");
                continue;
            }
            bool truncated = false;
            Result<std::string> data = read_whole_file(*abs, size_t(ws.cfg.max_read_bytes) * 4, &truncated);
            if (!data.ok()) { outcome.failures.push_back(edit.path + ": " + data.error().message); continue; }
            std::string text = *data;
            std::string why;
            Result<size_t> applied = apply_block_lines(text, edit.lines, &why);
            if (!applied.ok()) {
                outcome.failures.push_back(edit.path + ": " + why);
                continue;
            }
            for (const DiffOp& op : edit.lines) {
                if (op.kind == '+') ++outcome.added_lines;
                else if (op.kind == '-') ++outcome.removed_lines;
            }
            if (!dry_run) {
                Result<WriteResult> w = fs_write(ws, edit.path, text, true);
                if (!w.ok()) { outcome.failures.push_back(edit.path + ": " + w.error().message); continue; }
            }
            outcome.files_changed.push_back(edit.path);
        }
        return finish();
    }

    // Unified diff
    std::vector<UnifiedFile> files = parse_unified_diff(patch);
    if (files.empty()) return LCA_FAIL(Code::ParseError, "not a patch (no --- / +++ headers found)");
    for (const UnifiedFile& file : files) {
        std::string rel = strip_ab_prefix(file.new_path.empty() ? file.old_path : file.new_path);
        if (rel == "/dev/null") rel = strip_ab_prefix(file.old_path);
        Result<std::string> abs = fs_resolve(ws, rel);
        if (!abs.ok()) { outcome.failures.push_back(rel + ": " + abs.error().message); continue; }
        std::string original;
        if (file_exists(*abs)) {
            bool truncated = false;
            Result<std::string> data = read_whole_file(*abs, size_t(ws.cfg.max_read_bytes) * 4, &truncated);
            if (data.ok()) original = *data;
        } else if (file.new_path == "/dev/null") {
            outcome.failures.push_back(rel + ": no such file");
            continue;
        }
        std::string updated = original;
        std::string why;
        if (!apply_hunks(updated, file.hunks, 0, &why)) {
            outcome.failures.push_back(rel + ": " + why);
            continue;
        }
        if (!dry_run) {
            Result<WriteResult> w = fs_write(ws, rel, updated, true);
            if (!w.ok()) { outcome.failures.push_back(rel + ": " + w.error().message); continue; }
        }
        outcome.files_changed.push_back(rel);
        for (const Hunk& h : file.hunks) {
            for (const HunkLine& hl : h.lines) {
                if (hl.kind == '+') ++outcome.added_lines;
                else if (hl.kind == '-') ++outcome.removed_lines;
            }
        }
    }
    return finish();
}

// -----------------------------------------------------------------------------
// Project map
// -----------------------------------------------------------------------------
Json ProjectMap::to_json() const {
    Json j = Json::object();
    j["root"] = root;
    j["total_files"] = int64_t(total_files);
    j["total_dirs"] = int64_t(total_dirs);
    j["total_bytes"] = int64_t(total_bytes);
    Json exts = Json::object();
    for (const auto& kv : ext_counts) exts[kv.first] = int64_t(kv.second);
    j["extensions"] = exts;
    Json entries = Json::array();
    for (const std::string& e : entry_points) entries.push_back(Json(e));
    j["entry_points"] = entries;
    Json builds = Json::array();
    for (const std::string& b : build_files) builds.push_back(Json(b));
    j["build_files"] = builds;
    return j;
}

std::string ProjectMap::summary_text(size_t max_files) const {
    std::string out;
    out += "project: " + root + "\n";
    out += std::to_string(total_files) + " files, " + std::to_string(total_dirs) + " directories, " +
           human_bytes(total_bytes) + "\n";
    if (!entry_points.empty()) out += "entry points: " + join(entry_points, ", ") + "\n";
    if (!build_files.empty()) out += "build files : " + join(build_files, ", ") + "\n";
    std::vector<std::pair<size_t, std::string>> by_count;
    for (const auto& kv : ext_counts) by_count.push_back({kv.second, kv.first});
    std::sort(by_count.begin(), by_count.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    if (!by_count.empty()) {
        out += "languages   : ";
        size_t shown = 0;
        for (const auto& kv : by_count) {
            if (shown++ >= 6) break;
            out += (shown > 1 ? ", " : "");
            out += (kv.second.empty() ? "other" : kv.second) + " (" + std::to_string(kv.first) + ")";
        }
        out += "\n";
    }
    out += "\n";
    out += tree.empty() ? "" : tree;
    if (!files.empty() && files.size() > max_files) {
        out += "... " + std::to_string(files.size() - max_files) + " more files\n";
    }
    return out;
}

Result<ProjectMap> fs_project_map(const Workspace& ws, size_t max_files) {
    ProjectMap map;
    map.root = ws.root_abs;
    size_t limit = max_files ? max_files : ws.cfg.max_entries;
    WalkState st;
    std::vector<FileInfo> all;
    st.out = &all;
    st.limit = 0;              // no hard cap during mapping, but bounded below
    st.recursive = true;
    st.ws = &ws;
    walk_dir(st, ws.root_abs, 0);
    if (all.size() > limit * 4) all.resize(limit * 4);

    // Build the tree text.
    struct Node {
        std::string name;
        bool        dir{false};
        std::map<std::string, Node> children;
    };
    Node root;
    root.name = basename_of(ws.root_abs);
    root.dir = true;
    for (const FileInfo& info : all) {
        std::string rel = info.rel;
        if (rel == "." || rel.empty()) continue;
        std::vector<std::string> parts = split(rel, '/');
        Node* node = &root;
        for (size_t i = 0; i < parts.size(); ++i) {
            bool leaf = (i + 1 == parts.size());
            Node& child = node->children[parts[i]];
            if (child.name.empty()) {
                child.name = parts[i];
                child.dir = leaf ? info.is_dir : true;
            }
            node = &child;
        }
    }
    std::function<void(const Node&, int, std::string&, size_t&, size_t)> render =
        [&](const Node& node, int depth, std::string& out, size_t& shown, size_t max_show) {
            if (shown >= max_show) return;
            std::string indent(size_t(depth) * 2, ' ');
            for (const auto& kv : node.children) {
                if (shown >= max_show) {
                    out += indent + "...\n";
                    return;
                }
                const Node& child = kv.second;
                out += indent + child.name + (child.dir ? "/" : "") + "\n";
                ++shown;
                if (child.dir) render(child, depth + 1, out, shown, max_show);
            }
        };
    size_t shown = 0;
    render(root, 0, map.tree, shown, max_files);

    for (const FileInfo& info : all) {
        if (info.is_dir) { ++map.total_dirs; continue; }
        ++map.total_files;
        map.total_bytes += info.size;
        std::string ext = info.extension();
        map.ext_counts[ext] += 1;
        map.ext_bytes[ext] += info.size;
        if (looks_like_entry_point(info.rel)) map.entry_points.push_back(info.rel);
        if (looks_like_build_file(info.rel)) map.build_files.push_back(info.rel);
        if (map.files.size() < limit) map.files.push_back(info);
    }
    std::sort(map.entry_points.begin(), map.entry_points.end());
    std::sort(map.build_files.begin(), map.build_files.end());
    return map;
}

}  // namespace lca
