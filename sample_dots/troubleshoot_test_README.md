# Troubleshoot test fixtures

Four DOT files designed to exercise the auto-troubleshooter across its three
tiers. Each one is small, runs in seconds, and has a predictable failure the
agent should be able to recognise and address.

| File | Tier | Failure | Expected fix |
|------|------|---------|--------------|
| `troubleshoot_test_tweak_timeout.dot` | **Tweak** | `slow_op` has `timeout="1s"` but sleeps 5s. WallClock timeout. | Bump the `timeout` attribute (e.g. `30s`), then `needle stage retry:slow_op`. |
| `troubleshoot_test_tweak_typo.dot`    | **Tweak** | `say_hello` runs `ehco 'hello …'` — typo. SelfExitError. | Edit the DOT: `ehco` → `echo`, then `needle stage retry:say_hello`. |
| `troubleshoot_test_diagnose_only.dot` | **Diagnose** | `needs_human` exits 42 with an operator-review stderr line. | No fix: write recovery report (`outcome: reported`), or escalate to interactive chat. |
| `troubleshoot_test_full_missing_tool.dot` | **Full** | `parse_json` pipes JSON through `jq`. Many environments don't have it. | Either substitute a portable equivalent (`python -c '...'`), OR run `brew install jq` / `apt-get install jq` and retry. |

## How to drive them from the dashboard

1. `needle serve` (binds to localhost by default).
2. Open the dashboard, click **Run** on one of the fixtures.
3. In the new-run modal, set **Troubleshoot mode on failure** to the tier
   shown in the table above.
4. Click **Run**. The pipeline starts, executes `setup`, then trips the
   failing node. The troubleshoot tile attaches and streams agent activity
   live.
5. On success for Tweak/Full, the run resumes and `finalize` writes
   `"resumed cleanly"` to its stdout. On Diagnose, the run stays failed
   but the tile shows the report link.

## How to drive them from the CLI

```bash
# Tweak — auto-fix via the agent
needle run sample_dots/troubleshoot_test_tweak_typo.dot --troubleshoot-mode tweak

# Diagnose — report only, do not auto-resume
needle run sample_dots/troubleshoot_test_diagnose_only.dot --troubleshoot-mode diagnose

# Full — broader allow-list
needle run sample_dots/troubleshoot_test_full_missing_tool.dot --troubleshoot-mode full
```

## Resetting between runs

Each fixture is designed so the **DOT itself gets mutated by the agent's
fix**. Re-running afterwards from a clean checkout would re-trigger the
failure; re-running from the post-fix state would skip the troubleshoot
path because the fault is gone. To restore the original DOT after a
session:

```bash
# Roll back the agent's edits via the backup branch the session captured
needle troubleshoot rollback <run-dir> <session-id>

# Or just discard the working-tree edits
git checkout -- sample_dots/troubleshoot_test_*.dot
```

`needle troubleshoot rollback` uses the SPRINT-017+ preflight (branch +
diff-subset) so it refuses if the operator has made unrelated edits since
the session started.

## What the agent's recovery report looks like

After any session, look under:

```
<run-dir>/troubleshoot/session-<ISO8601-Z>-<rand4>/
├── recovery.md       # operator-facing report (YAML frontmatter + body)
├── events.ndjson     # raw stream-json events from the agent
├── backup-base.txt   # pre-agent HEAD SHA  (Tweak / Full only)
├── backup-branch.txt # backup ref name      (Tweak / Full only)
├── pre-modified.txt  # operator's uncommitted edits at session start
├── agent-modified.txt# files the agent touched (written at exit)
├── write-hook.json   # allow-region manifest (all tiers)
└── agent.stdout.log / agent.stderr.log
```

The recovery report's `outcome:` field is the quick way to tell what
happened. Look for `resumed`, `reported`, `escalated`, `failed_agent`, or
`failed_hook_violation`.
