// SPRINT-013 Phase 6: dashboard smoke test.
//
// Drives a real browser against a running needle dashboard. Verifies:
//   1. The dashboard loads.
//   2. Loading a DOT file renders the graph.
//   3. (xfail) The graph survives the run-start transition.
//
// Run locally:
//   1. Boot needle in another terminal:
//        ./build/needle serve --port 18483 --project-dir /tmp/needle-smoke
//   2. From this directory:
//        npm install
//        BASE_URL=http://localhost:18483 npx playwright test
//
// CI: not yet wired into `.github/workflows/ci.yml`. The smoke test
// requires Node.js + Playwright on the runner; pulling those in is
// tracked as a SPRINT-013 follow-up so this phase doesn't balloon
// into a workflow rewrite.

const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const os = require('os');

const BASE_URL = process.env.BASE_URL || 'http://localhost:18483';
const TMP_DIR = path.join(os.tmpdir(), `needle-smoke-${process.pid}`);

test.beforeAll(() => {
    fs.mkdirSync(TMP_DIR, { recursive: true });
    fs.writeFileSync(path.join(TMP_DIR, 'smoke.dot'), `
digraph g {
    graph [label="needle smoke test"]
    start [shape=Mdiamond, label="Start"]
    work [prompt="say hello"]
    exit [shape=Msquare, label="End"]
    start -> work -> exit
}
`);
});

test.afterAll(() => {
    fs.rmSync(TMP_DIR, { recursive: true, force: true });
});

test('dashboard loads', async ({ page }) => {
    await page.goto(BASE_URL);
    await expect(page).toHaveTitle(/needle/i);
});

test('graph renders after loading a DOT file', async ({ page }) => {
    await page.goto(BASE_URL);

    // Use the API directly to seed a run state, then navigate to the
    // monitor view. (Driving the file-picker UI in headless mode is
    // brittle; the API path exercises the same code that the UI feeds
    // into.)
    const dotPath = path.join(TMP_DIR, 'smoke.dot');
    const projectDir = TMP_DIR;
    const checkRes = await page.request.post(`${BASE_URL}/api/v1/check-run`, {
        data: {
            dot_source: fs.readFileSync(dotPath, 'utf8'),
            project_dir: projectDir,
        },
    });
    expect(checkRes.ok()).toBeTruthy();
    const checkData = await checkRes.json();
    expect(checkData.dot_stem).toBeTruthy();

    // Navigate to a create view where the editor renders the graph
    // preview from the loaded source.
    await page.goto(`${BASE_URL}/#create`);
    const editor = page.locator('#ndl-dot-editor, textarea').first();
    await editor.fill(fs.readFileSync(dotPath, 'utf8'));

    // The preview SVG should appear with at least one <g class="node">.
    const preview = page.locator('#ndl-create-preview svg g.node, svg .node').first();
    await expect(preview).toBeVisible({ timeout: 10_000 });
});

// SPRINT-013 §3.6 + KNOWN_ISSUES: the graph view disappears after
// clicking Run from the monitor and requires a full browser reload to
// recover. Mark `fixme` so the test is tracked but doesn't fail CI.
// Fix is deferred to a later sprint.
test.fixme('graph survives run-start transition', async ({ page }) => {
    await page.goto(`${BASE_URL}/#monitor`);
    // (Skeleton — exact selectors depend on the run state.)
    await page.locator('#ndl-monitor-load-dot').click();
    // ... seed run, click Run ...
    const graphAfterRun = page.locator('#ndl-monitor-graph svg');
    await expect(graphAfterRun).toBeVisible();
});
