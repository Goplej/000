// =============================================================================
//  tests/test_util.h  --  minimal assertion helpers shared by the test binaries
// -----------------------------------------------------------------------------
//  No test framework: a counter, a few macros and a summary.  Each test binary
//  returns a non-zero exit code when something fails, so `ctest` and plain
//  shell scripts can both drive them.
// =============================================================================
#ifndef LCA_TEST_UTIL_H
#define LCA_TEST_UTIL_H

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace lca_test {

inline int& failures() { static int n = 0; return n; }
inline int& checks()   { static int n = 0; return n; }

inline void report(bool ok, const std::string& what, const std::string& detail = {}) {
    ++checks();
    if (ok) {
        std::printf("  [ok]   %s\n", what.c_str());
        return;
    }
    ++failures();
    std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  --  ",
                detail.c_str());
}

inline void section(const std::string& name) {
    std::printf("\n%s\n%s\n", name.c_str(), std::string(name.size(), '-').c_str());
}

inline int finish(const char* suite) {
    std::printf("\n%s: %d checks, %d failed\n", suite, checks(), failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace lca_test

#define LCA_CHECK(cond) ::lca_test::report((cond), #cond)
#define LCA_CHECK_MSG(cond, msg) ::lca_test::report((cond), #cond, (msg))
#define LCA_EQ(a, b)                                                                     \
    do {                                                                                 \
        auto _a = (a);                                                                   \
        auto _b = (b);                                                                   \
        ::lca_test::report(_a == _b, #a " == " #b,                                       \
                           _a == _b ? std::string()                                      \
                                    : std::string("got '") + std::to_string(_a) + "'");  \
    } while (0)
#define LCA_STREQ(a, b)                                                                  \
    do {                                                                                 \
        std::string _a = (a);                                                            \
        std::string _b = (b);                                                            \
        ::lca_test::report(_a == _b, #a " == " #b,                                       \
                           _a == _b ? std::string()                                      \
                                    : ("got '" + _a + "', want '" + _b + "'"));          \
    } while (0)

#endif  // LCA_TEST_UTIL_H
