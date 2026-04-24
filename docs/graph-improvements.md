# Graph Generation Improvements

Ideas and patterns for improving needle's attractor graph generation, based on analysis of the reference spec, coreys-attractor (Kotlin), attractor-c (C11), soulcaster (C#), and kilroy's create-dotfile skill.

## Sources Analyzed

- **Reference spec**: `~/github/attractor-analysis/reference/attractor-spec.md` — canonical DOT DSL schema
- **Coreys-attractor**: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/web/DotGenerator.kt` — LLM-powered DOT generation system prompt (282 lines)
- **Kilroy create-dotfile**: `~/github/attractor-analysis/kilroy/.claude/skills/create-dotfile/SKILL.md` — 25KB Claude Code skill with 8-phase workflow
- **Kilroy reference template**: `~/github/attractor-analysis/kilroy/.claude/skills/create-dotfile/reference_template.dot` — canonical complex topology (421 lines)
- **Soulcaster dotfiles**: `~/github/attractor-analysis/soulcaster/dotfiles/project-cli-task-tracker.dot` — hand-crafted 248-line project pipeline
- **Attractor-c**: `~/github/attractor-analysis/attractor-c/` — C11 implementation with built-in agent tools

---

## 1. Reference Template Topology

**Problem**: Needle generates graph structure from scratch every time. LLMs tend to produce shallow, linear graphs that miss important patterns (validate-fix loops, multi-phase critique, parallel decomposition).

**Solution**: Start from a proven reference template and customize for the project.

**Source**: Kilroy's `reference_template.dot` (421 lines) defines a canonical topology:
```
Bootstrap: start → check_toolchain → expand_spec → check_dod
DoD fanout: dod_fanout → dod_a/b/c → consolidate_dod → plan_fanout
Planning fanout: plan_fanout → plan_a/b/c → debate_consolidate → implement
Implement & verify: implement → check → fix_fmt → verify_fmt → ... → verify_fidelity
Review fanout: review_fanout → review_a/b/c → review_consensus
Postmortem routing: postmortem → impl_repair | needs_replan | needs_toolchain
```

**Implementation**: Include a reference template DOT file in needle's resources. The `generate` prompt should say "start from this template and adapt" rather than "generate from scratch."

---

## 2. Orient Phase

**Problem**: Pipelines jump straight to implementation without checking what tools, versions, and prior work are available.

**Solution**: Always include an orient node as the first real step.

**Source**: Soulcaster's project DOTs all start with:
```dot
orient [
    class="opus",
    label="Orient: Assess workspace",
    prompt="Assess the current working directory for any existing projects,
    toolchain version, available disk space.
    Check for prior run artifacts:
    - If logs/orient/ORIENT-*.md exists, read the latest.
    - If logs/implement/PROGRESS-*.md exists, read the latest.
    Write findings to logs/orient/ORIENT-{N}.md."
]
```

**Key insight**: Orient prompts explicitly check for prior artifacts, making them resume-aware.

---

## 3. Resume-Aware Prompts

**Problem**: When a pipeline resumes, nodes start from scratch even though previous work may exist.

**Solution**: Every prompt should start by checking for prior artifacts.

**Source**: All soulcaster dotfiles include this pattern in every node prompt:
```
Check for prior run artifacts: logs/orient/ORIENT-*.md,
logs/implement/PROGRESS-*.md, logs/validate/VALIDATION-RUN-*.md.
```

**Implementation**: The `generate` prompt should instruct that every codergen node's prompt begins with artifact checking. Alternatively, needle could inject this automatically as a preamble.

---

## 4. Numbered Artifacts

**Problem**: Each run overwrites the same files (`response.md`, `prompt.md`), losing history.

**Solution**: Use numbered artifact files that accumulate across iterations.

**Source**: Soulcaster/kilroy pattern:
```
logs/orient/ORIENT-1.md
logs/orient/ORIENT-2.md
logs/plan/PLAN-1.md
logs/plan/PLAN-2.md
logs/validate/VALIDATION-RUN-1.md
logs/validate/VALIDATION-RUN-2.md
```

Each phase reads prior artifacts and builds on them (not overwriting). The agent can see its own history.

---

## 5. Model Stylesheet

**Problem**: Needle uses the same model for all nodes. Complex tasks benefit from different models for different roles.

**Solution**: CSS-like model stylesheet with selectors.

**Source**: All other implementations support this (reference spec section 2, soulcaster, coreys-attractor):
```dot
model_stylesheet = "
    * { provider = \"anthropic\"; model = \"claude-sonnet-4-6\" }
    .opus  { provider = \"anthropic\"; model = \"claude-opus-4-7\" }
    .codex { provider = \"openai\";    model = \"codex-5.2\" }
    .gpt   { provider = \"openai\";    model = \"gpt-5.2\" }
"
```

Nodes are assigned classes: `orient [class="opus"]`, `critique_harsh [class="gpt"]`.

**Use case**: Use opus for planning/orient, sonnet for implementation, GPT for adversarial critique. Different perspectives catch different blind spots.

---

## 6. Multi-Model Critique

**Problem**: The same model that wrote the code reviews it — blind spots are shared.

**Solution**: Use a different model/provider for critique.

**Source**: Soulcaster's project pipelines include two critique passes:
```dot
critique_harsh [class="gpt", label="Critique: Adversarial Review"]
critique_pareto [class="opus", label="Critique: Pareto Prioritization"]
critique_gate [shape=hexagon, label="Ship or Iterate?"]
```

The harsh critique (GPT) finds issues, the pareto critique (Opus) prioritizes them, then the human decides.

---

## 7. Implementation Decomposition

**Problem**: Large implementations are given to a single node, which times out or produces partial work.

**Solution**: Decompose into per-module fan-out with ~200-500 lines per node.

**Source**: Kilroy's SKILL.md:
```
For ~1,000+ lines of new code:
- Decompose implement into per-module fan-out
  (implement_core, implement_api, implement_data_layer)
- Each module node targets ~200-500 lines
- Add merge_implementation synthesis node
- Use parallel fan-out or sequential chain
```

**Also**: Analyze-before-implement pattern for porting existing code:
```
analyze_fanout → analyze_<module>×N → merge_analysis → [implement cluster]
```

---

## 8. Validation Via Scripts (Not Inline Commands)

**Problem**: Validation commands are hardcoded in DOT node attributes. They're fragile, hard to iterate on, and can't grow beyond one line.

**Solution**: Write validation scripts during the pipeline, then invoke them.

**Source**: Kilroy mandates:
```dot
validate [handler="tool",
    tool_command="sh scripts/validate-build.sh || { echo 'VALIDATE_FAILURE: ...'; exit 1; }"]
```

The implement nodes write `scripts/validate-*.sh` as part of their work. Validation scripts can be iteratively improved without changing the DOT.

---

## 9. Failure Classification

**Problem**: Needle only knows pass/fail. Different failure types need different responses.

**Solution**: Classify failures so routing can be intelligent.

**Source**: Kilroy defines:
- `failure_class`: `transient_infra` (network flake), `deterministic` (code bug), `structural` (design issue)
- `failure_reason`: human-readable description
- `failure_signature`: stable fingerprint for cycle detection

Routing based on failure class:
```dot
postmortem → impl_repair [condition="context.failure_class=deterministic"]
postmortem → needs_replan [condition="context.failure_class=structural"]
postmortem → needs_toolchain [condition="context.failure_class=transient_infra", loop_restart=true]
```

---

## 10. Cycle Detection

**Problem**: Validate-fix loops can run forever if the same failure keeps recurring.

**Solution**: Track failure signatures and abort after N identical failures.

**Source**: Kilroy's cycle detection contract:
- Engine tracks `map[signature]int` where `signature = nodeID|failureClass|normalizedReason`
- When tuple appears `loop_restart_signature_limit` times (default 3), run aborts
- Only `status=fail` or `status=retry` enter tracker
- Only `deterministic` and `structural` failures tracked; `transient_infra` excluded
- Signatures never reset on success

**Implementation**: Add cycle detection to needle's engine. Set `failure_signature` on validate nodes with variable failure prose.

---

## 11. Postmortem Node

**Problem**: When validation fails, the fix node gets a generic "fix the failures" prompt without analysis of what went wrong.

**Solution**: Add a postmortem/triage node between failure and fix.

**Source**: Kilroy requires postmortem nodes with ≥3 condition-keyed outbound edges:
```dot
postmortem → impl_repair [condition="context.failure_class=deterministic"]
postmortem → needs_replan [condition="context.failure_class=structural"]
postmortem → needs_toolchain [condition="context.failure_class=transient_infra"]
```

The postmortem reads test evidence, analyzes the failure, classifies it, and routes appropriately. It also detects zero-progress loops (same failing set as last time → route to replan, not repair).

---

## 12. Goal Gates

**Problem**: A pipeline can reach the exit node even if critical steps failed (e.g., via unconditional edges).

**Solution**: Mark critical nodes as goal gates. Exit checks all gates passed.

**Source**: Reference spec and all implementations:
```dot
validate [goal_gate=true, retry_target="plan"]
```

At the exit node, the engine checks if all `goal_gate=true` nodes achieved SUCCESS. If not, routes to `retry_target` instead of exiting.

---

## 13. Fidelity Modes

**Problem**: Large pipeline contexts can exceed LLM token limits. Every node gets the full context.

**Solution**: Control how much context each node receives.

**Source**: Reference spec section 5:
- `full` — complete context + session reuse
- `truncate` — last N chars per value
- `compact` — skip internal keys, short values
- `summary:low/medium/high` — LLM-summarized context

Set per-node or per-edge:
```dot
implement [fidelity="full"]
critique [fidelity="summary:high"]
```

---

## 14. Built-In Agent Tools

**Problem**: Needle shells out to `claude -p` which brings its own tools. Needle has no visibility into what tools the agent uses, can't control the tool set, and can't stream intermediate results.

**Solution**: Provide workspace tools (read_file, write_file, shell, grep, glob) directly from the engine.

**Source**: All other implementations provide 5 built-in tools:

| Tool | attractor-c | coreys-attractor | soulcaster |
|------|-------------|-----------------|------------|
| read_file | Yes | Yes | Yes |
| write_file | Yes | Yes | Yes |
| shell/bash | Yes | Yes (run_command) | Yes (bash) |
| grep | Yes | No | Yes |
| glob | Yes (list_files) | Yes (list_files) | Yes (glob) |
| edit_file | No | No | Yes |

The engine manages the tool loop, sees all tool calls, can log them, enforce timeouts, and stream progress.

---

## 15. Dynamic Model List

**Problem**: Model IDs get stale. The `generate` prompt hardcodes model names.

**Solution**: Fetch the current model list before writing the model stylesheet.

**Source**: Kilroy's Phase 0:
```bash
kilroy attractor modeldb suggest
```
"Use ONLY listed model IDs; don't use stale memory."

**Implementation**: Before the `generate` node runs, a tool node could call the model list API and inject available models into context.

---

## 16. Test Evidence Contract

**Problem**: Validation passes or fails, but there's no structured evidence of what was tested.

**Solution**: Validation scripts produce structured evidence artifacts.

**Source**: Kilroy requires:
- `scripts/validate-test.sh` writes artifacts to `.ai/runs/$RUN_ID/test-evidence/latest/`
- Produces `manifest.json` mapping each test scenario to artifact paths and pass/fail status
- UI scenarios require screenshots; non-UI require text/structured evidence
- Postmortem node reads manifest and cites artifact paths in findings

---

## 17. Loop Restart

**Problem**: Some failures (transient infrastructure) need a completely fresh start, not a fix attempt.

**Solution**: Edge attribute that terminates the current run and starts fresh.

**Source**: Reference spec:
```dot
postmortem -> start [condition="context.failure_class=transient_infra", loop_restart=true]
```

When `loop_restart=true`, the engine terminates the current run, creates a new log directory, and starts fresh from the target node with clean context.

---

---

## Spec Compliance Gaps

The following features are defined in the official attractor spec (https://github.com/strongdm/attractor) but missing from needle. Items marked **PRIORITY** should be implemented before other improvements.

### 18. Goal Gates **[PRIORITY — HIGH]**

**Spec reference**: Section 3.4 — Goal Gate Enforcement

**Problem**: A pipeline can reach the exit node even if critical steps failed (e.g., tests never passed, deployment never validated). The spec requires `goal_gate=true` nodes to succeed before exit is allowed.

**What the spec requires**:
- Nodes with `goal_gate=true` must reach SUCCESS or PARTIAL_SUCCESS before the pipeline can exit
- At exit node: check all visited goal_gate nodes. If any failed, jump to `retry_target` instead of exiting
- Retry target cascade: node retry_target → node fallback_retry_target → graph retry_target → graph fallback_retry_target → FAIL

**Implementation references**:
- Attractor-c: `~/github/attractor-analysis/attractor-c/src/attractor/engine.c` — `check_goal_gates()` function
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/engine/Engine.kt` — goal gate check at terminal node
- Soulcaster: `~/github/attractor-analysis/soulcaster/src/JcAttractor.Attractor/Execution/PipelineEngine.cs` — goal gate enforcement in main loop
- Reference spec pseudocode in Section 3.2 (Steps 1-2 of the WHILE loop)

**Needle change**: Add goal gate check in `pipeline_engine.cpp` at the exit node check. Add `goal_gate`, `retry_target`, `fallback_retry_target` node attributes. Add graph-level `retry_target` and `fallback_retry_target`.

---

### 19. Fidelity Modes **[PRIORITY — HIGH]**

**Spec reference**: Section 5.4 — Context Fidelity

**Problem**: Every node in needle gets the full context. For long pipelines with large outputs (doc_fetch, web_search), this can exceed LLM token limits or waste tokens on irrelevant history.

**What the spec requires**:
- 6 fidelity modes: `full`, `truncate`, `compact`, `summary:low`, `summary:medium`, `summary:high`
- `full`: reuse LLM session (same thread), carry full conversation history
- `truncate`: fresh session, minimal context (goal + run ID only)
- `compact`: fresh session, structured bullet-point summary of completed stages and key context
- `summary:*`: fresh session, LLM-generated summary at varying detail levels
- Resolution precedence: edge fidelity → node fidelity → graph default_fidelity → `compact`
- Thread resolution for `full` mode: node thread_id → edge thread_id → graph default → subgraph class → previous node ID

**Implementation references**:
- Reference spec: Section 5.4 with full mode table and resolution algorithm
- Attractor-c: `~/github/attractor-analysis/attractor-c/src/attractor/engine.c` — preamble construction with fidelity modes (full/truncate/compact/summary fallback to compact)
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/state/Context.kt` — `buildPreamble()` with fidelity filtering
- Soulcaster: `~/github/attractor-analysis/soulcaster/src/JcAttractor.Attractor/Execution/PipelineEngine.cs` — fidelity resolution and context filtering

**Needle change**: Add fidelity attribute to nodes/edges/graph. In CLIBackend, construct context preamble based on fidelity mode before prompt. Add `default_fidelity` graph attribute.

---

### 20. Model Stylesheet **[PRIORITY — HIGH]**

**Spec reference**: Section 8 — Model Stylesheet

**Problem**: Needle uses the same model for all codergen nodes. Different tasks benefit from different models (opus for planning, sonnet for implementation, GPT for adversarial critique).

**What the spec requires**:
- CSS-like stylesheet in graph attribute `model_stylesheet`
- Selectors: `*` (all nodes), `.class` (class match), `#id` (node ID match), `shape` (shape match)
- Properties: `llm_model`, `llm_provider`, `reasoning_effort`, `fidelity`, `timeout`
- Specificity: ID > class > shape > universal
- Applied as a transform before execution

Example:
```
model_stylesheet = "
    * { llm_provider = \"anthropic\"; llm_model = \"claude-sonnet-4-6\" }
    .planning { llm_model = \"claude-opus-4-7\" }
    .critique { llm_provider = \"openai\"; llm_model = \"gpt-5.2\" }
    #critical_validation { reasoning_effort = \"high\"; timeout = \"60m\" }
"
```

**Implementation references**:
- Reference spec: Section 8 with full selector syntax and specificity rules
- Needle already has basic `.nss` stylesheet support (`src/engine/stylesheet_transform.cpp`) — extend it
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/transforms/StylesheetTransform.kt`
- Soulcaster dotfiles: `~/github/attractor-analysis/soulcaster/dotfiles/project-cli-task-tracker.dot` — shows `.opus`, `.codex`, `.gpt` classes in practice

**Needle change**: Extend existing stylesheet transform to support `llm_model`, `llm_provider`, `reasoning_effort`. CLIBackend already reads `llm_model` and `llm_provider` from node attrs — stylesheet just needs to set them.

---

### 21. PARTIAL_SUCCESS and allow_partial **[PRIORITY — MEDIUM]**

**Spec reference**: Section 5.2 (Outcome) and Section 3.5 (Retry Logic)

**Problem**: When retries are exhausted, needle either fails hard or succeeds. There's no middle ground for "partial work was done, continue with caveats."

**What the spec requires**:
- `PARTIAL_SUCCESS` status: treated as success for routing but notes describe what was incomplete
- `allow_partial=true` node attribute: when retries exhausted, accept PARTIAL_SUCCESS instead of FAIL
- Goal gates accept both SUCCESS and PARTIAL_SUCCESS

**Implementation references**:
- All four implementations support this status
- Attractor-c: `~/github/attractor-analysis/attractor-c/src/attractor/engine.c` — `allow_partial` check after retry exhaustion

**Needle change**: Add `PARTIAL_SUCCESS` to StageStatus enum, add `allow_partial` node attribute, update engine retry logic.

---

### 22. Failure Routing Cascade **[PRIORITY — MEDIUM]**

**Spec reference**: Section 3.7 — Failure Routing

**Problem**: Needle checks for conditional fail edges but doesn't implement the full retry_target cascade.

**What the spec requires** (in order):
1. Outgoing edge with `condition="outcome=fail"` → follow it
2. Node `retry_target` → jump to that node
3. Node `fallback_retry_target` → jump to that node
4. Graph `retry_target` → jump to that node
5. Graph `fallback_retry_target` → jump to that node
6. Pipeline terminates with FAIL

**Implementation references**:
- Attractor-c: `~/github/attractor-analysis/attractor-c/src/attractor/engine.c` — full 5-level cascade
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/engine/Engine.kt` — `resolveRetryTarget()` with cascade

**Needle change**: In `pipeline_engine.cpp` failure handling, add retry_target cascade after checking conditional edges.

---

### 23. Loop Restart **[PRIORITY — MEDIUM]**

**Spec reference**: Section 2.7 (edge attribute) and Section 3.2 (Step 7)

**Problem**: Some failures (transient infrastructure, stale state) need a fresh start rather than a fix attempt within the current context.

**What the spec requires**:
- Edge attribute `loop_restart=true`
- When traversing this edge, terminate current run, create new log directory, start fresh from target node

**Implementation references**:
- Reference spec: Section 3.2 Step 7
- Attractor-c: `~/github/attractor-analysis/attractor-c/src/attractor/engine.c` — `loop_restart` handling
- Kilroy SKILL.md: `loop_restart=true` only for `context.failure_class=transient_infra`

**Needle change**: Add `loop_restart` edge attribute. In edge selection, if selected edge has loop_restart, save state and re-invoke engine with fresh context.

---

### 24. Backoff Configuration **[PRIORITY — MEDIUM]**

**Spec reference**: Section 3.6 — Retry Policy

**Problem**: Needle has simple retry with basic delay. The spec defines configurable backoff with presets.

**What the spec requires**:
- Configurable: `initial_delay_ms`, `backoff_factor`, `max_delay_ms`, `jitter`
- Preset policies: `none`, `standard`, `aggressive`, `linear`, `patient`
- Delay formula: `initial * factor^(attempt-1)`, capped at max, with optional jitter

**Implementation references**:
- Needle already has `RetryPolicy` with `base_delay_ms`, `multiplier`, `max_delay_ms`, `jitter` in `src/model/retry_policy.cpp`
- Needs: preset names (`retry_preset` attribute) and the `should_retry` predicate for error classification

**Needle change**: Minimal — needle's RetryPolicy is close. Add preset resolution from node attribute.

---

### 25. Artifact Store **[PRIORITY — LOW]**

**Spec reference**: Section 5.5 — Artifact Store

**Problem**: Large stage outputs stored in context can bloat checkpoint.json and exhaust memory.

**What the spec requires**:
- Named, typed artifact store separate from context
- File-backed for artifacts > 100KB
- Store/retrieve/list/remove API

**Implementation references**:
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/db/RunStore.kt` — database-backed artifact storage

**Needle change**: Add ArtifactStore class. Large context values (doc_fetch content, web_search results) stored as artifacts rather than in context map.

---

### 26. Accelerator Key Parsing **[PRIORITY — LOW]**

**Spec reference**: Section 4.6 — Wait For Human Handler

**Problem**: Human gate edge labels like `[A] Approve` aren't parsed to extract the `A` accelerator key.

**What the spec requires**:
- Parse patterns: `[K] Label`, `K) Label`, `K - Label`
- Use extracted key for keyboard shortcut matching
- Strip prefix for label normalization in edge matching

**Implementation references**:
- Reference spec: Section 4.6, accelerator parsing patterns
- Coreys-attractor: `~/github/attractor-analysis/coreys-attractor/src/main/kotlin/attractor/handlers/WaitForHumanHandler.kt`

**Needle change**: Add accelerator parsing in wait_human_handler and edge selection label normalization.

---

### 27. Context Logs **[PRIORITY — LOW]**

**Spec reference**: Section 5.1 — Context

**Problem**: The spec defines an append-only run log in the context (`context.logs`). Needle uses the event bus instead.

**What the spec requires**:
- `Context.append_log(entry)` — append-only log entries
- Logs serialized in checkpoint for resume
- Logs available in context preamble for fidelity modes

**Needle change**: Low priority since needle's event system + needle.log serve a similar purpose. Could add if fidelity modes are implemented (logs become part of context preamble).

---

## Priority Order for Implementation

### Tier 1: Spec compliance — HIGH priority
These are required by the spec and missing from needle. Implement before other improvements.

1. **Goal gates** (#18) — prevent incomplete pipelines from exiting
2. **Model stylesheet** (#20) — per-node model/provider selection (needle has the plumbing, needs the transform)
3. **Fidelity modes** (#19) — token budget management for long pipelines
4. **Failure routing cascade** (#22) — full retry_target resolution
5. **PARTIAL_SUCCESS** (#21) — graceful degradation when retries exhausted

### Tier 2: Spec compliance — MEDIUM priority + high-impact patterns
6. **Loop restart** (#23) — fresh start on transient failures
7. **Backoff presets** (#24) — configurable retry behavior
8. **Cycle detection** (#10) — prevent infinite validate-fix loops
9. **Reference template topology** (#1) — better generated graphs
10. **Orient phase** (#2) — workspace assessment before planning

### Tier 3: Graph generation improvements
11. Resume-aware prompts (#3)
12. Multi-model critique (#6, requires #20)
13. Validation via scripts (#8)
14. Implementation decomposition (#7)
15. Numbered artifacts (#4)
16. Postmortem node pattern (#11)
17. Failure classification (#9)

### Tier 4: Engine features + lower priority
18. Built-in agent tools (#14) — replacing CLI shelling
19. Dynamic model list (#15)
20. Test evidence contract (#16)
21. Artifact store (#25)
22. Accelerator key parsing (#26)
23. Context logs (#27)
