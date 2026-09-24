// =============================================================================
//  lca/fs_engine.h  --  autonomous file engine
// -----------------------------------------------------------------------------
//  Everything the agent does to the outside world through the file system goes
//  through this module.  It is deliberately sandboxed: every path is resolved
//  against a workspace root, symlink escapes are rejected, writes are atomic
//  (temp file + rename) and optionally backed up, and every bulk operation is
//  charged against the MemoryGuard budget so a runaway glob cannot exhaust RAM.
//
//  Capabilities
//    * read / write / append / delete / move / copy / mkdir
//    * directory listing, recursive walk with ignore rules
//    * glob matching (**, *, ?, [..], {a,b}) and grep (literal or regex)
//    * structured edits (exact replace, line insert/delete, file create)
//    * unified diff generation and patch application (unified + block format)
//    * project mapping: tree view, per-extension statistics, entry points
//    * backups with restore
// =============================================================================
#ifndef LCA_FS_ENGINE_H
#define LCA_FS_ENGINE_H

#include "lca/common.h"
#include "lca/mem.h"

#include <map>

namespace lca {

// -----------------------------------------------------------------------------
// Workspace
// -----------------------------------------------------------------------------
struct WorkspaceConfig {
    std::string root{"."};
    bool        allow_outside_root{false};   // when true the sandbox is advisory
    bool        create_backups{true};        // keep .lca-backup copies before writes
    uint64_t    max_read_bytes{32ull * 1024 * 1024};
    size_t      max_entries{20000};          // cap for walks / globs
    bool        follow_symlinks{false};
    std::vector<std::string> ignore_dirs{
        ".git", ".hg", ".svn", "node_modules", "vendor", "build", "dist", "out",
        "target", ".venv", "venv", "__pycache__", ".mypy_cache", ".pytest_cache",
        ".cache", ".idea", ".vscode", "cmake-build-debug", "cmake-build-release",
        ".next", ".nuxt", ".svelte-kit", ".gradle", ".terraform"};
    // Files matching these suffixes are agent bookkeeping, not project content:
    // they are hidden from listings, globs, greps and the project map.
    std::vector<std::string> ignore_suffixes{".lca-backup-", ".lca-tmp-"};
    std::vector<std::string> text_extensions{
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hxx", ".ipp", ".inc",
        ".m", ".mm", ".rs", ".go", ".py", ".pyi", ".js", ".mjs", ".cjs", ".ts",
        ".tsx", ".jsx", ".java", ".kt", ".kts", ".scala", ".cs", ".fs", ".vb",
        ".rb", ".php", ".pl", ".lua", ".sh", ".bash", ".zsh", ".fish", ".ps1",
        ".sql", ".html", ".htm", ".css", ".scss", ".sass", ".less", ".xml",
        ".json", ".jsonc", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
        ".md", ".markdown", ".rst", ".txt", ".tex", ".csv", ".tsv", ".log",
        ".cmake", ".mk", ".make", ".dockerfile", ".env", ".properties", ".proto",
        ".graphql", ".tf", ".hcl", ".nix", ".vim", ".el", ".clj", ".ex", ".exs"};
};

struct Workspace {
    WorkspaceConfig cfg;
    std::string     root_abs;      // canonical absolute root, no trailing slash
    bool            absolute_paths{true};
};

// Opens (and validates) the workspace.  The root is created when missing.
Result<Workspace> workspace_open(const WorkspaceConfig& cfg);

// Resolves `path` inside the workspace.  Relative paths are taken from the
// root; absolute paths are allowed only when they stay inside it (or when
// allow_outside_root is set).  Returns a canonical absolute path.
Result<std::string> fs_resolve(const Workspace& ws, std::string_view path);

// Paths relative to the workspace root (for display/logging).
std::string fs_relative(const Workspace& ws, const std::string& abs_path);

// -----------------------------------------------------------------------------
// File metadata and iteration
// -----------------------------------------------------------------------------
struct FileInfo {
    std::string path;         // absolute
    std::string rel;          // relative to workspace root
    std::string name;         // basename
    bool        is_dir{false};
    bool        is_symlink{false};
    bool        is_text{false};
    uint64_t    size{0};
    int64_t     mtime_ms{0};
    std::string extension() const;   // lower-case, including the dot
};

Result<FileInfo>              fs_stat(const Workspace& ws, std::string_view path);
Result<std::vector<FileInfo>> fs_list(const Workspace& ws, std::string_view dir,
                                      bool recursive = false, size_t max_entries = 0);
Result<std::vector<FileInfo>> fs_glob(const Workspace& ws, std::string_view pattern,
                                      size_t max_results = 0);

// Glob helper: supports *, ?, **, [abc], [!abc] and {a,b} alternatives.
bool glob_match(std::string_view pattern, std::string_view text);

// -----------------------------------------------------------------------------
// Reading and writing
// -----------------------------------------------------------------------------
struct ReadResult {
    std::string  text;
    FileInfo     info;
    bool         truncated{false};
    size_t       total_bytes{0};
    size_t       shown_bytes{0};
    size_t       line_count{0};
    std::string  language_hint;      // best-effort, used by the prompt builder
};

Result<ReadResult> fs_read(const Workspace& ws, std::string_view path, size_t max_bytes = 0);

struct WriteResult {
    std::string path;
    size_t      bytes_written{0};
    bool        created{false};
    std::string backup_path;
    std::string diff;
};

Result<WriteResult> fs_write(const Workspace& ws, std::string_view path, std::string_view content,
                             bool create_parents = true);
Result<WriteResult> fs_append(const Workspace& ws, std::string_view path, std::string_view content,
                              bool create_parents = true);

Result<bool> fs_mkdirs(const Workspace& ws, std::string_view path);
Result<bool> fs_delete(const Workspace& ws, std::string_view path, bool recursive = false);
Result<bool> fs_move(const Workspace& ws, std::string_view from, std::string_view to);
Result<bool> fs_copy(const Workspace& ws, std::string_view from, std::string_view to);

// Backups ---------------------------------------------------------------
Result<std::string> fs_backup(const Workspace& ws, std::string_view path);
Result<bool>        fs_restore(const Workspace& ws, std::string_view path, std::string_view backup_path);
std::vector<std::string> fs_list_backups(const Workspace& ws, std::string_view path);

// -----------------------------------------------------------------------------
// Search
// -----------------------------------------------------------------------------
struct GrepHit {
    std::string rel;
    size_t      line{0};
    std::string text;
    bool        is_match{true};     // false for context lines
};

struct GrepOptions {
    bool        regex{false};
    bool        ignore_case{true};
    bool        include_binary{false};
    std::string include_glob{"*"};      // e.g. "*.cpp"
    std::string exclude_glob;
    size_t      max_hits{200};
    int         context_lines{0};
    size_t      max_file_bytes{4u << 20};
};

struct GrepSummary {
    std::vector<GrepHit> hits;
    size_t files_scanned{0};
    size_t files_matched{0};
    bool   truncated{false};
};

Result<GrepSummary> fs_grep(const Workspace& ws, std::string_view pattern, const GrepOptions& opts = {});

// -----------------------------------------------------------------------------
// Structured edits
// -----------------------------------------------------------------------------
struct EditRequest {
    enum class Op {
        ReplaceExact,      // `find` must occur exactly once
        ReplaceAll,        // replaces every occurrence
        ReplaceLines,      // replace the [line, end_line] range
        InsertBeforeLine,  // insert `content` before `line` (1-based)
        InsertAfterLine,   // insert `content` after `line`
        DeleteLines,       // delete the [line, end_line] range
        CreateFile,        // create `path` with `content`
        DeleteFile,
    };
    Op          op{Op::ReplaceExact};
    std::string path;
    std::string find;
    std::string replace;
    std::string content;
    size_t      line{0};
    size_t      end_line{0};
    bool        dry_run{false};
};

struct EditOutcome {
    std::string path;
    std::string diff;
    size_t      replacements{0};
    size_t      bytes_before{0};
    size_t      bytes_after{0};
    bool        applied{false};
};

Result<EditOutcome> fs_edit(const Workspace& ws, const EditRequest& req);

// -----------------------------------------------------------------------------
// Diffs and patches
// -----------------------------------------------------------------------------
std::string unified_diff(std::string_view before, std::string_view after,
                         const std::string& label, int context = 3);

// Unified-diff size estimate without building the diff (used for budgeting).
size_t diff_line_count(std::string_view text);

struct PatchOutcome {
    std::vector<std::string> files_changed;
    std::vector<std::string> failures;
    size_t added_lines{0};
    size_t removed_lines{0};
    bool   applied{false};
};

// Applies either a unified diff or a block patch of the form
//   *** Begin Patch
//   *** Update File: src/x.cpp
//   @@
//   -old
//   +new
//   *** Add File: src/y.cpp
//   +content
//   *** Delete File: src/z.cpp
//   *** End Patch
Result<PatchOutcome> fs_apply_patch(const Workspace& ws, std::string_view patch, bool dry_run = false);

// -----------------------------------------------------------------------------
// Project mapping
// -----------------------------------------------------------------------------
struct ProjectMap {
    std::string              root;
    std::vector<FileInfo>    files;
    std::map<std::string, size_t>   ext_counts;
    std::map<std::string, uint64_t> ext_bytes;
    uint64_t                 total_bytes{0};
    size_t                   total_files{0};
    size_t                   total_dirs{0};
    std::string              tree;             // indented, human readable
    std::vector<std::string> entry_points;     // main.cpp, main.py, index.js, ...
    std::vector<std::string> build_files;      // CMakeLists.txt, Makefile, package.json
    std::map<std::string, std::vector<std::string>> symbols;   // best-effort per file

    Json        to_json() const;
    std::string summary_text(size_t max_files = 120) const;
};

Result<ProjectMap> fs_project_map(const Workspace& ws, size_t max_files = 0);

// Cheap heuristic: which files in the project are entry points / build files.
bool looks_like_entry_point(std::string_view rel);
bool looks_like_build_file(std::string_view rel);
std::string language_for_path(std::string_view path);

// -----------------------------------------------------------------------------
// Small helpers shared with the rest of the agent
// -----------------------------------------------------------------------------
bool file_exists(const std::string& abs_path);
bool is_directory(const std::string& abs_path);
int64_t file_mtime_ms(const std::string& abs_path);
std::string absolute_path(const std::string& path);
std::string directory_of(const std::string& path);
std::string basename_of(const std::string& path);
std::string parent_of(const std::string& path);

}  // namespace lca

#endif  // LCA_FS_ENGINE_H
