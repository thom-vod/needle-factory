---
name: needle-pipeline
description: Generate a Needle DOT pipeline for the current project using live rules and config defaults.
argument-hint: [filename.dot] <what to build>
allowed-tools: Read, Write, Edit, Bash, Glob, Grep, AskUserQuestion
---

## Generate a needle pipeline

Create a DOT pipeline for Needle from `$ARGUMENTS`.

This skill is procedural. Authoring constraints and node semantics
are canonical in the output of `needle dot-rules` (compiled into the
binary). Do not assume the needle repository is checked out on the
machine — the binary is the source of truth.

## Step 1: Determine output filename

Parse `$ARGUMENTS`:
1. If first token ends with `.dot`, use it as filename.
2. Otherwise generate `MMDD-<short_slug>.dot`.
3. Keep slug short (2-4 words).
4. Remaining text is pipeline intent.

Before writing:
1. Check whether file already exists.
2. If present, ask via `AskUserQuestion` whether to overwrite or rename.
3. Never overwrite without explicit confirmation.

## Step 2: Refresh live data

Refresh rule/config context before drafting. The needle binary is
the source of truth — never read documentation from the filesystem.

Run:
1. `needle dot-rules`
2. `needle config list --scope defaults --json`
3. `needle template list` — names of bundled sample DOTs (may be
   useful in Step 5 when picking a starting template).

Cache outputs in working context.

If `needle` is not on PATH: **stop and tell the user**. The skill
cannot proceed without the binary — there's no offline fallback,
since the rules, the validator, and the linter all live in the
binary. Suggest the user install needle and re-invoke the skill.

## Step 3: Understand the project

Read project context files when present:
- `README*`
- `package.json`
- `Cargo.toml`
- `go.mod`
- `pyproject.toml`
- `CMakeLists.txt`
- `Makefile`

Extract:
1. language/runtime stack
2. build commands
3. test commands/frameworks
4. app shape (CLI/service/web/library/monorepo)
5. reusable validation hooks/scripts

## Step 4: Identify external dependencies and agree on a test harness

Look for external dependency surfaces:
- remote APIs/base URLs
- DB/queue/cloud integrations
- auth providers
- hardware/device endpoints
- frontend-backend coupling

If none found, continue to step 5.

If found:
1. Use `AskUserQuestion` before drafting DOT.
2. Offer harness options:
- local boot script
- docker-compose fixture
- staging endpoint + test creds
- record/replay fixture
- manual-only surface
3. Capture:
- setup command
- teardown command
- base URL/port
- credentials/seeds
- mandatory end-to-end flows

Bake into DOT:
1. `setup_fixture` before first integration test
2. `integration_test` that exercises agreed flows
3. `teardown_fixture` on success and failure paths
4. manual-only dependency notes in `write_readme` prompt

Do not claim cross-system validation without executable or explicitly manual coverage.

## Step 5: Draft DOT using live rules + live config

Requirements:
1. Follow canonical rules from the cached `needle dot-rules` output
   (Step 2). When unsure about a node type, edge condition, or
   attribute, search the cached output rather than guessing.
2. Include `model_stylesheet` with `$context.config.defaults.*` references.
3. Do not hardcode model names.
4. Include core stages:
- `orient`
- `prereqs`
- `plan`
- implementation phase(s)
- validate/fix loop
- integration_test/fix_integration loop when needed
- critique
- write_readme
- review + apply_feedback loop
5. Prompts write artifacts under `{{logs_dir}}/<phase>/<NAME>-{N}.md`.
6. Keep node scope bounded; split oversized prompts.

Class mapping defaults:
- `planning`: orient/plan
- `fix`: repair loops
- `critique`: adversarial review
- `docs`: documentation stage
- `apply_feedback`: post-review changes

## Step 5b: Shell-safety constraints on `tool` node commands

`tool` nodes with shell metacharacters (`|`, `>`, `<`, `&&`, `;`,
etc.) execute via `/bin/sh -c "<command>"`. On macOS that's
**bash 3.2**, which has a heredoc + apostrophe parser bug that's
bitten real production pipelines. The canonical authoring rule is
**T11** in `needle dot-rules` (read it before drafting any
`command="..."` value with a heredoc).

Short summary of T11: prefer single-line writes
(`echo`, `printf`); if a heredoc is needed, quote the delimiter
(`<<'EOF' ... EOF`); assume natural-language content contains
apostrophes.

Reproduction (for your own reference, not authoritative — T11 is):

```bash
bash -c '
cat > /tmp/probe.txt <<EOF
${NAME}'\''s pipeline broke
EOF
'
# On bash 3.2: ${NAME} expands to empty, output starts with 's
```

The same constraint applies to any `command=` value the troubleshoot
agent might land in via Tweak/Full repair — generate them with the
same care.

## Step 6: Validate + lint

Run:
1. `needle validate <filename>`
2. `needle dot-lint <filename> --json`

Process:
1. inspect warnings/errors
2. revise DOT inline
3. rerun until clean or only intentional, explained warnings remain
4. do not rely on `--allow-unresolved-vars` for standard generation

`needle` should already have been verified available in Step 2. If
validate/lint commands fail for any reason, stop and surface the
error rather than declaring the DOT done — without successful lint
the pipeline almost certainly has bugs the author can't see.

## Step 7: Self-review pass

Same drafting session performs a self-review:
1. DOT matches the live `dot-rules` output cached in Step 2
2. required validation and feedback loops are present
3. tool-node exit-code/timeout/logging rules are respected
4. human-review ordering and edges are correct
5. prompts are specific, bounded, artifact-oriented
6. cross-system dependencies are tested or explicitly documented
7. tool-node `command=` values follow the shell-safety rules from
   Step 5b — no unquoted heredocs, no bashisms in commands without
   an explicit `bash -c '...'` wrapper, no assumption that
   apostrophe-free natural language exists

Revise and re-check before finalizing.

## Step 8: Write and summarize

Write final DOT file and summarize:
1. output filename
2. high-level flow
3. stage count
4. validation approach
5. run command: `needle run <filename>`
6. customization pointers (stylesheet defaults, prompts, commands)

## Bundled templates

If the user's request matches a bundled template (e.g. UI/UX work,
deep research, design-pipeline scaffolding), prefer to start from
that template and adapt it rather than re-deriving from canonical
rules. Discover and inspect via:

- `needle template list` — names of bundled templates.
- `needle template show <name>` — full DOT source.

Both commands are binary-backed (no filesystem assumption).

## Dependencies for this pipeline

Always include this block in final summary:

> **Dependencies for this pipeline**
>
> *Handled automatically by `prereqs`:*
> - <deps installed by prereqs>
>
> *Requires manual install/setup before run:*
> - <system packages, runtimes, secrets, external tooling>
>
> *Runtime assumptions:*
> - <ports, network reachability, env flags, OS/runtime assumptions>

Rules:
1. clearly separate automated vs manual setup
2. include system-level requirements not safe for unattended install
3. include credentials/config assumptions
4. keep entries concrete and actionable

## Authoring boundaries

Keep this skill procedural.
Do not copy full format/tool rule text here.
Those rules live in the `needle dot-rules` output (binary-embedded).

## Completion criteria

Complete only when:
1. DOT file is written to agreed filename.
2. validation/lint run (or explicitly unavailable).
3. self-review pass is complete.
4. summary includes dependencies callout.
