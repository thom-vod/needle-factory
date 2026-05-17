# needle

A DOT-graph pipeline runner. Define workflows as Graphviz DOT files, execute them with LLM and CLI backends, and monitor everything through a real-time web dashboard.

Needle parses DOT graphs where nodes represent pipeline stages (LLM calls, shell commands, human approvals, parallel branches) and edges represent control flow. It executes the graph, handles retries, checkpoints, and streams events via SSE to a browser-based dashboard.

## Quick start

```bash
# Build (Linux / macOS / Windows MSYS2)
make
sudo make install         # installs to /usr/local on Linux/macOS, ~/.local on Windows

# Run with an existing pipeline
./build/needle serve sample_dots/simple_pipeline.dot
# Open http://localhost:8080

# Or try the interactive-chat template (opens a chat panel in the dashboard)
./build/needle serve sample_dots/interactive_chat.dot

# Or start with an empty workspace and build interactively
./build/needle serve
# The AI assistant in the Create view will help you design a pipeline
```

The top-level `Makefile` is a thin wrapper around CMake — see [Building](#building)
for the underlying `cmake` invocations and platform-specific notes.

## Stage status

The engine writes a `status.json` for every executed stage under
`<run-dir>/stages/<node-id>/`. Beyond the basic outcome fields, two
diagnostic fields are populated automatically:

- `git_state` — commits added, files newly untracked, and files modified
  during the stage (relative to the stage's start). Surfaces salvageable
  partial work after a stage failure without manual `git status`
  archaeology.
- `timeout_kind` — `"wall_clock"` or `"idle"`, when `timed_out=true`.
  Distinguishes a stalled silent process from one that simply ran past
  its hard cap.

## Per-project worktree strategy

Concurrent work against the same repo clobbers the working tree,
commits, and `bin/`/`obj/`. Worktrees scope work to its own `git
worktree` so parallel work doesn't interfere.

**Today** (post-SPRINT-012), parallel branches inside a single run
each get their own worktree at `<repo>-wt-<run_id>-<branch_id>` when
`worktree.strategy = auto`. Branch commits cherry-pick into the launch
repo at fan-in; conflicts surface as `FailureKind::CherryPickConflict`.
Checkpoint persists `branch_worktrees` so resume reuses them. The
`cleanup = remove-on-success` mode removes per-branch worktrees on
fan-in success.

Config (`~/.needle/config.json`):

- `worktree.strategy` — `"off"` (default) | `"auto"` | `"manual"` (stub)
- `worktree.branch_template` — `"auto/${run_id}"`
- `worktree.path_template` — `"../${repo_basename}-wt-${run_id}"`
- `worktree.cleanup` — `"keep"` (default) | `"remove-on-success"`

**Not yet:** whole-pipeline scoping (two independent runs in the same
repo still share a checkout), UI exposure, `manual` mode, and the
`needle worktree list/prune` CLI verbs.

For the forward-looking design — scenarios, open decisions, and what
to address before shipping whole-pipeline scoping — see
[`docs/worktree-design.md`](docs/worktree-design.md).

## Troubleshoot agent

Automates recovery after a failed pipeline stage. The troubleshooter
uses the built-in classifier as a first-pass diagnosis, then launches a
tier-scoped Claude Code session that streams activity to the dashboard
and writes a v2 recovery report under:

```
<run-dir>/troubleshoot/session-<timestamp>/recovery.md
```

Modes:

- `diagnose`: read-only triage plus a recovery report.
- `tweak`: default auto-recovery tier; can edit the DOT, stage
  `prompt.md` files, selected `defaults.*` config keys, and run Needle
  recovery commands. Changes are snapshot-backed and can be reverted.
- `full`: broad repair in a dedicated `auto/troubleshoot/<run-id>` git
  worktree. Apply merges the worktree branch; discard removes it.

Enable auto-troubleshoot on a graph:

```dot
graph [ troubleshoot_on_failure="tweak" ]
```

Legacy `troubleshoot_on_failure="true"` still works and maps to
`tweak`, but logs a one-time deprecation warning. Use
`troubleshoot_on_failure="false"` or `"off"` to disable it.

CLI surface:

```
needle run graph.dot --troubleshoot-mode diagnose|tweak|full
needle run graph.dot --troubleshoot
needle troubleshoot run <run-dir> [--mode=diagnose|tweak|full] [--trust=snapshot|worktree]
needle troubleshoot escalate --reason X --next-question Y [--session-id SID] [--run-dir DIR]
needle troubleshoot revert <run-dir> <session-id>
needle troubleshoot apply <run-dir> <session-id>
needle troubleshoot discard <run-dir> <session-id>
```

`needle troubleshoot <run-dir>` remains as the legacy diagnose/report
command. The dashboard exposes the same flow from failed run cards and
the run detail troubleshooter tile. For the canonical design reference,
see [`docs/troubleshooter-design.md`](docs/troubleshooter-design.md).

## Per-node tool allow-lists

Non-coding stages (critique, write_pr, docs) need to be prevented from
modifying source even if a context-hijack convinces the agent to call
`Edit` or `Write`. The `allowed_tools` attribute pairs with the skill's
role-isolation prompt preamble (S7) for defence-in-depth:

```dot
critique [
    class="critique",
    allowed_tools="Read,Bash,Grep,Glob",   // explicit allow-list
    ...
]
```

Or via stylesheet:

```dot
graph [ model_stylesheet="
    .critique  { allowed_tools = \"Read,Bash,Grep,Glob\" }
    .write_pr  { allowed_tools = \"Read,Bash,Grep,Glob,Write:logs/pr/**\" }
" ]
```

For `claude`-routed nodes, the listed tools are passed via
`--allowedTools` and obvious file-modifying tools (Edit, Write,
NotebookEdit) are explicitly disallowed unless they appear in the
allow-list. For `codex` and `gemini`, allow-list enforcement is left
to the prompt preamble in v1.

Path-scoped writes (`Write:<glob>`) are recognised but currently
translated to bare `Write` with a logged note — full glob enforcement
is a follow-up.

## Soft graph-hash check on resume

`needle resume` no longer fails simply because the DOT was edited mid-run.
The validator records each completed node's per-node hash on the
checkpoint and uses it to distinguish two cases on hash mismatch:

- All completed nodes' hashes match the current graph: warn quietly
  ("only unstarted nodes affected") and continue. Mid-run timeout bumps
  and prompt edits to upcoming nodes work without `graph_hash`
  surgery.
- A completed node's hash has changed: warn loudly with the node id.
  Pass `--strict-graph-hash` (or set `strict_hash_check=true` on the
  graph attrs) to escalate to an error that blocks resume.

## Manual stage management

When an operator hand-finishes a stage (after debugging, manual recovery,
or the troubleshoot agent applying a fix), two CLI verbs replace the
five-edit checkpoint dance:

```
needle stage mark <run-dir> <node-id> <success|failure> [--output "summary"]
needle stage advance <run-dir> --to <node-id>
```

`mark` updates `completed_nodes`, `context["codergen.<node-id>.output"]`,
`context["needle.last_outcome.status"]`, `current_node`, and the stage's
`status.json` atomically. `advance` sets `current_node` so a subsequent
`needle resume <run-dir>` continues from the chosen target.

The graph-aware "what's next from here" lookup belongs to the
troubleshoot agent (Sprint 6+); for now, `advance` requires `--to`.

## Prompt-size guards

Codergen prompts grow over a multi-phase pipeline as upstream context
gets pulled forward. A prompt much larger than 100 KB almost always
indicates a misshapen DOT (typically `fidelity="full"` carrying verify
output through every phase) and is worth aborting before burning a full
attempt.

needle warns at 100 KB and hard-fails at 500 KB by default. Tune via the
user config:

```
needle config set defaults.prompt_warn_kb 75
needle config set defaults.prompt_fail_kb 300
```

Set either to `0` to disable.

## Commands

```
needle run <graph.dot>           Run a pipeline to completion
needle serve [graph.dot]         Start HTTP server with web dashboard
needle validate <graph.dot>      Validate a graph without running
needle resume <checkpoint.json>  Resume from a checkpoint
needle retry <node>              Reset checkpoint to before a node and resume
needle config [get|set|list]     Manage configuration
needle auth                      Validate API keys
needle status                    Show pipeline status from checkpoint
```

**Note on resume behavior**: When resuming from a checkpoint, retry budgets are
intentionally reset. Each node gets a fresh retry allowance, regardless of how
many retries were consumed in the prior run. This gives the pipeline a clean
chance to succeed after transient failures.

### Options

```
--port PORT           HTTP server port (default: 8080)
--bind ADDR           Bind address (default: 127.0.0.1)
--logs-dir DIR        Directory for run logs and checkpoints
--project-dir DIR     Project working directory (default: cwd)
--stylesheet FILE     NSS stylesheet file for model routing
--backend cli|llmkit  Backend to use (default: cli)
--var key=value       Set a context variable (repeatable)
--dry-run             Use no-op handlers (validate only)
--debug               Enable debug/trace logging
--json                Output events as JSON lines on stdout
--no-color            Disable colored output
```

### Model stylesheet

Route nodes to different agents and models using CSS-like rules in a `model_stylesheet` graph attribute or external `.nss` file:

```dot
graph [model_stylesheet="
    * { agent = \"codex\"; llm_model = \"gpt-5.4\" }
    .planning { agent = \"claude\"; llm_model = \"claude-opus-4-7\"; reasoning_effort = \"high\" }
    .coding { agent = \"codex\"; llm_model = \"gpt-5.4\" }
    .critique { agent = \"gemini\"; llm_model = \"gemini-2.5-pro\" }
"]
```

The `agent` property selects the CLI tool (claude, codex, gemini). `llm_provider` is accepted as an alias. Default values come from your configured model defaults (`needle config`).

Selectors by specificity: `*` (all) < `.class` < `#node_id`. Node-level attributes override stylesheet rules.

## Pipeline definition

Pipelines are Graphviz DOT digraphs. Each node has a `type` attribute that determines how it executes:

| Type | Description |
|------|-------------|
| `start` | Entry point (exactly one per graph) |
| `exit` | Termination (exactly one per graph) |
| `codergen` | Runs an external CLI tool (code generation, builds) |
| `llmkit` | Calls an LLM provider directly |
| `tool` | Executes a shell command |
| `conditional` | Branches based on edge conditions |
| `parallel` | Fan-out: all outgoing edges execute concurrently |
| `fan_in` | Synchronization point for parallel branches |
| `wait_human` | Pauses for human input or approval |
| `manager_loop` | Iterative loop with a goal gate |
| `interactive` | Multi-turn human-AI collaborative chat |
| `nested_run` | Execute a sub-pipeline |
| `doc_fetch` | Fetch and extract content from URLs |
| `web_search` | Web search via configured provider |

### Node attributes

- `label` — display name
- `type` — node type (inferred from shape if not set)
- `prompt` — prompt text for `llmkit` and `codergen` nodes
- `agent` — CLI agent: `"claude"`, `"codex"`, or `"gemini"` (alias: `llm_provider`)
- `llm_model` — model name override
- `class` — for model stylesheet targeting (e.g., `"planning"`, `"coding"`, `"critique"`)
- `command` — shell command for `tool` nodes
- `timeout` — wall-clock max execution time, e.g. `"90m"`, `"30s"`. Codergen nodes default to the template's 45m; tool nodes default to 60s.
- `idle_timeout` — kill the wrapped process if no stdout/stderr arrives for this duration. Disabled (`0`) by default for both codergen and tool nodes — reasoning models can think silently for long stretches. Opt in per-node, or graph-wide via `node [idle_timeout="10m"]`.
- `goal_gate` — `"true"` for `manager_loop` exit condition
- `max_iterations` — max loop count for `manager_loop`

Graph-level `node [timeout="90m", idle_timeout="6m"]` declarations propagate to every node (declared, edge-only-referenced, or otherwise). Per-node attrs override the graph default. When a process is killed, the stage status's `timeout_kind` field distinguishes `wall_clock` (total elapsed exceeded `timeout`) from `idle` (no output for `idle_timeout`).

### Edge attributes

- `label` — display label
- `condition` — condition expression (e.g., `outcome=success`)

### Example

```dot
digraph ci_pipeline {
    start      [type=start, label="Start", shape=Mdiamond]
    build      [type=tool, label="Build", command="make"]
    test_unit  [type=tool, label="Unit Tests", command="make test"]
    test_lint  [type=tool, label="Lint", command="make lint"]
    merge      [type=fan_in, label="Merge"]
    review     [type=wait_human, label="Review"]
    deploy     [type=tool, label="Deploy", command="make deploy"]
    end        [type=exit, label="Done", shape=Msquare]

    start -> build
    build -> test_unit
    build -> test_lint
    test_unit -> merge
    test_lint -> merge
    merge -> review
    review -> deploy [label="Approve"]
    deploy -> end
}
```

## Web dashboard

The dashboard is served at `http://localhost:8080` when running `needle serve`. It is a single-page app with no external build system — all HTML, CSS, and JS are embedded in the binary.

### Views

- **Dashboard** — stats cards (total/running/completed/failed), run grid with status badges and progress bars
- **Monitor** — stage list with expandable logs, SVG graph with real-time node status coloring, zoom controls, tab bar for multiple runs
- **Create** — AI assistant chat for building pipelines from natural language, DOT editor with live graph preview, file upload, template loading
- **Settings** — API keys with validation, agent/model defaults (coding, planning, critique, chat), dark/light theme, gate sound, logging level
- **Logs** — pipeline event log (node started/completed/failed) and needle system log (`~/.needle/needle.log`), both with live updates and auto-scroll

### AI assistant

The Create view includes an AI-powered pipeline builder. Describe what you want in plain language and the assistant generates valid DOT:

1. Set your API key: `export ANTHROPIC_API_KEY=sk-...` (or `OPENAI_API_KEY` / `GEMINI_API_KEY`)
2. Run `needle serve`
3. Open the Create view, type a description, and click Send
4. The assistant generates DOT source, which populates the editor and updates the graph preview
5. Iterate: "add a retry step", "make the tests run in parallel", etc.
6. Click "Run Pipeline" to execute

### Real-time updates

- SSE event stream at `/api/v1/events` multiplexes events from all runs
- Automatic reconnection with exponential backoff
- SVG node coloring updates via CSS classes (no page refresh)
- Toast notifications on run completion/failure
- Dashboard API requests use relative paths (via `apiUrl()` helper) so the UI works behind reverse proxies
- Interactive chat sessions are persisted incrementally to `chat_history.json` — reconnecting after a disconnect restores the conversation

## API

All endpoints under `/api/v1/`.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Dashboard HTML |
| `GET` | `/api/v1/status` | Server metadata (graph name, node/edge count, active runs) |
| `GET` | `/api/v1/runs` | List all runs (includes CLI-started runs from `runs.json`) |
| `GET` | `/api/v1/runs/{id}` | Single run detail |
| `POST` | `/api/v1/runs` | Start a run (optional `dot_source`, `project_dir`, `vars`) |
| `POST` | `/api/v1/resume` | Resume from checkpoint (`project_dir`, `dot_stem`, optional `replace_run_id`) |
| `POST` | `/api/v1/runs/{id}/cancel` | Cancel a run |
| `DELETE` | `/api/v1/runs/{id}` | Delete a run (optional `?artifacts=true`) |
| `POST` | `/api/v1/runs/{id}/answer` | Submit human gate answer |
| `POST` | `/api/v1/runs/{id}/continue` | Continue interactive/gated run |
| `GET` | `/api/v1/stages/{node_id}` | Stage detail (prompt, response, status) |
| `GET` | `/api/v1/graph/svg` | Startup graph as SVG |
| `GET` | `/api/v1/graph/dot` | Startup graph as DOT |
| `POST` | `/api/v1/render-dot` | Render DOT source to SVG |
| `POST` | `/api/v1/generate-dot` | AI pipeline generation (send `{provider, messages}`) |
| `GET` | `/api/v1/events` | Global SSE event stream |
| `GET` | `/api/v1/logs/needle` | Tail needle log file (`?offset=N` for incremental) |
| `GET` | `/api/v1/config` | Read configuration |
| `POST` | `/api/v1/config` | Update configuration values |
| `POST` | `/api/v1/config/validate-key` | Validate a provider API key |
| `GET` | `/api/v1/models/{provider}` | List available models for a provider |
| `POST` | `/api/v1/check-checkpoint` | Check if checkpoint exists for a project/DOT |
| `POST` | `/api/v1/pause` | Pause all running pipelines |
| `POST` | `/api/v1/pause/resume` | Resume paused pipelines |

## Building

### Requirements

- C++14 compiler (GCC 5+, Clang 3.4+, MSVC 2015+)
- CMake 3.10+
- A build driver: GNU Make, Ninja, or MSBuild (Ninja is recommended on Windows)
- pthreads (provided by the OS on Linux/macOS, by mingw-w64 on Windows)

### Optional dependencies

| Dependency | Purpose | Fallback |
|------------|---------|----------|
| libcurl | LLM API calls (`llmkit` nodes, AI assistant) | LLM features disabled |
| Graphviz `dot` binary | SVG graph rendering in Monitor view | Textual stage list only |
| Python 3 | Regenerating dashboard assets from source | Use committed generated file |

### Bundled (in `third_party/`)

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — HTTP server
- [nlohmann/json](https://github.com/nlohmann/json) — JSON parsing
- [Catch2](https://github.com/catchorg/Catch2) v2 — testing

### Make wrapper

The top-level `Makefile` wraps the canonical CMake build so the same `make`
commands work on Linux, macOS, and Windows MSYS2:

```bash
make             # configure (if needed) and build
make install     # install to PREFIX (CMake's platform default; see below)
make test        # run the test suite
make clean       # remove build artefacts (keeps cmake cache)
make distclean   # remove the entire build directory
make reconfigure # force cmake to re-run configure
make help        # print all targets and current settings
```

**Default install prefix**:

| Platform     | Default `PREFIX` | Needs elevation? |
|--------------|------------------|------------------|
| Linux, macOS | `/usr/local` (CMake's default — Unix convention) | yes (`sudo make install`) |
| Windows MSYS2 | `~/.local` | no — CMake's `C:/Program Files/...` default would |

Override with `PREFIX=...` for any other location:

```bash
make PREFIX=~/.local install                # user-local install, no sudo
make PREFIX=/opt/needle install
make BUILD_TYPE=Debug
make CMAKE_FLAGS="-DNEEDLE_ASAN=ON"         # extra cmake flags
make GENERATOR="Unix Makefiles"             # force a generator
```

The wrapper auto-selects the build generator: Ninja when present, otherwise
`Unix Makefiles` on POSIX or `MSYS Makefiles` on Windows.

### Build options

The wrapper passes `-DNEEDLE_BUILD_SERVER=ON` by default. To toggle anything
else, either invoke `cmake` directly or pass `CMAKE_FLAGS=...` to make:

```bash
cmake -B build \
  -DNEEDLE_BUILD_SERVER=ON   \  # HTTP server and dashboard (default: ON)
  -DNEEDLE_BUILD_TESTS=ON    \  # Test suite (default: ON)
  -DNEEDLE_ASAN=ON           \  # AddressSanitizer (GCC/Clang only)
  -DNEEDLE_TSAN=ON              # ThreadSanitizer (GCC/Clang only)
```

### Building on Windows (MSYS2)

The project is developed and tested with the MSYS2 MinGW64 toolchain. Native MSVC
should also work but is not part of the regular CI matrix.

1. Install [MSYS2](https://www.msys2.org/) and update the package database
   (`pacman -Syu`, restart the shell, then `pacman -Su`).
2. From an **MSYS2 MinGW64** shell (Start menu → "MSYS2 MinGW 64-bit"), install
   the toolchain and dependencies:

   ```bash
   pacman -S --needed \
       mingw-w64-x86_64-gcc \
       mingw-w64-x86_64-cmake \
       mingw-w64-x86_64-ninja \
       mingw-w64-x86_64-curl \
       mingw-w64-x86_64-python \
       mingw-w64-x86_64-graphviz
   ```

   `mingw-w64-x86_64-curl` and `mingw-w64-x86_64-graphviz` are optional but
   recommended; without them the AI assistant and SVG graph preview are
   disabled at runtime (the build still succeeds).

3. Build and install with the make wrapper (Ninja is auto-selected when
   present, which is the recommended generator on Windows):

   ```bash
   make
   make install                     # installs to ~/.local on Windows
   ```

   The output is `build/needle.exe` (and `build/needle_tests.exe` if tests are
   enabled). After `make install`, the binary lands at `~/.local/bin/needle.exe`
   along with `~/.local/share/needle/sample_dots/` and `~/.local/share/needle/scripts/`.

   On Windows the wrapper defaults `PREFIX` to `~/.local` because CMake's
   platform default (`C:/Program Files/needle`) requires admin. `~/.local/bin`
   is not on Windows `PATH` by default — add it once from a regular shell, or
   invoke the binary by full path.

4. Smoke-test:

   ```bash
   ./build/needle.exe --version
   ./build/needle.exe serve sample_dots/simple_pipeline.dot --port 8080
   ```

#### Pitfalls on Windows

- **Don't build from Git Bash.** Git for Windows ships its own MSYS-based
  `make`/`sh` on `PATH`. Its `sh` overrides `TMP` to `/tmp`, which the native
  `gcc.exe` cannot write to — you will see
  `Cannot create temporary file in C:\WINDOWS\: Permission denied`. Use the
  MSYS2 MinGW64 shell, or invoke the build from `cmd`/PowerShell with the
  MSYS2 directories on `PATH`:

  ```powershell
  $env:Path = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:Path"
  make
  ```

- **The wrapper auto-prefers Ninja over `make` as the underlying CMake
  generator.** The `MSYS Makefiles` generator shells through `sh -c` for every
  recipe, which inherits the broken `TMP` behaviour above when invoked from
  the wrong shell. Ninja calls compilers directly and avoids the issue.

- **Adjust the `MSYS2` path** above if you installed MSYS2 somewhere other
  than `C:\msys64`.

### Asset pipeline

Dashboard source files live in `src/server/assets/` as standalone HTML, CSS, and JS. The embed script converts them to C++ string constants:

```bash
python3 scripts/embed_html.py
```

The generated `src/server/dashboard_html.cpp` is committed to the repo, so Python is not a build dependency. CMake regenerates it automatically if Python 3 is available and asset files change.

## Testing

```bash
./build/needle_tests                    # Run all tests
./build/needle_tests "[graph_serializer]"  # Run specific tag
./build/needle_tests "[dashboard]"         # Dashboard endpoint tests
```

On Windows the binary is `build\needle_tests.exe`.

## LLM providers

### CLI backend (codergen nodes)

The CLI backend dispatches to local CLI tools. The `agent` node attribute (or stylesheet) selects the template:

| Agent | CLI tool | Default model | Prompt delivery |
|-------|----------|---------------|-----------------|
| `claude` | `claude` | claude-opus-4-7 | stdin |
| `codex` | `codex` | gpt-5.4 | stdin |
| `gemini` | `gemini` | gemini-2.5-pro | stdin via `-p` |

Configure defaults via `needle config set defaults.coding_agent codex` etc., or in the dashboard Settings.

### LLMKit backend (llmkit nodes)

Direct HTTP API calls. Set the appropriate environment variable:

| Provider | Environment variable | Default model |
|----------|---------------------|---------------|
| Anthropic | `ANTHROPIC_API_KEY` | claude-sonnet-4-20250514 |
| OpenAI | `OPENAI_API_KEY` | gpt-4o |
| Google | `GEMINI_API_KEY` | gemini-2.5-flash |

### Web search provider

The `web_search` handler uses Tavily:

| Provider | Environment variable |
|----------|---------------------|
| `tavily` | `TAVILY_API_KEY` |

All keys can also be set via `needle config set providers.<name>.api_key <key>` or the dashboard Settings UI.

## JSON event output

Pass `--json` to emit pipeline events as JSON lines on stdout (one event per
line). Useful for scripting, CI, or piping into another process:

```bash
needle run --json pipeline.dot | jq 'select(.type == "node.complete")'
```

Events include `node.transition`, `node.complete`, `gate.waiting`, and the
usual lifecycle signals.

## Architecture

```
include/needle/
  model/         Graph, Node, Edge, Context, AttributeMap
  parser/        DOT lexer, parser, graph builder
  validation/    Graph validation rules
  engine/        Pipeline engine, edge selection, checkpoints, retry
  handlers/      Node type handlers (start, exit, codergen, llmkit, ...)
  backend/       LLM and CLI backends
  interviewer/   Human interaction (console, HTTP, auto-approve)
  event/         Event types, bus, collector
  server/        HTTP server, dashboard, DOT generator
  util/          Helpers (graph serializer, fs helpers, run registry, logger)

src/             Implementation files matching the header structure
scripts/         embed_html.py (asset pipeline)
tests/           Unit and integration tests
third_party/     Bundled dependencies
sample_dots/     Example pipeline definitions
```

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for bundled
third-party licenses.
