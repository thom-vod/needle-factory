#!/usr/bin/env bash
# Install git pre-commit hooks for this repo.
#
# Hooks installed:
#   1. embed-regen: when staged content includes source for any of the
#      `embed_*.py` generators, regenerate the matching `*_embedded.cpp`
#      and stage the result. Prevents the "I edited the source but
#      forgot to commit the regen" gap.
#   2. gitleaks: scan staged content for secrets.
#
# Run once per clone per machine. Idempotent — safe to re-run.

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v gitleaks >/dev/null 2>&1; then
    cat >&2 <<EOF
warning: gitleaks is not on PATH. The hook will warn-skip the secret
         scan step on each commit until you install it:

  macOS:    brew install gitleaks
  Linux:    https://github.com/gitleaks/gitleaks#installing
            (or download a release tarball and put gitleaks on \$PATH)
  Windows:  scoop install gitleaks
            choco install gitleaks

         The embed-regen hook will still install and work.

EOF
fi

hook=".git/hooks/pre-commit"
mkdir -p "$(dirname "$hook")"

cat > "$hook" <<'HOOK_EOF'
#!/usr/bin/env bash
# pre-commit hook for needle-factory.
#
#  1. For each embed_*.py generator, if any of its source files are
#     staged, regenerate the matching *_embedded.cpp and stage the
#     result. The corresponding CMake `add_custom_command` would do
#     the same at build time, but CI doesn't push the regen back —
#     a fresh checkout without a build sees a stale embedded copy.
#  2. Scan staged content for secrets via gitleaks.
#
# Bypass with `git commit --no-verify` if you must (CI will still
# enforce both).

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

py=$(command -v python3 || true)

# Helper: returns true if any staged path matches one of the given
# globs. `git diff --cached --name-only` is robust against renames
# (it lists post-rename paths for renames-with-modifications).
staged_matches() {
    local match=0
    local staged
    staged=$(git diff --cached --name-only --diff-filter=ACMR)
    [ -z "$staged" ] && return 1
    local pattern
    for pattern in "$@"; do
        # shellcheck disable=SC2254
        if printf '%s\n' "$staged" | grep -qE "$pattern"; then
            match=1
            break
        fi
    done
    [ "$match" -eq 1 ]
}

# Run a generator if its sources are staged. Stage the result if it
# differs from what's in the index. Abort the commit on regen failure.
run_generator() {
    local label="$1" script="$2" output="$3"
    shift 3
    local source_patterns=("$@")
    if ! staged_matches "${source_patterns[@]}"; then
        return 0
    fi
    if [ -z "$py" ]; then
        echo "warning: python3 not found; cannot regenerate $output (sources of $label are staged)" >&2
        echo "         skipping. Run 'python3 scripts/$script' manually after install." >&2
        return 0
    fi
    if [ ! -f "scripts/$script" ]; then
        echo "warning: scripts/$script not found; skipping $label regen" >&2
        return 0
    fi
    echo "→ $label sources staged; regenerating $output..."
    if ! "$py" "scripts/$script" >/dev/null; then
        echo "error: $label regen failed (scripts/$script). Aborting commit." >&2
        echo "       Re-run manually for details:  python3 scripts/$script" >&2
        exit 1
    fi
    if ! git diff --quiet -- "$output"; then
        git add -- "$output"
        echo "✓ staged regenerated $output"
    fi
}

# Generator → source paths (regex, used by `grep -E` against staged
# paths). Keep aligned with the `add_custom_command` DEPENDS lists
# in CMakeLists.txt.
run_generator \
    "dashboard HTML" \
    "embed_html.py" \
    "src/server/dashboard_html.cpp" \
    "^src/server/assets/dashboard\.(html|css|js)$"

run_generator \
    "DOT authoring rules" \
    "embed_rules.py" \
    "src/server/dot_authoring_rules_embedded.cpp" \
    "^docs/dot-authoring-rules\.md$"

run_generator \
    "bundled templates" \
    "embed_templates.py" \
    "src/server/templates_embedded.cpp" \
    "^sample_dots/.*\.dot$"

# Secret scan (gitleaks). Runs LAST so any regen content is scanned too.
if ! command -v gitleaks >/dev/null 2>&1; then
    echo "warning: gitleaks not installed; skipping secret scan" >&2
    exit 0
fi

gitleaks protect --staged --redact --verbose
HOOK_EOF

chmod +x "$hook"

echo "✓ pre-commit hook installed at $hook"
if command -v gitleaks >/dev/null 2>&1; then
    echo "  gitleaks version: $(gitleaks version 2>&1 | head -1)"
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  note: python3 not on PATH — embed regen step will be a warn-skip."
    echo "        Install python3 to enable automatic regen."
fi
echo "  Smoke tests:"
echo "    secret leak:  echo 'sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' > /tmp/leak.txt"
echo "                  git add -f /tmp/leak.txt && git commit -m bad   # should fail"
echo "    embed regen:  touch sample_dots/empty.dot && git add sample_dots/empty.dot"
echo "                  git commit -m test   # should auto-stage templates_embedded.cpp"
