# Needle vs Attractor Spec Comparison

Comparison of needle's implementation and the needle-pipeline skill against the
upstream [attractor spec](https://github.com/strongdm/attractor) (`attractor-spec.md`).

Last updated: 2026-03-30

---

## Where we align

- **Core topology**: start/exit nodes, edge conditions, checkpoint/resume, human gates — identical
- **Edge selection**: Same 5-step priority (condition > label > suggested_next > weight > lexical)
- **Shape mapping** (mostly): `Mdiamond`=start, `Msquare`=exit, `box`=codergen, `hexagon`=wait_human, `component`=parallel
- **Graph/node/edge attributes**: goal, label, prompt, fidelity, class, model_stylesheet, timeout, goal_gate, retry_target — all present
- **Condition language**: `outcome=success`, `outcome!=success`, `&&`, `context.*` — identical
- **Stylesheet system**: Same CSS-like specificity (`*` < shape < `.class` < `#id`)
- **Execution loop**: parse > transform > validate > initialize > execute > finalize
- **Checkpoint/resume**: Same structure (current_node, completed_nodes, context, retry counters)
- **Outcome model**: SUCCESS, PARTIAL_SUCCESS, RETRY, FAIL, SKIPPED statuses
- **Interviewer pattern**: Same interface (ask/inform), same implementations (console, auto-approve, queue, callback, recording)
- **Context namespaces**: `context.*`, `parallel.*`, `human.gate.*`, `stack.*` — identical conventions
- **Validation rules**: Same core lint rules (single start/exit, reachability, no incoming to start, no outgoing from exit, condition syntax)

---

## Gaps in needle vs the spec

| Area | Attractor Spec | Needle |
|------|---------------|--------|
| **Fan-in shape** | `tripleoctagon` | `trapezium` |
| **Tool shape** | `parallelogram` | `box` + `handler="tool"` |
| **Conditional shape** | `diamond` | Not shape-based |
| **Manager loop shape** | `house` | Not shape-based |
| **Handler type names** | Dotted: `wait.human`, `parallel.fan_in`, `stack.manager_loop` | Underscored: `wait_human`, `fan_in`, `manager_loop` |
| **`allow_partial`** | Node attribute to accept PARTIAL_SUCCESS on retry exhaustion | Not implemented |
| **`auto_status`** | Auto-generate SUCCESS if handler writes no status.json | Not implemented |
| **`reasoning_effort`** | `low`/`medium`/`high` on nodes and in stylesheet | Not exposed in skill (engine may support it) |
| **`fallback_retry_target`** | Secondary jump target at node and graph level | Not implemented |
| **`weight` on edges** | Documented for priority tiebreak | Implemented in engine but not documented in skill |
| **`loop_restart` on edges** | Terminate run and re-launch with fresh log directory | Not implemented |
| **`thread_id`** | Session reuse key for `full` fidelity | Not implemented |
| **Edge `fidelity`/`thread_id`** | Override target node's fidelity/thread via edge attrs | Not implemented |
| **Subgraphs** | Scoping defaults + class derivation from labels | Needle parses them but skill says "NO subgraph blocks" |
| **Accelerator keys** | `[Y] Label`, `Y) Label`, `Y - Label` patterns on human gate edges | Not documented in skill |
| **`max_parallel`** | Bounds concurrent parallel branches | Not implemented |
| **Chained edges** | `A -> B -> C [label="x"]` expands to individual edges | Supported by parser but not documented in skill |
| **`rankdir`** | Used in spec examples (`rankdir=LR`) | Skill forbids standalone attrs outside `graph []` |
| **Backoff presets** | `none`, `standard`, `aggressive`, `linear`, `patient` | Not implemented (needle uses fixed retry logic) |
| **Tool hooks** | `tool_hooks.pre`/`tool_hooks.post` graph attrs for shell hooks around LLM tool calls | Not implemented |
| **Artifact store** | Named, typed storage with file-backing threshold (100KB) | Needle uses stage directories + context but no formal artifact store API |

---

## Where needle goes beyond the spec

| Area | Needle | Attractor Spec |
|------|--------|---------------|
| **`threshold` join policy** | Succeed if >= N branches pass (`join_threshold` attr) | Not in spec (only `wait_all`, `first_success`) |
| **`wait_any` join policy** | Succeed when first branch succeeds | Not in spec by that name (spec has `first_success`) |
| **`interactive` handler** | Human-AI collaborative chat within pipeline | Not in spec |
| **`nested_run` handler** | Execute sub-pipelines as a node | Spec mentions concept but no named handler |
| **`web_search` handler** | Web search via Tavily | Not in spec |
| **`doc_fetch` handler** | Clone and analyze repositories | Not in spec |
| **`llmkit` handler** | Direct LLM API call (bypasses codergen backend) | Not in spec (spec routes all LLM through codergen backend) |
| **Artifact logging convention** | `logs/<phase>/<NAME>-{N}.md` with sequential numbering | Spec has `{logs_root}/{node_id}/` but no naming convention for LLM-written artifacts |
| **Pipeline generation rules** | 31 rules for authoring good pipelines (validation cycles, critique, README, human review placement) | Spec is about the engine, not authoring best practices |
| **`no_commit` attribute** | Prevent agent from committing after a node | Not in spec |
| **Web dashboard** | Full single-page app with SSE streaming, DOT editor, AI chat, run management | Spec mentions HTTP server mode as optional with core endpoints |
| **Per-DOT isolation** | `.needle/<dot_stem>/` subdirectories for concurrent runs | Spec assumes single `logs_root` per run |
| **Run registry** | `~/.needle/runs.json` for persistent run history | Not in spec |

---

## Shape mapping discrepancy

| Node type | Attractor spec | Needle |
|-----------|---------------|--------|
| start | `Mdiamond` | `Mdiamond` |
| exit | `Msquare` | `Msquare` |
| codergen | `box` | `box` |
| wait_human | `hexagon` | `hexagon` |
| conditional | `diamond` | Not shape-based (uses `handler="conditional"`) |
| parallel | `component` | `component` |
| fan_in | `tripleoctagon` | `trapezium` |
| tool | `parallelogram` | `box` + `handler="tool"` |
| manager_loop | `house` | Not shape-based (uses `handler="manager_loop"`) |

Needle evolved with different shape conventions for fan_in, tool, conditional, and manager_loop.
The `trapezium` shape for fan_in is visually more intuitive (funnel shape) but differs from spec.

---

## Recommended additions to needle

High value, low effort additions from the spec:

1. **`reasoning_effort`** — document in skill; useful for routing planning nodes to deeper thinking
2. **`weight` on edges** — already implemented, just undocumented in skill
3. **Accelerator key patterns** on human gate labels (e.g. `[A] Approve`) — already parsed by wait_human handler
4. **Chained edge syntax** documentation — already supported by parser
5. **`fallback_retry_target`** — useful safety net for goal gate enforcement
6. **`allow_partial`** — graceful degradation when retries exhausted

Lower priority:

7. **`loop_restart`** on edges — useful for long-running iterative pipelines
8. **`thread_id`** for `full` fidelity session reuse — matters for multi-turn LLM conversations
9. **`max_parallel`** — bounding concurrency for resource-constrained environments
10. **Backoff presets** — more flexible retry behavior
