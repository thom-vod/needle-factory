# Dashboard smoke tests

Playwright-driven smoke tests that exercise the needle dashboard end-to-end.
Verifies the basic load path, the graph renders from a DOT file, and (when
fixed) that the graph survives the run-start transition.

## Run locally

1. Boot needle in another terminal:

   ```sh
   ./build/needle serve --port 18483 --project-dir /tmp/needle-smoke
   ```

2. From this directory:

   ```sh
   npm install
   BASE_URL=http://localhost:18483 npx playwright test
   ```

The `BASE_URL` env var lets you point at any running instance. Defaults to
`http://localhost:18483`.

## CI

Not yet wired into `.github/workflows/ci.yml`. Pulling Node.js + Playwright
into the CI matrix on three platforms is a non-trivial workflow change —
tracked as a SPRINT-013 follow-up so this phase doesn't balloon into a
workflow rewrite.

## Known-failing tests

The `graph survives run-start transition` case is marked `fixme` and
tracks the disappearing-graph UI bug filed as a SPRINT-013 follow-up.
The fix is scoped to a later sprint; the test exists so it's not
silently forgotten.
