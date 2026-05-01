# Skills

Claude Code skills for working with needle. Drop these into
`~/.claude/skills/` (or symlink them) so they're available as slash
commands during a Claude Code session.

## Available skills

- **`needle-pipeline/`** — generates a DOT-graph pipeline for the current
  project. Invoke with `/needle-pipeline [filename.dot] <description>`.
  Knows the canonical orient → prereqs → plan → implement → validate →
  fix → integration_test → critique → review topology, the format and
  validation rules, the role-specific prompt preambles, and the
  CLI-specific (claude / codex) prompt variants.

## Installing a skill

```bash
# Symlink (recommended — picks up upstream updates):
ln -sfn "$(pwd)/skills/needle-pipeline" ~/.claude/skills/needle-pipeline

# Or copy:
cp -r skills/needle-pipeline ~/.claude/skills/needle-pipeline
```

After either, restart Claude Code or run `/skills` to refresh.
