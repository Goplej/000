# ==============================================================================
#  local-claude-code-agent -- plain GNU Make fallback build
#
#  CMakeLists.txt is the primary build file; this Makefile exists so the agent
#  can be built on systems without CMake (it only needs g++/clang++ and make).
#
#  Targets:
#    make            build ./build/lca
#    make test       build and run every test suite
#    make install    install the binary to $(PREFIX)/bin (default ~/.local/bin)
#    make clean      remove ./build
# ==============================================================================

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude
LDFLAGS  += -pthread
PREFIX   ?= $(HOME)/.local

BUILD := build

CORE_SRC := \
	src/agent.cpp \
	src/buf.cpp \
	src/crypto.cpp \
	src/crypto_asym.cpp \
	src/fs_engine.cpp \
	src/mem.cpp \
	src/model.cpp \
	src/net.cpp \
	src/proc.cpp \
	src/search.cpp \
	src/tls.cpp \
	src/x509.cpp

CORE_OBJ := $(CORE_SRC:src/%.cpp=$(BUILD)/obj/%.o)

TESTS := test_crypto test_buf test_fs_engine test_search test_tls_fixture test_sandbox

.PHONY: all tests test install clean

all: $(BUILD)/lca

$(BUILD)/obj/%.o: src/%.cpp
	@mkdir -p $(BUILD)/obj
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/lca: src/main.cpp $(CORE_OBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_%: tests/test_%.cpp $(CORE_OBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Itests $^ -o $@ $(LDFLAGS)

tests: $(TESTS:%=$(BUILD)/%)

test: tests
	@fail=0; \
	for t in $(TESTS); do \
	  printf '%-18s ' $$t; \
	  if timeout 600 $(BUILD)/$$t > $(BUILD)/$$t.log 2>&1; then \
	    grep -E "passed|checks," $(BUILD)/$$t.log | tail -n 1; \
	  else \
	    echo "FAILED (see $(BUILD)/$$t.log)"; fail=1; \
	  fi; \
	done; \
	exit $$fail

install: $(BUILD)/lca
	install -d $(PREFIX)/bin
	install -m 0755 $(BUILD)/lca $(PREFIX)/bin/lca
	@echo "installed $(PREFIX)/bin/lca"

clean:
	rm -rf $(BUILD)
