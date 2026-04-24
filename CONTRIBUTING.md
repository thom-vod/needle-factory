# Contributing to Needle

Thanks for considering a contribution. Needle is a small, focused codebase —
the contribution loop is intentionally lightweight.

## Getting started

1. Fork and clone.
2. Build from source (see `README.md` for dependencies).
3. Run the test suite: `./build/needle_tests`.

## Scope

Needle is a DOT-graph pipeline runner. Changes should fit one of:

- **Engine** — execution, checkpoint/resume, retry, subgraphs (`src/engine/`).
- **Parser / validation** — DOT parsing, stylesheet parsing, lint rules
  (`src/parser/`, `src/validation/`).
- **Handlers** — new node types or refinements to existing ones
  (`src/handlers/`).
- **Backends** — CLI provider templates, LLMKit HTTP, process runner
  (`src/backend/`).
- **Dashboard / server** — HTTP server, embedded SPA, SSE streaming
  (`src/server/`).

Changes that don't fit one of those categories are welcome but warrant a
quick issue-first discussion.

## Pull requests

- Keep PRs focused. One logical change per PR. If a refactor is needed to
  enable a feature, land the refactor first.
- Build must pass with `-Wall -Wextra -Wpedantic -Werror` on Linux, macOS,
  and Windows (MSYS2). CI will check.
- Add tests for new behaviour. `tests/test_<feature>.cpp` — Catch2 v2.
- Run `./needle_tests` locally before pushing.
- Don't commit generated files. The dashboard is an exception: if you edit
  `src/server/assets/dashboard.{html,css,js}`, re-run
  `python3 scripts/embed_html.py` and commit the regenerated
  `src/server/dashboard_html.cpp` in the same PR.
- Commit messages: subject line in imperative mood, body explains *why*.

## Adding a new handler

1. Declare the factory in `include/needle/handlers/all_handlers.h`.
2. Implement the handler in `src/handlers/<name>_handler.cpp` following the
   pattern in `codergen_handler.cpp` or similar.
3. Register it in `src/handlers/handler_registry.cpp::create_default()`.
4. Add it to the `NEEDLE_LIB_SOURCES` list in `CMakeLists.txt`.
5. Write tests in `tests/test_<name>_handler.cpp` and add the entry to
   `NEEDLE_TEST_SOURCES` in `CMakeLists.txt`.
6. Document the handler in `src/server/assets/dashboard.html` under the
   "Node Types & Handlers" help page, then re-embed.

## Code style

- C++14, no newer features.
- Prefer small free functions and plain structs over class hierarchies.
- Headers use `#pragma once`.
- Namespace is `needle`, with nested namespaces for subsystems
  (`needle::parser`, `needle::engine`, etc).
- No exceptions across library boundaries. Use `Result<T>` (see
  `include/needle/util/result.h`).

## License

By contributing, you agree that your contributions will be licensed under
the Apache License, Version 2.0. See `LICENSE`.
