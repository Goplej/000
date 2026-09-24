cmake_minimum_required(VERSION 3.13)
project(local-claude-code-agent
        VERSION 1.0.0
        DESCRIPTION "Autonomous local coding agent: file engine, web search/scraping, execution sandbox, hard memory caps"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LCA_BUILD_TESTS "Build the test suite" ON)
option(LCA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Strict but portable warning set; the codebase builds clean with these.
if(MSVC)
    add_compile_options(/W4)
    if(LCA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra)
    if(LCA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# -----------------------------------------------------------------------------
# Core library: every module except the CLI entry point.
#
# The whole agent is self-contained C++17 with explicit memory management.
# There are NO third-party dependencies: crypto (SHA-2, HMAC, HKDF, ECDSA,
# RSA, X25519), TLS 1.3, X.509, HTTP, gzip/deflate, JSON and HTML parsing are
# all implemented in-tree under src/.
# -----------------------------------------------------------------------------
add_library(lca_core STATIC
    src/agent.cpp
    src/buf.cpp
    src/crypto.cpp
    src/crypto_asym.cpp
    src/fs_engine.cpp
    src/mem.cpp
    src/model.cpp
    src/net.cpp
    src/proc.cpp
    src/search.cpp
    src/tls.cpp
    src/x509.cpp
)

target_include_directories(lca_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# POSIX threads for the sandbox supervisor and the memory guard.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(lca_core PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# CLI binary
# -----------------------------------------------------------------------------
add_executable(lca src/main.cpp)
target_link_libraries(lca PRIVATE lca_core)
set_target_properties(lca PROPERTIES OUTPUT_NAME "lca")

# -----------------------------------------------------------------------------
# Tests (ctest). Suites that need `openssl` or `python3` skip themselves at
# runtime when the tool is missing, so `ctest` stays green on minimal systems.
# -----------------------------------------------------------------------------
if(LCA_BUILD_TESTS)
    enable_testing()

    function(lca_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_include_directories(${name} PRIVATE tests)
        target_link_libraries(${name} PRIVATE lca_core)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 600)
    endfunction()

    lca_add_test(test_crypto)
    lca_add_test(test_buf)
    lca_add_test(test_fs_engine)
    lca_add_test(test_search)
    lca_add_test(test_tls_fixture)
    lca_add_test(test_sandbox)
endif()

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS lca RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/lca DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
