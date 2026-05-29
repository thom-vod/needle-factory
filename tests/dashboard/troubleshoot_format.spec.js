// Troubleshooter activity formatting + report link.
//
// Verifies the dashboard renders troubleshoot tool activity as plain-English
// summaries (not raw minified JSON) and links the recovery report through the
// sandboxed read-file endpoint (not a bare filesystem path that 404s).
//
// These exercise the pure formatting helpers as loaded in the real browser
// bundle via page.evaluate, so no live troubleshoot session is required.
//
// Run locally:
//   1. Boot needle:  ./build/needle serve --port 18483 --project-dir /tmp/needle-smoke
//   2. From this dir: npm install && BASE_URL=http://localhost:18483 npx playwright test
//
// CI: not yet wired into .github/workflows/ci.yml (needs Node + Playwright on
// the runner) — same status as smoke.spec.js.

const { test, expect } = require('@playwright/test');

const BASE_URL = process.env.BASE_URL || 'http://localhost:18483';

test.beforeEach(async ({ page }) => {
    await page.goto(BASE_URL);
});

test('report link routes through the read-file endpoint', async ({ page }) => {
    const reportPath = '/abs/run/.needle/game_design/troubleshoot/session-1/recovery.md';
    const href = await page.evaluate((p) => troubleshootReportHref(p), reportPath);
    expect(href).toBe('api/v1/read-file?path=' + encodeURIComponent(reportPath));
    expect(href.startsWith('/')).toBeFalsy();  // relative, for reverse-proxy base href
});

test('tool_call activity renders a plain-English summary, not raw JSON', async ({ page }) => {
    const out = await page.evaluate(() => formatTroubleshootActivity({
        type: 'tool_call',
        tool: 'Bash',
        input: '{"command":"ls -la /tmp/proj/","description":"List project directory"}',
    }));
    expect(out.label).toBe('Bash');
    expect(out.summary).toBe('Ran `ls -la /tmp/proj/`');
    expect(out.raw.length).toBeGreaterThan(0);  // raw JSON preserved behind the expander
});

test('file and search tools summarize by path/pattern', async ({ page }) => {
    const cases = await page.evaluate(() => ({
        write: formatTroubleshootActivity({ type: 'tool_call', tool: 'Write', input: '{"file_path":"STORYBOARD-1.md"}' }).summary,
        read: formatTroubleshootActivity({ type: 'tool_call', tool: 'Read', input: '{"file_path":"spec.md"}' }).summary,
        grep: formatTroubleshootActivity({ type: 'tool_call', tool: 'Grep', input: '{"pattern":"TODO"}' }).summary,
    }));
    expect(cases.write).toBe('Wrote `STORYBOARD-1.md`');
    expect(cases.read).toBe('Read `spec.md`');
    expect(cases.grep).toBe('Searched `TODO`');
});

test('session rows pass through their human-readable text', async ({ page }) => {
    const summary = await page.evaluate(() => formatTroubleshootActivity({
        type: 'session_started', tool: 'session', input: 'started claude-opus-4-7',
    }).summary);
    expect(summary).toBe('started claude-opus-4-7');
});
