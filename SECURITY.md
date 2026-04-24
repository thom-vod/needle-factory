# Security policy

## Reporting a vulnerability

If you believe you have found a security vulnerability in Needle, please
report it privately rather than opening a public issue.

Open a
[private security advisory](https://github.com/thom-vod/needle-factory/security/advisories/new)
on GitHub. Include:

- A description of the issue.
- Steps to reproduce (minimal DOT pipeline + commands if applicable).
- The version / commit SHA you observed it on.
- Any mitigation you have already identified.

You will receive an acknowledgement within a few days. Fixes for confirmed
vulnerabilities will be prepared in a private branch and disclosed
coordinated with the fix release.

## Scope

Needle runs user-authored pipelines that may invoke shell commands, LLM
providers, and HTTP endpoints. Misuse within that scope (e.g. a pipeline
that deliberately runs destructive shell commands) is expected behaviour,
not a vulnerability.

In-scope issues include:

- Memory safety bugs in the engine, parser, or handlers.
- Path traversal, command injection, or SSRF reachable through the dashboard
  HTTP API or through DOT file parsing.
- Authentication or authorisation bypass on the dashboard server.
- Leakage of API keys or run artefacts to unintended destinations.
- Denial-of-service via malformed DOT files, malformed HTTP payloads, or
  crafted stylesheets.

## Supported versions

The current `main` branch and the most recent tagged release receive
security fixes.
