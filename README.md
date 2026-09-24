# local-claude-code-agent

A complete, monolithic, **autonomous coding agent** in self-contained C++17 —
a local Claude-Code-style tool that reads and restructures multi-file projects,
searches and scrapes the live web, executes commands in a hardened sandbox and
self-corrects until the task is done. **Zero paid APIs. Zero external
frameworks. Zero third-party libraries.** Every line of crypto, TLS, HTTP,
JSON, HTML and process supervision is implemented in this repository.

```
local-claude-code-agent 1.0.0 (Ryzen)
```

---

## Capabilities

| Area | Module | What it does |
|------|--------|--------------|
| **Autonomous file engine** | `include/lca/fs_engine.h`, `src/fs_engine.cpp` | Sandboxed read / write / exact-edit / unified-diff patch / move / list / glob / grep / project-map over a workspace root. Path escape is refused, every mutation can keep a `.lca-backup-<ts>` copy, edits are exact-match (an ambiguous find fails instead of guessing) and patches apply with fuzz = 0 (a hunk either matches exactly or is reported). |
| **Web search & scraping** | `include/lca/search.h`, `src/search.cpp`, `include/lca/net.h`, `src/net.cpp` | Raw-socket DNS + TCP + HTTP/1.1 + TLS 1.3 client (all hand-rolled: SHA-2, HMAC, HKDF, X25519, ECDSA, RSA, X.509 chain building, gzip/deflate). Queries DuckDuckGo / Bing / Wikipedia / Google / Mojeek, parses result HTML, obeys robots.txt, follows redirects, extracts page text and finds keyword snippets. |
| **Embedded execution sandbox** | `include/lca/proc.h`, `src/proc.cpp`, `include/lca/agent.h`, `src/agent.cpp` | Supervised shell with wall-clock timeouts, output caps, `RLIMIT_AS` / `RLIMIT_FSIZE` / `RLIMIT_NPROC`, per-policy command allow/deny lists, captured stdout/stderr and resource usage. The agent loop runs tools, observes results, re-plans and re-runs verification until green. |
| **Hardware-bounded memory** | `include/lca/mem.h`, `src/mem.cpp` | Detects the machine (CPU / RAM / GPU), derives hard budgets that stay **strictly below 8 GiB RAM and 4 GiB VRAM**, applies `RLIMIT_AS` to the whole process tree and refuses reservations above budget. |

Target hardware: **AMD Ryzen 5 7000 series, NVIDIA RTX 2050 (4 GiB), 8 GB RAM.**
Defaults: RAM budget = min(hardware, 6 GiB) × 0.8 safety — on an 8 GB machine
that is ≈ 3.1 GiB; VRAM budget = min(hardware, 3 GiB) × 0.8. The process
address-space limit is budget + 12.5 % and is always clamped below the 8 GiB /
4 GiB envelope. `lca doctor` prints and verifies all of it.

---

## Requirements

* C++17 compiler (g++ ≥ 9, clang++ ≥ 10) and GNU Make — **or** CMake ≥ 3.13
* POSIX system (Linux; macOS should work)
* Optional at runtime: `python3` and `openssl` (test fixtures skip without them),
  a local OpenAI-compatible model server (e.g. llama.cpp `server`) for the
  `local` brain — the built-in heuristic planner needs nothing

No OpenSSL development headers are required: the TLS 1.3 stack is in-tree and
never links `-lssl` / `-lcrypto`.

---

## Build

### Option A — installation script (recommended)

```sh
git clone https://github.com/Goplej/000.git && cd 000
./scripts/install.sh                # builds Release and installs to ~/.local/bin
```

Use `PREFIX=/opt/lca ./scripts/install.sh` for a different install root, or
`./scripts/install.sh --build-only` to only produce `./build/lca`.

### Option B — CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build               # optional: installs `lca` + headers
```

### Option C — plain Make (no CMake needed)

```sh
make -j
make install                        # optional: installs to ~/.local/bin
```

Either way the binary is `build/lca` (or `./build/lca` with Make).

---

## Quick start

```sh
lca doctor                                   # hardware + memory envelope + toolchain
lca run  "create a python project called hello_cli"
lca run  "fix the failing tests and make the build green" --verify "make test"
lca chat "explain what this repository does"

# Direct tool surface (same engine the agent drives):
lca fetch https://example.com                 # raw-socket HTTP(S) fetch
lca search "cpp reference vector"             # web search via HTML scrapers
lca exec "make -j2"                           # sandboxed command
lca ls . && lca glob "**/*.cpp" && lca grep "TODO" -r
lca read src/main.cpp
lca write notes.txt "hello world"
lca edit notes.txt hello helloworld           # exact find -> replace
lca patch fix.patch                           # unified diff, fuzz = 0
lca map                                       # project map (entry points, build files)
```

Run `lca --help` for the full option list (`--root`, `--brain`, `--policy`,
`--max-steps`, `--timeout`, `--max-ram`, `--max-vram`, `--verify`, `--no-network`,
`--dry-run`, `--confirm`, `--json`, `--config`, …).

### Autonomy loop

`lca run "…"` maps the workspace, plans, calls tools (13 of them: `project_map`,
`fs_read`, `fs_write`, `fs_edit`, `fs_patch`, `fs_list`, `fs_glob`, `fs_grep`,
`run_command`, `web_search`, `fetch_page`, `finish`, `follow_up`), runs an
inferred verification command (CMake → npm → cargo → go → python → make → meson
→ maven → gradle → per-language syntax sweep) after every edit and again before
finishing, and re-plans from the failures. The run stops on success, on the step
budget, or after the same tool call repeats three times.

### Brains

* `--brain heuristic` — deterministic built-in planner (create / fix / explain /
  run intents, template scaffolds for C++ and Python). No network, no model.
* `--brain local` — talks to any OpenAI-compatible server
  (`--model-endpoint http://127.0.0.1:8080`, e.g. llama.cpp `server -m model.gguf`).
  Nothing leaves your machine; no API keys, no paid services.
* `--brain auto` (default) — probes the local endpoint and falls back to heuristic.

---

## Memory envelope

```
$ lca doctor
memory envelope
---------------
hardware
--------
cpu            : AMD Ryzen 5 7640HS ...
ram            : 7.21 GiB total, ...
gpu            : NVIDIA GeForce RTX 2050, 4 GiB

budgets
-------
ram budget     : 3.08 GiB
vram budget    : 3.00 GiB
rlimit AS      : 3.46 GiB
within <8GB/<4GB envelope: yes
```

Every child process inherits the address-space limit; the agent refuses
allocations that would breach the budget (`rejected alloc` counter), and
`--max-ram` / `--max-vram` can only lower the caps, never raise them above the
hardware envelope.

---

## Sandbox policies

| Policy | Meaning |
|--------|---------|
| `developer` (default) | Deny-list: dangerous commands (`rm -rf /`, fork bombs, raw `dd`, `mkfs`, …) are refused; everything else runs under the rlimits. |
| `strict` | Default-deny: only build/test/edit-adjacent command shapes run. |
| `permissive` | Advisory only; rlimits and timeouts still apply. |

`--confirm` prompts before every shell command; `--no-network` disables the
network tools entirely.

---

## Tests

```sh
./scripts/run_tests.sh        # or: make test   /   ctest --test-dir build
```

464 assertions across six suites:

| Suite | Checks | Covers |
|-------|--------|--------|
| `test_crypto` | 32 | SHA-256/384/512 KATs, HMAC, HKDF, base64/hex, CSPRNG, BigInt, EC, ECDSA, RSA, X25519 |
| `test_buf` | 73 | ByteReader, string/JSON/URL helpers, line counting, path addressing |
| `test_fs_engine` | 106 | Sandbox path rules, edits, patches, backups, move, glob/grep/map |
| `test_search` | 93 | HTML parsers, robots.txt, snippet finder, live fetch/search against a local `python3 http.server` fixture |
| `test_tls_fixture` | 46 | Real TLS 1.3 handshakes against `openssl s_server`: verify on/off, pinned CA, hostname mismatch, chain introspection |
| `test_sandbox` | 114 | rlimits, timeouts, output caps, policies, memory guard, brain intents, model reply parsing, agent tools and run loop |

Suites that need `openssl` or `python3` print `SKIP` and exit 0 when absent.

---

## Project layout

```
CMakeLists.txt            primary build (CMake ≥ 3.13)
Makefile                  plain-make fallback build
scripts/install.sh        one-shot build + install
scripts/build.sh          build only (CMake when present, else make)
scripts/run_tests.sh      build + run all six suites
config/lca.conf.example   config file for --config
include/lca/              public headers (one per module)
src/                      implementation + CLI entry point (main.cpp)
tests/                    six self-contained suites + tiny harness
```

| Module | Responsibility |
|--------|----------------|
| `common.h` | `Result<T>`, `Error`, logging, time/size helpers |
| `buf` | bytes, strings, JSON, URL parsing, text tools |
| `crypto` / `crypto_asym` | SHA-2 family, HMAC, HKDF, AES-free AEAD via ChaCha20-Poly1305, BigInt, RSA, ECDSA (P-256), X25519 |
| `x509` | DER/X.509 parsing, chain building, trust stores |
| `tls` | TLS 1.3 client (record layer, handshake, key schedule) |
| `net` | DNS, TCP, HTTP/1.1, gzip/deflate, redirect policy |
| `search` | search-engine HTML parsers, robots.txt, extraction |
| `fs_engine` | workspace sandbox + the autonomous file engine |
| `proc` | process manager + execution sandbox |
| `mem` | hardware detection + memory guard |
| `model` | heuristic planner + local model client |
| `agent` | tool registry, autonomy loop, verification |
| `main` | CLI (run/chat/search/fetch/exec/read/write/edit/patch/ls/glob/grep/map/doctor/tools/version) |

---

## Configuration

`--config FILE` reads defaults (CLI flags win). See
[`config/lca.conf.example`](config/lca.conf.example):

```ini
brain=auto
model-endpoint=http://127.0.0.1:8080
max-steps=24
timeout=120000
max-ram=4G
max-vram=3G
policy=developer
```

---

## Troubleshooting

* `lca doctor` reports `warn cmake` — expected when CMake is not installed;
  use `make` or `./scripts/install.sh`.
* `no local model server found … falling back to the built-in planner` —
  informational: start llama.cpp `server` (or similar) on the configured
  endpoint to use the `local` brain.
* TLS to the public internet requires a system CA bundle
  (`/etc/ssl/certs/ca-certificates.crt` on Debian/Ubuntu); `doctor` checks it.

## License

Delivered as-is for local use. No third-party code is vendored; the entire
stack is original to this repository.
