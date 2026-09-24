// =============================================================================
//  tests/test_fs_engine.cpp  --  sandboxed file engine
// =============================================================================
#include "lca/fs_engine.h"
#include "test_util.h"

#include <cstdio>
#include <fstream>

using namespace lca;
using lca_test::section;

namespace {

std::string g_root;

Workspace open_ws() {
    WorkspaceConfig cfg;
    cfg.root = g_root;
    auto ws = workspace_open(cfg);
    if (!ws.ok()) {
        std::fprintf(stderr, "cannot open workspace: %s\n", ws.error().str().c_str());
        std::exit(2);
    }
    return *ws;
}

void test_workspace_sandbox(const Workspace& ws) {
    section("workspace + sandbox");
    LCA_CHECK(is_directory(ws.root_abs));
    auto inside = fs_resolve(ws, "sub/dir/file.txt");
    LCA_CHECK(inside.ok());
    auto escape = fs_resolve(ws, "../outside.txt");
    LCA_CHECK(!escape.ok());
    LCA_STREQ(code_name(escape.error().code), "SandboxViolation");
    auto absolute_inside = fs_resolve(ws, ws.root_abs + "/x.txt");
    LCA_CHECK(absolute_inside.ok());
    auto absolute_outside = fs_resolve(ws, "/etc/passwd");
    LCA_CHECK(!absolute_outside.ok());
}

void test_write_read(const Workspace& ws) {
    section("write / read / append");
    auto w = fs_write(ws, "src/hello.txt", "line one\nline two\n");
    LCA_CHECK(w.ok());
    LCA_CHECK(w->created);

    auto r = fs_read(ws, "src/hello.txt");
    LCA_CHECK(r.ok());
    LCA_STREQ(r->text, "line one\nline two\n");
    LCA_EQ(r->line_count, size_t(2));
    LCA_STREQ(r->language_hint, "text");

    auto w2 = fs_write(ws, "src/hello.txt", "line one\nline two changed\n");
    LCA_CHECK(w2.ok());
    LCA_CHECK(!w2->created);
    LCA_CHECK(!w2->backup_path.empty());
    LCA_CHECK(!w2->diff.empty());
    LCA_CHECK(w2->diff.find("-line two") != std::string::npos);

    auto a = fs_append(ws, "src/hello.txt", "line three\n");
    LCA_CHECK(a.ok());
    auto r2 = fs_read(ws, "src/hello.txt");
    LCA_CHECK(r2.ok());
    LCA_EQ(r2->line_count, size_t(3));

    // nested path outside the workspace must be refused
    LCA_CHECK(!fs_write(ws, "../evil.txt", "nope").ok());
    // binary content must not be returned as text
    std::string binary = std::string("abc\0def", 7);
    LCA_CHECK(fs_write(ws, "src/bin.dat", binary).ok());
    LCA_CHECK(!fs_read(ws, "src/bin.dat").ok());
}

void test_listing(const Workspace& ws) {
    section("listing / glob");
    fs_write(ws, "src/a.cpp", "int main() { return 0; }\n");
    fs_write(ws, "src/b.hpp", "#pragma once\n");
    fs_write(ws, "docs/notes.md", "# notes\n");
    fs_write(ws, "node_modules/ignored.js", "// should be skipped\n");

    auto list = fs_list(ws, ".", false, 0);
    LCA_CHECK(list.ok());
    bool saw_src = false;
    for (const FileInfo& f : *list) if (f.rel == "src" && f.is_dir) saw_src = true;
    LCA_CHECK(saw_src);

    auto recursive = fs_list(ws, ".", true, 0);
    LCA_CHECK(recursive.ok());
    bool saw_ignored = false, saw_notes = false;
    for (const FileInfo& f : *recursive) {
        if (f.rel.find("node_modules") != std::string::npos) saw_ignored = true;
        if (f.rel == "docs/notes.md") saw_notes = true;
    }
    LCA_CHECK(saw_notes);
    LCA_CHECK(!saw_ignored);        // ignore_dirs is honoured

    auto cpp = fs_glob(ws, "**/*.cpp", 0);
    LCA_CHECK(cpp.ok());
    LCA_CHECK(cpp->size() == 1);
    auto any = fs_glob(ws, "src/*.{hpp,cpp}", 0);
    LCA_CHECK(any.ok());
    LCA_EQ(any->size(), size_t(2));

    LCA_CHECK(glob_match("**/*.cpp", "a/b/c.cpp"));
    LCA_CHECK(!glob_match("*.cpp", "a/b/c.cpp"));
    LCA_CHECK(glob_match("src/?.cpp", "src/a.cpp"));
    LCA_CHECK(glob_match("[ab]*.txt", "b1.txt"));
    LCA_CHECK(!glob_match("[!ab]*.txt", "b1.txt"));
}

void test_grep(const Workspace& ws) {
    section("grep");
    GrepOptions opts;
    opts.max_hits = 50;
    auto hits = fs_grep(ws, "main", opts);
    LCA_CHECK(hits.ok());
    LCA_CHECK(hits->hits.size() >= 1);
    LCA_CHECK(hits->files_matched >= 1);

    GrepOptions regex_opts;
    regex_opts.regex = true;
    regex_opts.max_hits = 50;
    auto regex_hits = fs_grep(ws, "int\\s+main", regex_opts);
    LCA_CHECK(regex_hits.ok());
    LCA_CHECK(regex_hits->hits.size() >= 1);

    GrepOptions include_opts;
    include_opts.include_glob = "*.hpp";
    auto included = fs_grep(ws, "pragma", include_opts);
    LCA_CHECK(included.ok());
    LCA_EQ(included->files_matched, size_t(1));

    GrepOptions bad_regex;
    bad_regex.regex = true;
    LCA_CHECK(!fs_grep(ws, "([unclosed", bad_regex).ok());
}

void test_edits(const Workspace& ws) {
    section("structured edits");
    fs_write(ws, "edit.txt", "alpha\nbeta\ngamma\n");

    EditRequest req;
    req.path = "edit.txt";
    req.op = EditRequest::Op::ReplaceExact;
    req.find = "beta";
    req.replace = "BETA";
    auto applied = fs_edit(ws, req);
    LCA_CHECK(applied.ok());
    LCA_CHECK(applied->replacements == 1);
    auto after = fs_read(ws, "edit.txt");
    LCA_CHECK(after.ok());
    LCA_STREQ(after->text, "alpha\nBETA\ngamma\n");
    LCA_CHECK(applied->diff.find("+BETA") != std::string::npos);

    // ambiguous replace must be refused
    fs_write(ws, "edit2.txt", "x\nx\n");
    EditRequest ambiguous;
    ambiguous.path = "edit2.txt";
    ambiguous.find = "x";
    ambiguous.replace = "y";
    LCA_CHECK(!fs_edit(ws, ambiguous).ok());
    ambiguous.op = EditRequest::Op::ReplaceAll;
    auto all = fs_edit(ws, ambiguous);
    LCA_CHECK(all.ok());
    LCA_EQ(all->replacements, size_t(2));

    // line based edits
    fs_write(ws, "edit3.txt", "one\ntwo\nthree\n");
    EditRequest insert;
    insert.path = "edit3.txt";
    insert.op = EditRequest::Op::InsertAfterLine;
    insert.line = 1;
    insert.content = "inserted";
    insert.replace = "inserted";
    LCA_CHECK(fs_edit(ws, insert).ok());
    EditRequest del;
    del.path = "edit3.txt";
    del.op = EditRequest::Op::DeleteLines;
    del.line = 3;      // removes "two", leaving one/inserted/three
    del.end_line = 3;
    LCA_CHECK(fs_edit(ws, del).ok());
    auto lines = fs_read(ws, "edit3.txt");
    LCA_CHECK(lines.ok());
    LCA_STREQ(lines->text, "one\ninserted\nthree\n");

    // dry run must not touch the file
    EditRequest dry;
    dry.path = "edit3.txt";
    dry.find = "three";
    dry.replace = "FOUR";
    dry.dry_run = true;
    auto dry_result = fs_edit(ws, dry);
    LCA_CHECK(dry_result.ok());
    LCA_CHECK(!dry_result->applied);
    auto unchanged = fs_read(ws, "edit3.txt");
    LCA_CHECK(unchanged.ok());
    LCA_CHECK(unchanged->text.find("three") != std::string::npos);

    // create + delete file
    EditRequest create;
    create.path = "made.txt";
    create.op = EditRequest::Op::CreateFile;
    create.content = "made\n";
    LCA_CHECK(fs_edit(ws, create).ok());
    LCA_CHECK(file_exists(ws.root_abs + "/made.txt"));
    EditRequest remove;
    remove.path = "made.txt";
    remove.op = EditRequest::Op::DeleteFile;
    LCA_CHECK(fs_edit(ws, remove).ok());
    LCA_CHECK(!file_exists(ws.root_abs + "/made.txt"));
}

void test_patch(const Workspace& ws) {
    section("diffs and patches");
    std::string before = "one\ntwo\nthree\nfour\nfive\n";
    std::string after  = "one\nTWO\nthree\nfour\nfive\nsix\n";
    std::string diff = unified_diff(before, after, "patch.txt", 1);
    LCA_CHECK(diff.find("--- a/patch.txt") != std::string::npos);
    LCA_CHECK(diff.find("+++ b/patch.txt") != std::string::npos);
    LCA_CHECK(diff.find("@@") != std::string::npos);

    fs_write(ws, "patch.txt", before);
    auto applied = fs_apply_patch(ws, diff, false);
    LCA_CHECK(applied.ok());
    LCA_CHECK_MSG(applied->failures.empty(),
                  applied->failures.empty() ? "" : applied->failures.front());
    auto now = fs_read(ws, "patch.txt");
    LCA_CHECK(now.ok());
    LCA_STREQ(now->text, after);

    // block patch: add + update + delete
    const std::string block =
        "*** Begin Patch\n"
        "*** Add File: added/block.txt\n"
        "+first line\n"
        "+second line\n"
        "*** Update File: patch.txt\n"
        "@@\n"
        " three\n"
        "-four\n"
        "+FOUR\n"
        " five\n"
        "*** Delete File: src/bin.dat\n"
        "*** End Patch\n";
    auto block_result = fs_apply_patch(ws, block, false);
    LCA_CHECK(block_result.ok());
    LCA_CHECK_MSG(block_result->failures.empty(),
                  block_result->failures.empty() ? "" : block_result->failures.front());
    auto added = fs_read(ws, "added/block.txt");
    LCA_CHECK(added.ok());
    LCA_STREQ(added->text, "first line\nsecond line\n");
    auto updated = fs_read(ws, "patch.txt");
    LCA_CHECK(updated.ok());
    LCA_CHECK(updated->text.find("FOUR") != std::string::npos);

    // a patch that does not apply must fail without corrupting anything
    const std::string bad = "--- a/patch.txt\n+++ b/patch.txt\n@@ -1,2 +1,2 @@\n-zzz\n+yyy\n";
    auto bad_result = fs_apply_patch(ws, bad, false);
    LCA_CHECK(bad_result.ok());
    LCA_CHECK(!bad_result->failures.empty());

    // dry run
    const std::string dry_patch = "*** Begin Patch\n*** Add File: dry/only.txt\n+content\n*** End Patch\n";
    auto dry = fs_apply_patch(ws, dry_patch, true);
    LCA_CHECK(dry.ok());
    LCA_CHECK(!file_exists(ws.root_abs + "/dry/only.txt"));
}

void test_project_map(const Workspace& ws) {
    section("project map");
    fs_write(ws, "CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\n");
    fs_write(ws, "main.cpp", "int main() { return 0; }\n");
    fs_write(ws, "lib/util.cpp", "int util() { return 1; }\n");
    auto map = fs_project_map(ws, 50);
    LCA_CHECK(map.ok());
    LCA_CHECK(map->total_files >= 4);
    LCA_CHECK(map->ext_counts.count(".cpp") == 1);
    bool has_build = false, has_entry = false;
    for (const std::string& b : map->build_files) if (b == "CMakeLists.txt") has_build = true;
    for (const std::string& e : map->entry_points) if (e == "main.cpp") has_entry = true;
    LCA_CHECK(has_build);
    LCA_CHECK(has_entry);
    LCA_CHECK(!map->tree.empty());
    std::string summary = map->summary_text(50);
    LCA_CHECK(summary.find("project:") != std::string::npos);
    Json j = map->to_json();
    LCA_CHECK(j["total_files"].as_int() > 0);

    LCA_CHECK(looks_like_entry_point("src/main.cpp"));
    LCA_CHECK(looks_like_build_file("CMakeLists.txt"));
    LCA_STREQ(language_for_path("a/b/main.cpp"), "cpp");
    LCA_STREQ(language_for_path("x.py"), "python");
}

void test_backup_restore(const Workspace& ws) {
    section("backups");
    fs_write(ws, "backup.txt", "original\n");
    auto backup = fs_backup(ws, "backup.txt");
    LCA_CHECK(backup.ok());
    fs_write(ws, "backup.txt", "changed\n");
    auto restored = fs_restore(ws, "backup.txt", fs_relative(ws, *backup));
    LCA_CHECK(restored.ok());
    auto content = fs_read(ws, "backup.txt");
    LCA_CHECK(content.ok());
    LCA_STREQ(content->text, "original\n");
    LCA_CHECK(!fs_list_backups(ws, "backup.txt").empty());
}

void test_moves(const Workspace& ws) {
    section("copy / move / delete");
    fs_write(ws, "mv/a.txt", "payload\n");
    LCA_CHECK(fs_copy(ws, "mv/a.txt", "mv/b.txt").ok());
    LCA_CHECK(fs_read(ws, "mv/b.txt").ok());
    LCA_CHECK(fs_move(ws, "mv/b.txt", "mv/c.txt").ok());
    LCA_CHECK(!file_exists(ws.root_abs + "/mv/b.txt"));
    LCA_CHECK(file_exists(ws.root_abs + "/mv/c.txt"));
    LCA_CHECK(fs_delete(ws, "mv/c.txt", false).ok());
    LCA_CHECK(!fs_delete(ws, "..", true).ok());
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("lca fs_engine tests\n===================\n");
    if (argc > 1) g_root = argv[1];
    if (g_root.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        g_root = std::string(tmp && *tmp ? tmp : "/tmp") + "/lca-fs-tests-" +
                 std::to_string(int64_t(wall_millis()));
    }
    Workspace ws = open_ws();
    test_workspace_sandbox(ws);
    test_write_read(ws);
    test_listing(ws);
    test_grep(ws);
    test_edits(ws);
    test_patch(ws);
    test_project_map(ws);
    test_backup_restore(ws);
    test_moves(ws);
    return lca_test::finish("test_fs_engine");
}
