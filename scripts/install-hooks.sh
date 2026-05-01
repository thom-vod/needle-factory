#!/usr/bin/env bash
# Install git pre-commit hooks for this repo.
# Currently installs: gitleaks (secret scanning).
#
# Run once per clone per machine. Idempotent — safe to re-run.

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v gitleaks >/dev/null 2>&1; then
    cat >&2 <<EOF
gitleaks is not installed. Install it first, then re-run this script.

  macOS:    brew install gitleaks
  Linux:    https://github.com/gitleaks/gitleaks#installing
            (or download a release tarball and put gitleaks on \$PATH)
  Windows:  scoop install gitleaks
            choco install gitleaks

EOF
    exit 1
fi

hook=".git/hooks/pre-commit"
mkdir -p "$(dirname "$hook")"

cat > "$hook" <<'HOOK_EOF'
#!/usr/bin/env bash
# pre-commit hook: scan staged content for secrets.
# Bypass with `git commit --no-verify` if you must (CI will still catch it).

if ! command -v gitleaks >/dev/null 2>&1; then
    echo "warning: gitleaks not installed; skipping secret scan" >&2
    exit 0
fi

gitleaks protect --staged --redact --verbose
HOOK_EOF

chmod +x "$hook"

echo "✓ pre-commit hook installed at $hook"
echo "  gitleaks version: $(gitleaks version 2>&1 | head -1)"
echo "  Smoke test: stage a file with 'sk-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' and try to commit."
