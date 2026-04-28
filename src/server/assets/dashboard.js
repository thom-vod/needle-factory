/* needle dashboard — vanilla JS SPA */
/* global CodeMirror, Viz */

(function() {
'use strict';

// ── State ──────────────────────────────────────────────────────
var NeedleState = {
    runs: {},
    activeRunId: null,
    theme: 'dark',
    settings: {
        autoApprove: false,
        defaultModel: ''
    },
    config: {},          // Full config from GET /api/v1/config
    configLoaded: false,
    sseConnected: false,
    graphZoom: 1.0,
    openTabs: [],
    pendingGates: [],
    dashboardFilter: '',
    selectedRuns: {},
    paused: false,
    pauseResumeAt: null
};

var sseSource = null;
var sseRetryDelay = 1000;
var sseMaxDelay = 30000;
var cmEditor = null;
var vizInstance = null;
var previewDebounceTimer = null;
// Directory the .dot file lives in (also the default project_dir).
var loadedDotDir = '';
// Full path of the .dot file currently in the editor, when it has an
// on-disk home. Empty for typed/generated content that hasn't been saved.
var loadedDotFullPath = '';
// Editor content as last loaded/saved on disk. Used to detect dirty state
// without having to re-read the file. Empty when there is no on-disk home.
var editorBaseline = '';
// 'file'      — content came from disk (loadedDotFullPath set, baseline tracks disk)
// 'generated' — content came from chat or template; no on-disk home yet
// 'typed'     — content was typed/pasted into the editor; no on-disk home
// ''          — empty/initial
var editorOrigin = '';

// ── SSE Manager ────────────────────────────────────────────────
function connectSSE() {
    if (sseSource) {
        sseSource.close();
    }

    sseSource = new EventSource('api/v1/events');

    sseSource.onopen = function() {
        NeedleState.sseConnected = true;
        sseRetryDelay = 1000;
        updateConnDot();
    };

    sseSource.onmessage = function(event) {
        onSSEMessage(event);
    };

    sseSource.onerror = function() {
        NeedleState.sseConnected = false;
        updateConnDot();
        sseSource.close();
        sseSource = null;

        var delay = sseRetryDelay;
        sseRetryDelay = Math.min(sseRetryDelay * 2, sseMaxDelay);
        setTimeout(function() {
            reconcileState();
            connectSSE();
        }, delay);
    };
}

function onSSEMessage(event) {
    var data;
    try { data = JSON.parse(event.data); } catch(e) { return; }

    var runId = data.run_id;
    if (!runId) return;

    if (!NeedleState.runs[runId]) {
        NeedleState.runs[runId] = {
            id: runId, status: 'running', events: [],
            node_statuses: {}, current_node: '', completed_stages: 0,
            total_stages: 0, elapsed_seconds: 0, pending_question: '',
            node_errors: {}, warnings: []
        };
    }

    var run = NeedleState.runs[runId];
    run.events.push(data);

    // Update derived state from event
    switch (data.type) {
        case 'PIPELINE_STARTED':
            run.status = 'running';
            run.start_time = data.timestamp;
            break;
        case 'PIPELINE_COMPLETED':
            run.status = 'completed';
            showToast('Run ' + runId + ' completed', 'success');
            break;
        case 'PIPELINE_FAILED':
            run.status = 'failed';
            if (data.message) {
                run.error = data.message;
            }
            showToast('Run ' + runId + ' failed', 'error');
            break;
        case 'STAGE_STARTED':
            if (data.node_id) {
                run.node_statuses[data.node_id] = 'running';
                run.current_node = data.node_id;
            }
            break;
        case 'STAGE_COMPLETED':
            if (data.node_id) {
                run.node_statuses[data.node_id] = 'completed';
                run.completed_stages = countByStatus(run.node_statuses, 'completed');
                if (run.current_node === data.node_id) run.current_node = '';
            }
            break;
        case 'STAGE_FAILED':
            if (data.node_id) {
                run.node_statuses[data.node_id] = 'failed';
                if (run.current_node === data.node_id) run.current_node = '';
                if (!run.node_errors) run.node_errors = {};
                run.node_errors[data.node_id] = data.data && data.data.error ? data.data.error : data.message;
            }
            break;
        case 'STAGE_WARNING':
        case 'VARIABLE_UNRESOLVED':
        case 'RESUME_WARNING':
            if (!run.warnings) run.warnings = [];
            run.warnings.push(data.message);
            if (data.type === 'RESUME_WARNING') {
                showToast(data.message, 'warning');
            }
            break;
        case 'STAGE_RETRYING':
            if (data.node_id) {
                run.node_statuses[data.node_id] = 'running';
            }
            break;
        case 'HUMAN_QUESTION':
            if (data.data && data.data.interactive) {
                // Interactive session — show chat panel, NOT the gate modal
                run.interactive_active = true;
                run.interactive_node_id = data.data.node_id || data.node_id;
                run.interactive_prompt = data.data.prompt || '';
                run.interactive_context = data.data.context_summary || '';
                run.interactive_pipeline_context = data.data.pipeline_context || '';
                run.pending_question = ''; // Clear so gate modal doesn't show
                showInteractivePanel(runId, {
                    node_id: run.interactive_node_id,
                    node_label: data.data.node_label || run.interactive_node_id,
                    prompt: run.interactive_prompt,
                    context_summary: run.interactive_context,
                    pipeline_context: run.interactive_pipeline_context
                });
            } else {
                run.pending_question = data.message || 'Please provide input';
                run.pending_gate_choices = (data.data && data.data.choices) ? data.data.choices : [];
                run.pending_gate_run_id = runId;
                NeedleState.pendingGates.push({runId: runId, question: run.pending_question, choices: run.pending_gate_choices});
                playGateNotification();
                refreshCurrentView();
            }
            break;
        case 'HUMAN_ANSWER':
            run.pending_question = '';
            break;
        case 'STAGE_PAUSED':
            if (data.node_id) {
                run.node_statuses[data.node_id] = 'paused';
            }
            break;
        case 'PIPELINE_PAUSED':
            NeedleState.paused = true;
            updatePauseUI();
            showToast('Pipeline paused', 'warning');
            break;
        case 'PIPELINE_RESUMED':
            NeedleState.paused = false;
            NeedleState.pauseResumeAt = null;
            updatePauseUI();
            showToast('Pipeline resumed', 'success');
            break;
    }

    // Update elapsed
    if (run.start_time && run.status === 'running') {
        run.elapsed_seconds = (Date.now() - new Date(run.start_time).getTime()) / 1000;
    }

    // Refresh UI
    refreshCurrentView();
    updateTabBar();
    updateFooter();
    updatePipelineLogs();
}

function reconcileState() {
    apiGet('api/v1/runs').then(function(runs) {
        if (!Array.isArray(runs)) return;
        runs.forEach(function(rv) {
            NeedleState.runs[rv.id] = NeedleState.runs[rv.id] || {};
            Object.keys(rv).forEach(function(k) {
                NeedleState.runs[rv.id][k] = rv[k];
            });
        });
        refreshCurrentView();
        updateTabBar();
        updateFooter();
    });
}

function updateConnDot() {
    var dot = document.getElementById('ndl-conn');
    if (!dot) return;
    dot.className = 'ndl-conn-dot ' + (NeedleState.sseConnected ? 'connected' : 'disconnected');
    dot.title = NeedleState.sseConnected ? 'Connected' : 'Disconnected';
}

// ── Router ─────────────────────────────────────────────────────
function navigate(view, param) {
    if (param) {
        location.hash = '#' + view + '/' + param;
    } else {
        location.hash = '#' + view;
    }
}

function onHashChange() {
    var hash = location.hash.slice(1) || 'dashboard';
    var parts = hash.split('/');
    var view = parts[0];
    var param = parts[1] || null;

    // Activate nav button
    var btns = document.querySelectorAll('.ndl-nav-btn');
    for (var i = 0; i < btns.length; i++) {
        btns[i].classList.toggle('active', btns[i].getAttribute('data-view') === view);
    }

    // Stop needle log polling when leaving logs view
    if (view !== 'logs') stopNeedleLogPolling();

    // Show active view
    var views = document.querySelectorAll('.ndl-view');
    for (var j = 0; j < views.length; j++) {
        views[j].classList.toggle('active', views[j].id === 'ndl-view-' + view);
    }

    // Render view content
    switch (view) {
        case 'dashboard': renderDashboard(); break;
        case 'monitor': renderMonitor(param); break;
        case 'create': renderCreate(); break;
        case 'settings': renderSettings(); break;
        case 'logs': renderLogs(); break;
        case 'help': renderHelp(param); break;
    }
}

function refreshCurrentView() {
    var hash = location.hash.slice(1) || 'dashboard';
    var parts = hash.split('/');
    switch (parts[0]) {
        case 'dashboard': renderDashboard(); break;
        case 'monitor': renderMonitor(parts[1]); break;
    }
}

// ── API ────────────────────────────────────────────────────────
// Strip leading slash so paths resolve relative to <base href>, which
// reverse proxies typically set to /proxy/{id}/. Without a <base> tag
// (direct access) relative paths resolve to the origin root — same behavior.
function apiUrl(path) {
    return path.charAt(0) === '/' ? path.substring(1) : path;
}

function apiGet(path) {
    return fetch(apiUrl(path)).then(function(r) {
        if (!r.ok) throw new Error(r.status);
        return r.json();
    });
}

function apiPost(path, body) {
    return fetch(apiUrl(path), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: body ? JSON.stringify(body) : undefined
    }).then(function(r) {
        return r.json();
    });
}

function apiPut(path, body) {
    return fetch(apiUrl(path), {
        method: 'PUT',
        headers: {'Content-Type': 'application/json'},
        body: body ? JSON.stringify(body) : undefined
    }).then(function(r) {
        return r.json();
    });
}

function apiDelete(path) {
    return fetch(apiUrl(path), { method: 'DELETE' }).then(function(r) { return r.json(); });
}

// Start a run. `opts` may contain:
//   dot_path:    file path the server should read (preferred when the
//                editor is backed by a saved file)
//   dot_source:  raw DOT contents (used only for unsaved generated
//                content; the server stashes one canonical copy under
//                <project_dir>/.needle/<stem>/source.dot)
//   project_dir: directory to run in
//   vars:        optional template variables
function apiStartRun(opts) {
    var body = {};
    if (opts.dot_path) body.dot_path = opts.dot_path;
    if (opts.dot_source) body.dot_source = opts.dot_source;
    if (opts.project_dir) body.project_dir = opts.project_dir;
    if (opts.vars) body.vars = opts.vars;
    if (NeedleState.settings.autoApprove) body.auto_approve = true;
    return apiPost('api/v1/runs', body);
}

function apiCancelRun(id) {
    return apiPost('api/v1/runs/' + encodeURIComponent(id) + '/cancel');
}

function apiSubmitAnswer(id, answerJson) {
    // answerJson is already a JSON string with {selected_index, raw_input}
    return fetch('api/v1/runs/' + encodeURIComponent(id) + '/answer', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: answerJson
    }).then(function(r) {
        // Reject on non-2xx so the caller's catch handler sees the real
        // status. Without this, an empty 404 body causes r.json() to
        // throw with no useful detail in the resulting toast.
        return r.text().then(function(text) {
            if (!r.ok) {
                var msg = 'HTTP ' + r.status;
                try {
                    var parsed = JSON.parse(text);
                    if (parsed && parsed.error) msg += ': ' + parsed.error;
                } catch (_) {
                    if (text) msg += ': ' + text.substring(0, 200);
                }
                throw new Error(msg);
            }
            return text ? JSON.parse(text) : {};
        });
    });
}

// ── Dashboard View ─────────────────────────────────────────────
function renderDashboard() {
    renderStatsCards();
    renderRunGrid();
}

function renderStatsCards() {
    var el = document.getElementById('ndl-stats');
    if (!el) return;

    var runs = Object.values(NeedleState.runs);
    var total = runs.length;
    var running = runs.filter(function(r) { return r.status === 'running'; }).length;
    var completed = runs.filter(function(r) { return r.status === 'completed'; }).length;
    var failed = runs.filter(function(r) { return r.status === 'failed'; }).length;

    el.innerHTML =
        statCard('Total', total, '', '') +
        statCard('Running', running, 'running', 'running') +
        statCard('Completed', completed, 'completed', 'completed') +
        statCard('Failed', failed, 'failed', 'failed');

    // Attach filter click handlers
    var cards = el.querySelectorAll('.ndl-stat-card');
    for (var i = 0; i < cards.length; i++) {
        cards[i].addEventListener('click', (function(filter) {
            return function() {
                NeedleState.dashboardFilter = (NeedleState.dashboardFilter === filter) ? '' : filter;
                renderDashboard();
            };
        })(cards[i].getAttribute('data-filter')));
    }
}

function statCard(label, value, cls, filter) {
    var active = NeedleState.dashboardFilter === filter;
    return '<div class="ndl-stat-card' + (active ? ' ndl-stat-active' : '') +
        '" data-filter="' + filter + '" style="cursor:pointer">' +
        '<div class="ndl-stat-label">' + esc(label) + '</div>' +
        '<div class="ndl-stat-value' + (cls ? ' ndl-badge-' + cls : '') + '">' + value + '</div>' +
        '</div>';
}

function renderRunGrid() {
    var el = document.getElementById('ndl-runs-grid');
    if (!el) return;

    var runs = Object.values(NeedleState.runs);
    if (runs.length === 0) {
        el.innerHTML = '<div class="ndl-empty">' +
            '<div class="ndl-empty-icon">&#9654;</div>' +
            '<div class="ndl-empty-text">No pipeline runs yet</div>' +
            '<div class="ndl-empty-hint">Click "New Run" in the Monitor view or use the Create view</div>' +
            '</div>';
        return;
    }

    // Apply status filter
    var filter = NeedleState.dashboardFilter || '';
    if (filter) {
        runs = runs.filter(function(r) { return r.status === filter; });
    }

    // Sort: running first, then by newest
    runs.sort(function(a, b) {
        if (a.status === 'running' && b.status !== 'running') return -1;
        if (b.status === 'running' && a.status !== 'running') return 1;
        return 0;
    });

    // Selection toolbar
    var selected = NeedleState.selectedRuns || {};
    var selectedCount = Object.keys(selected).length;
    var toolbar = '';
    if (selectedCount > 0) {
        toolbar = '<div class="ndl-select-toolbar">' +
            '<span>' + selectedCount + ' selected</span>' +
            '<button id="ndl-discard-selected" class="ndl-btn ndl-btn-danger" style="margin-left:8px">Discard selected</button>' +
            '<button id="ndl-clear-selection" class="ndl-btn" style="margin-left:4px">Clear selection</button>' +
            '</div>';
    }

    el.innerHTML = toolbar + runs.map(function(run) { return renderRunCard(run); }).join('');

    // Attach toolbar handlers
    var discardBtn = document.getElementById('ndl-discard-selected');
    if (discardBtn) {
        discardBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            var ids = Object.keys(NeedleState.selectedRuns || {});
            if (!ids.length) return;
            if (!confirm('Discard ' + ids.length + ' run(s) and all their artifacts?\\nThis cannot be undone.')) return;
            var done = 0;
            ids.forEach(function(id) {
                apiDelete('api/v1/runs/' + encodeURIComponent(id) + '?artifacts=true').then(function() {
                    delete NeedleState.runs[id];
                    done++;
                    if (done === ids.length) {
                        NeedleState.selectedRuns = {};
                        refreshCurrentView();
                    }
                });
            });
        });
    }
    var clearBtn = document.getElementById('ndl-clear-selection');
    if (clearBtn) {
        clearBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            NeedleState.selectedRuns = {};
            renderDashboard();
        });
    }

    // Attach click handlers
    var cards = el.querySelectorAll('.ndl-run-card');
    for (var i = 0; i < cards.length; i++) {
        (function(card) {
            var id = card.getAttribute('data-run-id');
            var checkbox = card.querySelector('.ndl-run-select');
            if (checkbox) {
                checkbox.addEventListener('click', function(e) {
                    e.stopPropagation();
                    if (this.checked) {
                        if (!NeedleState.selectedRuns) NeedleState.selectedRuns = {};
                        NeedleState.selectedRuns[id] = true;
                    } else {
                        delete NeedleState.selectedRuns[id];
                    }
                    renderDashboard();
                });
            }
            card.addEventListener('click', function() {
                openRunTab(id);
                navigate('monitor', id);
            });
        })(cards[i]);
    }
}

function renderRunCard(run) {
    var progress = 0;
    if (run.total_stages > 0) {
        progress = Math.round((run.completed_stages / run.total_stages) * 100);
    }
    if (run.status === 'completed') progress = 100;

    var isSelected = NeedleState.selectedRuns && NeedleState.selectedRuns[run.id];
    var canSelect = run.status !== 'running';
    var failSummary = '';
    if (run.status === 'failed' && run.error) {
        // Truncate to first meaningful line, max ~60 chars
        var errText = run.error.replace(/^(stage failed|edge selection failed|Goal gate unsatisfied): /, '');
        if (errText.length > 60) errText = errText.substring(0, 57) + '...';
        failSummary = '<div class="ndl-run-error-summary">' + esc(errText) + '</div>';
    }

    return '<div class="ndl-run-card' + (isSelected ? ' ndl-run-selected' : '') +
        '" data-run-id="' + esc(run.id) + '">' +
        '<div class="ndl-run-card-header">' +
        (canSelect ? '<input type="checkbox" class="ndl-run-select"' +
            (isSelected ? ' checked' : '') + '>' : '') +
        '<span class="ndl-run-id">' + esc(run.id) + '</span>' +
        '<span class="ndl-badge ndl-badge-' + esc(run.status) + '">' + esc(run.status) + '</span>' +
        '</div>' +
        '<div class="ndl-run-meta">' +
        (run.current_node ? 'Stage: ' + esc(run.current_node) : '') +
        '<span class="ndl-run-duration">' + fmtDuration(run.elapsed_seconds || 0) + '</span>' +
        '</div>' +
        failSummary +
        '<div class="ndl-progress-bar">' +
        '<div class="ndl-progress-fill' + (run.status === 'running' ? ' running' : '') +
        '" style="width:' + progress + '%"></div>' +
        '</div>' +
        '</div>';
}

// ── Tab Bar ────────────────────────────────────────────────────
function openRunTab(runId) {
    if (NeedleState.openTabs.indexOf(runId) === -1) {
        NeedleState.openTabs.push(runId);
    }
    NeedleState.activeRunId = runId;
    updateTabBar();
}

function closeRunTab(runId) {
    var idx = NeedleState.openTabs.indexOf(runId);
    if (idx !== -1) NeedleState.openTabs.splice(idx, 1);
    if (NeedleState.activeRunId === runId) {
        NeedleState.activeRunId = NeedleState.openTabs.length > 0 ?
            NeedleState.openTabs[NeedleState.openTabs.length - 1] : null;
    }
    updateTabBar();
    if (NeedleState.activeRunId) {
        navigate('monitor', NeedleState.activeRunId);
    } else {
        navigate('dashboard');
    }
}

function updateTabBar() {
    var el = document.getElementById('ndl-tab-bar');
    if (!el) return;

    if (NeedleState.openTabs.length === 0) {
        el.innerHTML = '';
        return;
    }

    el.innerHTML = NeedleState.openTabs.map(function(id) {
        var run = NeedleState.runs[id] || {status: 'unknown'};
        var active = id === NeedleState.activeRunId;
        var isRunning = run.status === 'running';
        var hasPendingGate = run.pending_question && run.pending_question.length > 0;
        var statusColor = hasPendingGate ? 'var(--warning)' :
            run.status === 'running' ? 'var(--warning)' :
            run.status === 'completed' ? 'var(--success)' :
            run.status === 'failed' ? 'var(--danger)' : 'var(--text-muted)';

        var trashBtn = !isRunning ?
            '<span class="ndl-tab-trash" data-trash-id="' + esc(id) + '" title="Delete run and artifacts">&#x1F5D1;</span>' : '';

        return '<button class="ndl-tab' + (active ? ' active' : '') + '" data-tab-id="' + esc(id) + '">' +
            '<span class="ndl-tab-status' + (hasPendingGate ? ' ndl-tab-status-gate' : '') + '" style="background:' + statusColor + '"></span>' +
            '<span>' + esc(id) + '</span>' +
            trashBtn +
            '<span class="ndl-tab-close" data-close-id="' + esc(id) + '" title="Close tab">&times;</span>' +
            '</button>';
    }).join('');

    // Click handlers
    var tabs = el.querySelectorAll('.ndl-tab');
    for (var i = 0; i < tabs.length; i++) {
        tabs[i].addEventListener('click', (function(id) {
            return function(e) {
                if (e.target.classList.contains('ndl-tab-close')) {
                    var closeId = e.target.getAttribute('data-close-id');
                    closeRunTab(closeId);
                } else if (e.target.classList.contains('ndl-tab-trash')) {
                    var trashId = e.target.getAttribute('data-trash-id');
                    if (confirm('Delete run "' + trashId + '" and all its artifacts?\\nThis cannot be undone.')) {
                        apiDelete('api/v1/runs/' + encodeURIComponent(trashId) + '?artifacts=true').then(function() {
                            delete NeedleState.runs[trashId];
                            closeRunTab(trashId);
                            showToast('Run deleted', 'success');
                        });
                    }
                } else {
                    NeedleState.activeRunId = id;
                    navigate('monitor', id);
                }
            };
        })(tabs[i].getAttribute('data-tab-id')));
    }
}

// ── Monitor View ───────────────────────────────────────────────
function renderMonitor(runId) {
    if (runId) {
        NeedleState.activeRunId = runId;
        openRunTab(runId);
    }

    var run = NeedleState.runs[NeedleState.activeRunId];

    renderActionBar(run);
    renderWarningsBanner(run);
    renderGateBanner(run);
    renderStageList(run);
    renderGraphToolbar();

    // Render graph in monitor panel
    var gc = document.getElementById('ndl-graph-container');
    if (gc) {
        var svg = gc.querySelector('svg');
        var currentRunId = run ? run.id : null;

        if (svg && gc._renderedForRun === currentRunId) {
            // Already rendered for this run — just ensure classes are injected
            if (!svg._classesInjected) {
                svg._classesInjected = true;
                injectSvgNodeClasses(svg);
                setupNodeClickHandlers(gc, 'monitor');
            }
        } else {
            // Need to render (or re-render for a different run)
            var dotSource = run && run.dot_source;

            if (dotSource) {
                renderDotIntoContainer(gc, dotSource, 'monitor');
                // _renderedForRun is set inside renderDotIntoContainer's async
                // callback (after SVG is actually in the DOM). Don't set it here
                // or updateGraphStatus will run against a non-existent SVG.
                gc._pendingRenderForRun = currentRunId;
            } else if (svg && !svg._classesInjected) {
                // Server-rendered SVG present but classes not injected yet
                svg._classesInjected = true;
                injectSvgNodeClasses(svg);
                setupNodeClickHandlers(gc, 'monitor');
                gc._renderedForRun = currentRunId;
            } else if (!dotSource && !gc._fetchingDot) {
                // No DOT source — fetch from server
                gc._fetchingDot = true;
                fetch('api/v1/graph/dot')
                    .then(function(r) { return r.text(); })
                    .then(function(dot) {
                        gc._fetchingDot = false;
                        if (dot && dot.trim()) {
                            if (run) run.dot_source = dot;
                            renderDotIntoContainer(gc, dot, 'monitor');
                            gc._renderedForRun = currentRunId;
                        } else if (!gc.querySelector('svg') && !gc.querySelector('.ndl-graph-fallback')) {
                            gc.innerHTML = '<div class="ndl-graph-fallback">No graph available</div>';
                        }
                    })
                    .catch(function() { gc._fetchingDot = false; });
            } else if (!svg && !gc.querySelector('.ndl-graph-fallback')) {
                gc.innerHTML = '<div class="ndl-graph-fallback">No graph available</div>';
            }
        }
    }

    if (run) {
        // Only update graph status if SVG is actually rendered (not pending async)
        if (!gc || !gc._pendingRenderForRun) {
            updateGraphStatus(run.node_statuses || {});
        }
        checkInteractiveState(run);
    }
}

var interactiveChatMessages = [];
var interactiveGreetingSent = false;
var interactivePanelShown = false;

function checkInteractiveState(run) {
    if (!run || run.status !== 'running') {
        hideInteractivePanel();
        return;
    }

    // Don't poll if panel is already shown and active
    if (interactivePanelShown && run.interactive_active) return;

    fetch('api/v1/runs/' + encodeURIComponent(run.id) + '/interactive')
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.active && !interactivePanelShown) {
                showInteractivePanel(run.id, data);
            } else if (!data.active) {
                hideInteractivePanel();
            }
        })
        .catch(function() {});
}

function showInteractivePanel(runId, data) {
    var panel = document.getElementById('ndl-interactive-panel');
    if (!panel) return;

    interactivePanelShown = true;
    panel.style.display = 'block';
    panel.setAttribute('data-run-id', runId);

    var title = document.getElementById('ndl-interactive-title');
    if (title) title.textContent = 'Interactive: ' + (data.node_id || 'stage');

    var context = document.getElementById('ndl-interactive-context');
    if (context && data.context_summary) {
        context.innerHTML = '<pre class="ndl-stage-pre" style="max-height:150px">' + esc(data.context_summary) + '</pre>';
    }

    // Restore persisted chat history if reconnecting, otherwise auto-greet
    if (!interactiveGreetingSent) {
        interactiveGreetingSent = true;
        if (data.chat_history && data.chat_history.length > 0) {
            data.chat_history.forEach(function(m) {
                addInteractiveChatMessage(m.role, m.content);
            });
            // If the last message was from the user (no assistant response due to
            // a timeout/disconnect), automatically request the assistant response.
            var lastMsg = data.chat_history[data.chat_history.length - 1];
            if (lastMsg && lastMsg.role === 'user') {
                sendInteractiveChat(runId, lastMsg.content);
            }
        } else {
            addInteractiveChatMessage('assistant', 'Starting interactive session...');
            sendInteractiveChat(runId, 'Greet the user based on the context and help them develop their idea. Be warm, specific about what they want to build, and suggest some starting directions.');
        }
    }

    // Wire up Send button for AI chat via /interactive/chat
    var sendBtn = document.getElementById('ndl-interactive-send');
    if (sendBtn && !sendBtn._wired) {
        sendBtn._wired = true;
        sendBtn.addEventListener('click', function() {
            var input = document.getElementById('ndl-interactive-input');
            if (!input || !input.value.trim()) return;
            var msg = input.value.trim();
            input.value = '';
            addInteractiveChatMessage('user', msg);
            var currentRunId = panel.getAttribute('data-run-id') || runId;
            sendInteractiveChat(currentRunId, msg);
        });
    }

    // Wire up Continue button
    var continueBtn = document.getElementById('ndl-interactive-continue');
    if (continueBtn && !continueBtn._wired) {
        continueBtn._wired = true;
        continueBtn.addEventListener('click', function() {
            var currentRunId = panel.getAttribute('data-run-id') || runId;

            // If the user has typed something but not hit Send, ask what
            // they want to do — a common flow is "type a final note, then
            // Continue" and that note shouldn't be silently dropped.
            var input = document.getElementById('ndl-interactive-input');
            var pendingNote = (input && input.value) ? input.value.trim() : '';
            if (pendingNote) {
                var proceed = window.confirm(
                    'You have unsent text in the message box.\n\n' +
                    'OK = Continue now — your note will be sent to the agent as ' +
                    'a final user turn and incorporated into the summary.\n\n' +
                    'Cancel = Go back and click Send to do another full turn first.'
                );
                if (!proceed) return;  // leave textarea intact
                // Append the note as a real user turn so it appears in the
                // rendered history and the summary call sees it.
                input.value = '';
                addInteractiveChatMessage('user', pendingNote);
            }

            continueBtn.disabled = true;
            continueBtn.textContent = 'Summarizing...';

            // Ask the AI to produce a summary, then continue
            var history = interactiveChatMessages.map(function(m) {
                return {role: m.role, content: m.content};
            });
            var summaryMessage = 'Please produce a comprehensive summary document of our collaboration.';
            if (pendingNote) {
                summaryMessage += ' The user added a final note just now (last user turn above) — ' +
                                   'weave it into the summary rather than treating it as a new question to answer.';
            }
            fetch('api/v1/runs/' + encodeURIComponent(currentRunId) + '/interactive/chat', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({
                    message: summaryMessage,
                    history: history
                })
            })
            .then(function(r) { return r.json(); })
            .then(function(resp) {
                var summary = resp.response || '';
                addInteractiveChatMessage('assistant', summary);

                // Now continue the pipeline with the summary as result
                return fetch('api/v1/runs/' + encodeURIComponent(currentRunId) + '/continue', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({result: summary})
                });
            })
            .then(function() {
                hideInteractivePanel();
                interactiveChatMessages = [];
                showToast('Continuing pipeline', 'success');
            })
            .catch(function() {
                showToast('Failed to continue', 'error');
            })
            .then(function() {
                continueBtn.disabled = false;
                continueBtn.textContent = 'Continue';
            });
        });
    }
}

function showThinkingIndicator() {
    var chat = document.getElementById('ndl-interactive-chat');
    if (!chat) return;
    var existing = document.getElementById('ndl-thinking-indicator');
    if (existing) return;
    var div = document.createElement('div');
    div.id = 'ndl-thinking-indicator';
    div.className = 'ndl-chat-msg ndl-chat-assistant ndl-thinking';
    div.innerHTML = '<span class="ndl-thinking-dots"><span>.</span><span>.</span><span>.</span></span>';
    chat.appendChild(div);
    chat.scrollTop = chat.scrollHeight;
}

function hideThinkingIndicator() {
    var el = document.getElementById('ndl-thinking-indicator');
    if (el) el.remove();
}

function setInteractiveSendEnabled(enabled) {
    var btn = document.getElementById('ndl-interactive-send');
    if (btn) btn.disabled = !enabled;
}

function sendInteractiveChat(runId, message) {
    var history = interactiveChatMessages.map(function(m) {
        return {role: m.role, content: m.content};
    });
    showThinkingIndicator();
    setInteractiveSendEnabled(false);
    fetch('api/v1/runs/' + encodeURIComponent(runId) + '/interactive/chat', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            message: message,
            history: history,
            agent: NeedleState.settings.chatAgent || 'claude',
            model: NeedleState.settings.chatModel || 'claude-sonnet-4-6'
        })
    })
    .then(function(r) { return r.json(); })
    .then(function(resp) {
        hideThinkingIndicator();
        setInteractiveSendEnabled(true);
        var reply = resp.response || resp.error || 'No response';
        addInteractiveChatMessage('assistant', reply);
    })
    .catch(function() {
        hideThinkingIndicator();
        setInteractiveSendEnabled(true);
        addInteractiveChatMessage('assistant', 'Failed to get AI response');
    });
}

function hideInteractivePanel() {
    var panel = document.getElementById('ndl-interactive-panel');
    if (panel) panel.style.display = 'none';
    interactivePanelShown = false;
    interactiveGreetingSent = false;
    interactiveChatMessages = [];
    // Clear chat display
    var chat = document.getElementById('ndl-interactive-chat');
    if (chat) chat.innerHTML = '';
}

function addInteractiveChatMessage(role, content) {
    interactiveChatMessages.push({role: role, content: content});
    var chat = document.getElementById('ndl-interactive-chat');
    if (!chat) return;
    var div = document.createElement('div');
    div.className = 'ndl-chat-msg ndl-chat-' + role;
    div.textContent = content;
    chat.appendChild(div);
    chat.scrollTop = chat.scrollHeight;
}

function renderActionBar(run) {
    var el = document.getElementById('ndl-action-bar');
    if (!el) return;

    var html = '';
    var hasDot = run && run.dot_source;

    // Load DOT — always visible in monitor
    html += '<button class="ndl-btn" id="ndl-monitor-load-dot">Load DOT</button>';

    // New Run — only when DOT is available (from run or loaded file)
    if (hasDot) {
        html += '<button class="ndl-btn-primary" id="ndl-new-run-btn">New Run</button>';
    }

    // Resume Run — for failed or cancelled runs
    if (run && (run.status === 'failed' || run.status === 'cancelled')) {
        html += '<button class="ndl-btn-primary" id="ndl-resume-run-btn">Resume Run</button>';
    }

    // Cancel — for running runs
    if (run && run.status === 'running') {
        html += '<button class="ndl-btn-danger" id="ndl-cancel-btn">Cancel</button>';
    }

    el.innerHTML = html;

    // Wire Load DOT button
    var loadDotBtn = document.getElementById('ndl-monitor-load-dot');
    if (loadDotBtn) {
        loadDotBtn.addEventListener('click', function() {
            showDotFilePicker(function(dotSource, filePath, dirPath) {
                // Check for existing run for this DOT
                apiPost('api/v1/check-run', {
                    dot_source: dotSource,
                    project_dir: dirPath
                }).then(function(data) {
                    if (data.has_previous_run && data.previous_run_id) {
                        openRunTab(data.previous_run_id);
                        navigate('monitor', data.previous_run_id);
                    } else {
                        // No previous run — create a virtual entry with disk status
                        var virtualId = 'loaded-' + (data.dot_stem || 'design');
                        NeedleState.runs[virtualId] = {
                            id: virtualId,
                            status: data.has_checkpoint ? 'failed' : 'pending',
                            dot_source: dotSource,
                            project_dir: dirPath,
                            dot_stem: data.dot_stem,
                            node_statuses: data.node_statuses || {},
                            completed_stages: data.completed_stages || 0,
                            total_stages: data.total_stages || 0,
                            warnings: []
                        };
                        openRunTab(virtualId);
                        navigate('monitor', virtualId);
                        if (data.has_checkpoint) {
                            showToast('Checkpoint found. Click Resume to continue.', 'info');
                        }
                    }
                });
            });
        });
    }

    // Wire New Run button
    var newRunBtn = document.getElementById('ndl-new-run-btn');
    if (newRunBtn && run) {
        newRunBtn.addEventListener('click', function() {
            var dotSource = run.dot_source;
            if (!dotSource) {
                showToast('No DOT loaded', 'error');
                return;
            }
            showDirectoryPicker(function(projectDir) {
                // Re-running from an existing run: send the source as-is.
                // The server will stash it (or reuse the existing copy)
                // under <project_dir>/.needle/<stem>/source.dot.
                apiStartRun({dot_source: dotSource, project_dir: projectDir}).then(function(data) {
                    if (data && data.id) {
                        openRunTab(data.id);
                        navigate('monitor', data.id);
                        showToast('Run ' + data.id + ' started', 'success');
                    } else if (data && data.error) {
                        showToast(data.error, 'error');
                    }
                });
            }, run.project_dir || undefined);
        });
    }

    // Wire Resume Run button
    var resumeBtn = document.getElementById('ndl-resume-run-btn');
    if (resumeBtn && run) {
        resumeBtn.addEventListener('click', function() {
            var projectDir = run.project_dir || '.';
            var oldRunId = run.id;
            var body = {
                project_dir: projectDir,
                dot_stem: run.dot_stem,
                replace_run_id: oldRunId
            };
            // Prefer a recorded dot_path — the server then reads from disk
            // and doesn't have to fall back to checkpoint paths or stash.
            if (run.dot_path) body.dot_path = run.dot_path;
            apiPost('api/v1/resume', body).then(function(data) {
                if (data.error) {
                    showToast('Resume failed: ' + data.error, 'error');
                    return;
                }
                if (data.id) {
                    // Remove old run from state and close its tab
                    delete NeedleState.runs[oldRunId];
                    closeRunTab(oldRunId);

                    // Pre-create the run entry with dot_source BEFORE SSE events arrive
                    NeedleState.runs[data.id] = {
                        id: data.id, status: 'running', events: [],
                        node_statuses: {}, current_node: '', completed_stages: 0,
                        total_stages: 0, elapsed_seconds: 0, pending_question: '',
                        node_errors: {}, warnings: [],
                        dot_source: data.dot_source || run.dot_source,
                        project_dir: run.project_dir,
                        dot_stem: run.dot_stem
                    };
                    openRunTab(data.id);
                    navigate('monitor', data.id);
                    showToast('Resumed from ' + (data.resumed_from || 'checkpoint'), 'success');
                }
            }).catch(function(err) {
                showToast('Resume failed: ' + err.message, 'error');
            });
        });
    }

    // Wire Cancel button
    var cancelBtn = document.getElementById('ndl-cancel-btn');
    if (cancelBtn && run) {
        cancelBtn.addEventListener('click', function() {
            if (confirm('Cancel run ' + run.id + '?')) {
                apiCancelRun(run.id).then(function() {
                    run.status = 'cancelled';
                    refreshCurrentView();
                    showToast('Run ' + run.id + ' cancelled', 'success');
                });
            }
        });
    }
}

function renderWarningsBanner(run) {
    var el = document.getElementById('ndl-warnings-banner');
    if (!el) return;

    if (!run || !run.warnings || run.warnings.length === 0) {
        el.style.display = 'none';
        el.innerHTML = '';
        return;
    }

    el.style.display = 'block';
    var html = '<strong>Warnings (' + run.warnings.length + '):</strong><ul>';
    run.warnings.forEach(function(w) {
        html += '<li>' + esc(w) + '</li>';
    });
    html += '</ul>';
    el.innerHTML = html;
}

function renderStageList(run) {
    var el = document.getElementById('ndl-stage-list');
    if (!el) return;

    if (!run) {
        el.innerHTML = '<div class="ndl-empty">' +
            '<div class="ndl-empty-text">Select a run to monitor</div>' +
            '<div class="ndl-empty-hint">Start a new run or select one from the Dashboard</div>' +
            '</div>';
        return;
    }

    var nodes = Object.keys(run.node_statuses || {});
    if (nodes.length === 0 && run.events && run.events.length > 0) {
        // Extract node IDs from events
        run.events.forEach(function(e) {
            if (e.node_id && nodes.indexOf(e.node_id) === -1) {
                nodes.push(e.node_id);
            }
        });
    }

    if (nodes.length === 0) {
        el.innerHTML = '<div class="ndl-empty"><div class="ndl-empty-text">Waiting for stages...</div></div>';
        return;
    }

    el.innerHTML = nodes.map(function(nodeId) {
        var status = (run.node_statuses || {})[nodeId] || 'pending';
        var events = (run.events || []).filter(function(e) { return e.node_id === nodeId; });
        var errorMsg = (run.node_errors && run.node_errors[nodeId]) ? run.node_errors[nodeId] : '';
        return renderStageEntry(nodeId, status, events, errorMsg);
    }).join('');

    // Click stage to toggle detail view
    var headers = el.querySelectorAll('.ndl-stage-header');
    for (var i = 0; i < headers.length; i++) {
        headers[i].addEventListener('click', function() {
            var stage = this.parentElement;
            var nodeId = this.querySelector('.ndl-stage-name').textContent;
            stage.classList.toggle('open');

            // Fetch stage detail on first open
            var detail = stage.querySelector('.ndl-stage-detail');
            if (stage.classList.contains('open') && detail && !detail.dataset.loaded) {
                detail.dataset.loaded = 'true';
                detail.innerHTML = '<div class="ndl-stage-loading">Loading...</div>';
                fetch('api/v1/stages/' + encodeURIComponent(nodeId) + '?run_id=' + encodeURIComponent(run.id))
                    .then(function(r) { return r.json(); })
                    .then(function(data) {
                        var html = '';
                        // Show error detail at top of expanded section
                        if (run.node_errors && run.node_errors[nodeId]) {
                            html += '<div class="ndl-stage-error"><strong>Error:</strong> ' + esc(run.node_errors[nodeId]) + '</div>';
                        }
                        if (data.prompt) {
                            html += '<div class="ndl-stage-section"><strong>Prompt:</strong><pre class="ndl-stage-pre">' + esc(data.prompt.substring(0, 2000)) + (data.prompt.length > 2000 ? '\n[truncated]' : '') + '</pre></div>';
                        }
                        if (data.response) {
                            // Try to extract result from JSON response
                            var responseText = data.response;
                            try {
                                var parsed = JSON.parse(data.response);
                                if (parsed.result) responseText = parsed.result;
                            } catch(e) {}
                            html += '<div class="ndl-stage-section"><strong>Response:</strong><pre class="ndl-stage-pre">' + esc(responseText.substring(0, 3000)) + (responseText.length > 3000 ? '\n[truncated]' : '') + '</pre></div>';
                        }
                        if (!data.prompt && !data.response && !(run.node_errors && run.node_errors[nodeId])) {
                            html = '<div class="ndl-stage-section">No details available</div>';
                        }
                        detail.innerHTML = html;
                    })
                    .catch(function() {
                        detail.innerHTML = '<div class="ndl-stage-section">Failed to load details</div>';
                    });
            }
        });
    }
}

function renderStageEntry(nodeId, status, events, errorMsg) {
    var icon = status === 'completed' ? '&#10004;' :
        status === 'running' ? '&#9881;' :
        status === 'failed' ? '&#10008;' :
        status === 'paused' ? '&#9208;' : '&#9675;';

    var logs = events.map(function(e) {
        return '[' + fmtTimestamp(e.timestamp) + '] ' + (e.message || e.type);
    }).join('\n');

    var errorHtml = '';
    if (status === 'failed' && errorMsg) {
        errorHtml = '<div class="ndl-stage-error"><strong>Error:</strong> ' + esc(errorMsg) + '</div>';
    }

    return '<div class="ndl-stage">' +
        '<div class="ndl-stage-header">' +
        '<span class="ndl-stage-icon ' + esc(status) + '">' + icon + '</span>' +
        '<span class="ndl-stage-name">' + esc(nodeId) + '</span>' +
        '<span class="ndl-stage-status">' + esc(status) + '</span>' +
        '</div>' +
        errorHtml +
        '<div class="ndl-stage-detail"></div>' +
        '<div class="ndl-stage-logs">' + esc(logs || 'No log entries') + '</div>' +
        '</div>';
}

function updateGraphStatus(nodeStatuses) {
    var container = document.getElementById('ndl-graph-container');
    if (!container) return;

    // Remove status classes from all node groups
    var nodeGroups = container.querySelectorAll('g.node');
    for (var i = 0; i < nodeGroups.length; i++) {
        nodeGroups[i].classList.remove('ndl-node-running', 'ndl-node-completed', 'ndl-node-failed', 'ndl-node-paused');
    }

    // Apply new status classes by matching node ID via title element
    Object.keys(nodeStatuses).forEach(function(nodeId) {
        var status = nodeStatuses[nodeId];
        // Try classList-based lookup first (our injected classes)
        var el = container.querySelector('.ndl-node-' + nodeId);
        if (!el) {
            // Fallback: find by title content
            var titles = container.querySelectorAll('g.node > title');
            for (var i = 0; i < titles.length; i++) {
                if (titles[i].textContent.trim() === nodeId) {
                    el = titles[i].parentElement;
                    break;
                }
            }
        }
        if (el) {
            el.classList.add('ndl-node-' + status);
        }
    });
}

// ── Graph Pan/Zoom (generic, works with any container) ────────
// Each container stores its own zoom/pan state as _gz properties.

function renderGraphToolbar() {
    setupGraphToolbarFor('ndl-graph-toolbar', 'ndl-graph-container');
}

function setupGraphToolbarFor(toolbarId, containerId) {
    var el = document.getElementById(toolbarId);
    if (!el) return;

    el.innerHTML =
        '<button class="ndl-gz-in" title="Zoom in">+</button>' +
        '<button class="ndl-gz-fit" title="Fit to view">Fit</button>' +
        '<button class="ndl-gz-reset" title="Reset zoom">1:1</button>' +
        '<button class="ndl-gz-out" title="Zoom out">&minus;</button>' +
        '<span class="ndl-zoom-hint" style="font-size:11px;color:var(--text-muted);margin-left:8px">Scroll to zoom, drag to pan</span>';

    var container = document.getElementById(containerId);
    el.querySelector('.ndl-gz-in').addEventListener('click', function() { zoomGraphContainer(container, 1.25); });
    el.querySelector('.ndl-gz-out').addEventListener('click', function() { zoomGraphContainer(container, 0.8); });
    el.querySelector('.ndl-gz-reset').addEventListener('click', function() {
        if (!container) return;
        container._gzZoom = 1.0;
        container._gzPanX = 0;
        container._gzPanY = 0;
        applyGraphTransformFor(container);
    });
    el.querySelector('.ndl-gz-fit').addEventListener('click', function() { fitGraphToViewFor(container); });

    setupGraphPanZoom(container);
}

function setupGraphPanZoom(container) {
    if (!container || container._panZoomSetup) return;
    container._panZoomSetup = true;

    container._gzZoom = container._gzZoom || 1.0;
    container._gzPanX = container._gzPanX || 0;
    container._gzPanY = container._gzPanY || 0;

    var isDragging = false;
    var lastX = 0, lastY = 0;

    container.addEventListener('mousedown', function(e) {
        if (e.button !== 0) return;
        // Don't start drag if clicking on a node (let click handler fire)
        if (e.target.closest && e.target.closest('g.node')) return;
        isDragging = true;
        lastX = e.clientX;
        lastY = e.clientY;
        container.style.cursor = 'grabbing';
        e.preventDefault();
    });

    window.addEventListener('mousemove', function(e) {
        if (!isDragging) return;
        var dx = e.clientX - lastX;
        var dy = e.clientY - lastY;
        lastX = e.clientX;
        lastY = e.clientY;
        container._gzPanX += dx;
        container._gzPanY += dy;
        applyGraphTransformFor(container);
    });

    window.addEventListener('mouseup', function() {
        if (isDragging) {
            isDragging = false;
            container.style.cursor = 'grab';
        }
    });

    container.addEventListener('wheel', function(e) {
        e.preventDefault();
        var factor = e.deltaY < 0 ? 1.1 : 0.9;
        // Zoom centered on mouse pointer position
        var rect = container.getBoundingClientRect();
        var mouseX = e.clientX - rect.left;
        var mouseY = e.clientY - rect.top;
        zoomGraphContainerAt(container, factor, mouseX, mouseY);
    }, { passive: false });

    container.style.cursor = 'grab';
}

function zoomGraphContainer(container, factor) {
    if (!container) return;
    container._gzZoom = (container._gzZoom || 1) * factor;
    container._gzZoom = Math.max(0.1, Math.min(8, container._gzZoom));
    applyGraphTransformFor(container);
}

function zoomGraphContainerAt(container, factor, mouseX, mouseY) {
    if (!container) return;
    var oldZoom = container._gzZoom || 1;
    var newZoom = oldZoom * factor;
    newZoom = Math.max(0.1, Math.min(8, newZoom));
    var px = container._gzPanX || 0;
    var py = container._gzPanY || 0;
    // Adjust pan so the point under the mouse stays fixed:
    // mouseX = panX + svgX * zoom  =>  svgX = (mouseX - panX) / zoom
    // After zoom: mouseX = newPanX + svgX * newZoom
    // => newPanX = mouseX - svgX * newZoom = mouseX - (mouseX - panX) * newZoom / oldZoom
    container._gzPanX = mouseX - (mouseX - px) * (newZoom / oldZoom);
    container._gzPanY = mouseY - (mouseY - py) * (newZoom / oldZoom);
    container._gzZoom = newZoom;
    applyGraphTransformFor(container);
}

function applyGraphTransformFor(container) {
    if (!container) return;
    var svg = container.querySelector('svg');
    if (!svg) return;
    var px = container._gzPanX || 0;
    var py = container._gzPanY || 0;
    var zoom = container._gzZoom || 1;
    svg.style.transform = 'translate(' + px + 'px, ' + py + 'px) scale(' + zoom + ')';
    svg.style.transformOrigin = 'top left';
}

function fitGraphToViewFor(container) {
    if (!container) return;
    var svg = container.querySelector('svg');
    if (!svg) return;

    // Reset transform to measure natural size
    svg.style.transform = '';
    var svgRect = svg.getBoundingClientRect();
    var containerRect = container.getBoundingClientRect();

    if (svgRect.width === 0 || svgRect.height === 0) return;

    var scaleX = containerRect.width / svgRect.width;
    var scaleY = containerRect.height / svgRect.height;
    container._gzZoom = Math.min(scaleX, scaleY) * 0.95;
    container._gzPanX = 0;
    container._gzPanY = 0;
    applyGraphTransformFor(container);
}

// Legacy aliases for code that uses the global state pattern
function zoomGraph(factor) {
    var container = document.getElementById('ndl-graph-container');
    zoomGraphContainer(container, factor);
}

function applyGraphTransform() {
    var container = document.getElementById('ndl-graph-container');
    applyGraphTransformFor(container);
}

function fitGraphToView() {
    var container = document.getElementById('ndl-graph-container');
    fitGraphToViewFor(container);
}

// ── Graph Utilities ────────────────────────────────────────────
// Inject ndl-node-{id} classes on viz.js-rendered SVG nodes.
// Graphviz SVG uses <g class="node"><title>node_id</title>...</g>
function injectSvgNodeClasses(svgEl) {
    var nodeGroups = svgEl.querySelectorAll('g.node');
    for (var i = 0; i < nodeGroups.length; i++) {
        var title = nodeGroups[i].querySelector('title');
        if (title) {
            var nodeId = title.textContent.trim();
            if (nodeId) nodeGroups[i].classList.add('ndl-node-' + nodeId);
        }
    }
}

// Render DOT source into a container using server-side Graphviz or viz.js fallback
function renderDotIntoContainer(container, dotSource, mode) {
    if (!dotSource) return false;

    // Generation counter prevents stale async callbacks from clobbering newer renders
    var gen = (container._renderGeneration || 0) + 1;
    container._renderGeneration = gen;

    // Show loading indicator
    container.innerHTML = '<div class="ndl-graph-fallback">Rendering graph...</div>';

    // Prefer server-side rendering (always available with Graphviz installed)
    apiPost('api/v1/render-dot', {dot: dotSource})
        .then(function(data) {
            if (container._renderGeneration !== gen) return; // stale render
            if (data.error) throw new Error(data.error);
            container.innerHTML = data.svg;
            var svg = container.querySelector('svg');
            if (svg) {
                injectSvgNodeClasses(svg);
                setupNodeClickHandlers(container, mode || 'monitor');

                // Mark render complete so renderMonitor knows SVG is ready
                if (container._pendingRenderForRun) {
                    container._renderedForRun = container._pendingRenderForRun;
                    container._pendingRenderForRun = null;
                }

                // Apply status coloring now that SVG is in the DOM
                if (mode === 'monitor') {
                    var run = NeedleState.runs[NeedleState.activeRunId];
                    if (run) updateGraphStatus(run.node_statuses || {});
                }

                // Auto-fit graph to container on initial render
                if (mode === 'create') {
                    fitGraphToViewFor(container);
                }
            }
        })
        .catch(function(e) {
            if (container._renderGeneration !== gen) return; // stale render
            // Fallback to viz.js if server-side fails
            if (vizInstance) {
                try {
                    var svg = vizInstance.renderSVGElement(dotSource);
                    container.innerHTML = '';
                    container.appendChild(svg);
                    injectSvgNodeClasses(svg);
                    setupNodeClickHandlers(container, mode || 'monitor');
                    if (mode === 'create') fitGraphToViewFor(container);
                    return;
                } catch (e2) { /* fall through to error */ }
            }
            container.innerHTML = '<div class="ndl-graph-fallback">Graph render error: ' +
                esc(e.message || String(e)) + '</div>';
        });

    return true;  // async, returns immediately
}

// Add click handlers to SVG node groups
function setupNodeClickHandlers(container, mode) {
    var nodeGroups = container.querySelectorAll('g.node');
    for (var i = 0; i < nodeGroups.length; i++) {
        nodeGroups[i].addEventListener('click', (function(g) {
            return function(e) {
                e.stopPropagation();
                var title = g.querySelector('title');
                if (!title) return;
                var nodeId = title.textContent.trim();

                // Highlight this node, un-highlight others
                var all = g.closest('svg').querySelectorAll('g.node');
                for (var j = 0; j < all.length; j++) all[j].classList.remove('ndl-node-selected');
                g.classList.add('ndl-node-selected');

                if (mode === 'create') {
                    highlightEditorNode(nodeId);
                } else if (mode === 'monitor') {
                    expandStageForNode(nodeId);
                }
            };
        })(nodeGroups[i]));
    }
}

// Create view: click node → highlight in DOT editor
function highlightEditorNode(nodeId) {
    // Switch to editor tab
    var editorTab = document.querySelector('.ndl-create-tab[data-create-tab="editor"]');
    if (editorTab) editorTab.click();

    if (!cmEditor) return;
    var lines = cmEditor.getValue().split('\n');
    var escaped = nodeId.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    var patterns = [
        new RegExp('(?:^|\\s)' + escaped + '\\s*[\\[{]'),
        new RegExp('(?:^|\\s)"' + escaped + '"\\s*[\\[{]')
    ];
    for (var i = 0; i < lines.length; i++) {
        for (var p = 0; p < patterns.length; p++) {
            if (patterns[p].test(lines[i])) {
                cmEditor.setCursor(i, 0);
                cmEditor.setSelection({line: i, ch: 0}, {line: i, ch: lines[i].length});
                cmEditor.scrollIntoView({line: i, ch: 0}, 100);
                cmEditor.focus();
                return;
            }
        }
    }
}

// Monitor view: click node → expand stage details
function expandStageForNode(nodeId) {
    var stageList = document.getElementById('ndl-stage-list');
    if (!stageList) return;
    var stages = stageList.querySelectorAll('.ndl-stage');
    for (var i = 0; i < stages.length; i++) {
        var nameEl = stages[i].querySelector('.ndl-stage-name');
        if (nameEl && nameEl.textContent.trim() === nodeId) {
            stages[i].scrollIntoView({behavior: 'smooth', block: 'nearest'});
            if (!stages[i].classList.contains('open')) {
                var header = stages[i].querySelector('.ndl-stage-header');
                if (header) header.click();
            }
            return;
        }
    }
}

// ── Create View ────────────────────────────────────────────────
var chatMessages = [];  // conversation history for generate-dot

function renderCreate() {
    initCodeMirror();
    initVizJs();
    setupCreateTabs();
    setupChat();

    var uploadBtn = document.getElementById('ndl-upload-btn');
    var runBtn = document.getElementById('ndl-run-dot-btn');
    var resumeBtn = document.getElementById('ndl-resume-btn');

    if (uploadBtn) {
        uploadBtn.onclick = function() { showDotFilePicker(); };
    }

    if (runBtn) {
        runBtn.onclick = function() { handleRunDot(); };
    }

    if (resumeBtn) {
        resumeBtn.onclick = function() { handleResumeDot(); };
    }

    // Disable resume button by default
    setResumeButtonEnabled(false);

    // Set up zoom/pan toolbar for create preview graph
    setupGraphToolbarFor('ndl-create-graph-toolbar', 'ndl-create-preview');

    loadTemplateList();
}

var templateCache = {};

function loadTemplateList() {
    var select = document.getElementById('ndl-template-select');
    if (!select) return;

    fetch('api/v1/templates')
        .then(function(r) { return r.json(); })
        .then(function(templates) {
            select.innerHTML = '<option value="">Load Template</option>';
            templates.forEach(function(t) {
                templateCache[t.name] = t;
                var opt = document.createElement('option');
                opt.value = t.name;
                opt.textContent = t.label || t.name;
                select.appendChild(opt);
            });

            select.onchange = function() {
                if (!select.value) return;
                loadTemplate(select.value);
                select.value = '';
            };
        })
        .catch(function() {
            select.style.display = 'none';
        });
}

function loadTemplate(name) {
    var tmpl = templateCache[name];

    fetch('api/v1/templates/' + encodeURIComponent(name))
        .then(function(r) {
            if (!r.ok) throw new Error('Template not found');
            return r.text();
        })
        .then(function(dot) {
            var params = (tmpl && tmpl.params) ? tmpl.params : [];

            // Fall back to auto-detecting $var.* if no params declared
            if (params.length === 0) {
                var varRefs = {};
                var re = /\$var\.([a-zA-Z_][a-zA-Z0-9_.]*)/g;
                var match;
                while ((match = re.exec(dot)) !== null) {
                    varRefs[match[1]] = true;
                }
                Object.keys(varRefs).forEach(function(v) {
                    params.push({name: v, type: 'text', 'default': ''});
                });
            }

            if (params.length === 0) {
                setEditorContent(dot);
                loadedDotFullPath = '';
                editorBaseline = dot;
                editorOrigin = 'generated';
                return;
            }

            showTemplateParamForm(name, dot, params);
        })
        .catch(function(err) {
            console.error('Failed to load template:', err);
        });
}

function showTemplateParamForm(name, dot, params) {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal';

    var title = document.createElement('h3');
    title.textContent = 'Configure Pipeline';
    title.style.marginTop = '0';
    modal.appendChild(title);

    var desc = document.createElement('p');
    desc.textContent = 'Set parameters for this pipeline:';
    desc.style.color = 'var(--text-muted)';
    desc.style.fontSize = '13px';
    modal.appendChild(desc);

    var inputs = {};
    params.forEach(function(p) {
        var label = document.createElement('label');
        label.style.display = 'block';
        label.style.marginBottom = '12px';

        var span = document.createElement('span');
        span.textContent = p.name + (p['default'] === 'required' ? ' *' : '');
        span.style.display = 'block';
        span.style.fontSize = '12px';
        span.style.fontWeight = 'bold';
        span.style.marginBottom = '4px';
        label.appendChild(span);

        var input;
        if (p.type === 'choice' && p.options) {
            input = document.createElement('select');
            input.className = 'ndl-select';
            input.style.width = '100%';
            p.options.forEach(function(opt) {
                var o = document.createElement('option');
                o.value = opt;
                o.textContent = opt;
                if (opt === p['default']) o.selected = true;
                input.appendChild(o);
            });
        } else if (p.name === 'seed' || p.type === 'textarea') {
            input = document.createElement('textarea');
            input.rows = 3;
            input.placeholder = 'Describe your project idea...';
            input.className = 'ndl-param-input';
            input.style.width = '100%';
        } else {
            input = document.createElement('input');
            input.type = 'text';
            input.placeholder = p['default'] === 'optional' ? '(optional)' : p.name;
            input.className = 'ndl-param-input';
            if (p['default'] && p['default'] !== 'required' && p['default'] !== 'optional') {
                input.value = p['default'];
            }
            // Add browse button for directory params
            if (p.name.match(/dir$|directory$|path$/i)) {
                input.style.flex = '1';
                var row = document.createElement('div');
                row.style.display = 'flex';
                row.style.gap = '6px';
                row.appendChild(input);
                var browseBtn = document.createElement('button');
                browseBtn.type = 'button';
                browseBtn.textContent = 'Browse';
                browseBtn.className = 'ndl-select';
                browseBtn.style.whiteSpace = 'nowrap';
                (function(inp) {
                    browseBtn.onclick = function() {
                        showDirectoryPicker(function(dir) { inp.value = dir; }, inp.value || '.');
                    };
                })(input);
                row.appendChild(browseBtn);
                label.appendChild(row);
            } else {
                input.style.width = '100%';
                label.appendChild(input);
            }
        }
        modal.appendChild(label);
        inputs[p.name] = input;
    });

    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';
    actions.style.marginTop = '16px';

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-select';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    var loadBtn = document.createElement('button');
    loadBtn.textContent = 'Load Template';
    loadBtn.className = 'ndl-select';
    loadBtn.style.background = 'var(--accent)';
    loadBtn.style.color = '#fff';
    loadBtn.style.borderColor = 'var(--accent)';
    loadBtn.onclick = function() {
        // Check required fields
        for (var i = 0; i < params.length; i++) {
            if (params[i]['default'] === 'required') {
                var val = inputs[params[i].name].value;
                if (!val || !val.trim()) {
                    inputs[params[i].name].style.borderColor = 'red';
                    inputs[params[i].name].focus();
                    return;
                }
            }
        }

        var result = dot;
        params.forEach(function(p) {
            var value = inputs[p.name].value || '';
            result = result.replace(new RegExp('\\$var\\.' + p.name.replace(/\./g, '\\.'), 'g'), value);
            // If this template has a project_dir param, use it as the default run directory
            if (p.name === 'project_dir' && value) {
                loadedDotDir = value;
            }
        });
        setEditorContent(result);
        // Templates produce DOTs that have no on-disk home until the
        // server stashes them under .needle/<stem>/source.dot at run time.
        loadedDotFullPath = '';
        editorBaseline = result;
        editorOrigin = 'generated';
        document.body.removeChild(overlay);
    };
    actions.appendChild(loadBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);

    // Focus first text input
    for (var i = 0; i < params.length; i++) {
        if (params[i].type !== 'choice') {
            var first = inputs[params[i].name];
            if (first) setTimeout(function() { first.focus(); }, 50);
            break;
        }
    }
}

function setEditorContent(dot) {
    if (cmEditor) {
        cmEditor.setValue(dot);
    } else {
        var textarea = document.getElementById('ndl-editor-textarea');
        if (textarea) textarea.value = dot;
    }
    schedulePreviewUpdate();
}

function setupCreateTabs() {
    var tabs = document.querySelectorAll('.ndl-create-tab');
    for (var i = 0; i < tabs.length; i++) {
        tabs[i].onclick = (function(tab) {
            return function() {
                var target = tab.getAttribute('data-create-tab');
                // Toggle tab active state
                var allTabs = document.querySelectorAll('.ndl-create-tab');
                for (var j = 0; j < allTabs.length; j++) {
                    allTabs[j].classList.toggle('active', allTabs[j] === tab);
                }
                // Toggle panel
                var chatPanel = document.getElementById('ndl-create-chat-panel');
                var editorPanel = document.getElementById('ndl-create-editor-panel');
                if (chatPanel) chatPanel.classList.toggle('active', target === 'chat');
                if (editorPanel) editorPanel.classList.toggle('active', target === 'editor');

                // If switching to editor, refresh CodeMirror
                if (target === 'editor' && cmEditor) {
                    setTimeout(function() { cmEditor.refresh(); }, 10);
                }
            };
        })(tabs[i]);
    }
}

function setupChat() {
    var sendBtn = document.getElementById('ndl-chat-send');
    var input = document.getElementById('ndl-chat-input');

    if (sendBtn) {
        sendBtn.onclick = function() { sendChatMessage(); };
    }
    if (input) {
        input.onkeypress = function(e) {
            if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                sendChatMessage();
            }
        };
    }
}

function sendChatMessage() {
    var input = document.getElementById('ndl-chat-input');
    if (!input) return;

    var text = input.value.trim();
    if (!text) return;

    input.value = '';

    // Add user message
    chatMessages.push({role: 'user', content: text});
    appendChatBubble('user', text);

    // Show thinking indicator
    var messagesEl = document.getElementById('ndl-chat-messages');
    var thinkingEl = document.createElement('div');
    thinkingEl.className = 'ndl-chat-thinking';
    thinkingEl.id = 'ndl-chat-thinking';
    thinkingEl.textContent = 'Generating pipeline...';
    if (messagesEl) messagesEl.appendChild(thinkingEl);
    scrollChatToBottom();

    // Disable input while waiting
    input.disabled = true;
    var sendBtn = document.getElementById('ndl-chat-send');
    if (sendBtn) sendBtn.disabled = true;

    var provider = document.getElementById('ndl-chat-provider');
    var providerName = provider ? provider.value : 'anthropic';

    apiPost('api/v1/generate-dot', {
        provider: providerName,
        messages: chatMessages
    }).then(function(data) {
        // Remove thinking indicator
        var thinking = document.getElementById('ndl-chat-thinking');
        if (thinking) thinking.remove();

        if (data.error) {
            appendChatBubble('assistant', 'Error: ' + data.error);
            showToast(data.error, 'error');
        } else if (data.response) {
            chatMessages.push({role: 'assistant', content: data.response});
            appendChatBubble('assistant', data.response, data.dot_source);

            // If DOT was extracted, apply it to the editor
            if (data.dot_source) {
                setEditorValue(data.dot_source);
                schedulePreviewUpdate();
                // Generated content has no on-disk home yet; the server
                // will stash it under .needle/<stem>/source.dot when Run.
                loadedDotFullPath = '';
                editorBaseline = data.dot_source;
                editorOrigin = 'generated';
                showToast('Pipeline generated — check the preview', 'success');
            }
        }
    }).catch(function(err) {
        var thinking = document.getElementById('ndl-chat-thinking');
        if (thinking) thinking.remove();
        appendChatBubble('assistant', 'Request failed: ' + err.message);
        showToast('Failed to generate pipeline', 'error');
    }).then(function() {
        // Re-enable input
        input.disabled = false;
        if (sendBtn) sendBtn.disabled = false;
        input.focus();
    });
}

function appendChatBubble(role, text, dotSource) {
    var messagesEl = document.getElementById('ndl-chat-messages');
    if (!messagesEl) return;

    // Remove welcome message if present
    var welcome = messagesEl.querySelector('.ndl-chat-welcome');
    if (welcome) welcome.remove();

    var msg = document.createElement('div');
    msg.className = 'ndl-chat-msg ' + role;

    var roleLabel = document.createElement('div');
    roleLabel.className = 'ndl-chat-msg-role';
    roleLabel.textContent = role === 'user' ? 'You' : 'Assistant';
    msg.appendChild(roleLabel);

    var body = document.createElement('div');
    body.className = 'ndl-chat-msg-body';

    if (role === 'assistant') {
        // Render with basic markdown-like formatting for code blocks
        body.innerHTML = formatAssistantMessage(text);
    } else {
        body.textContent = text;
    }
    msg.appendChild(body);

    // Add "Apply to Editor" button if DOT was extracted
    if (dotSource && role === 'assistant') {
        var applyBtn = document.createElement('button');
        applyBtn.className = 'ndl-chat-apply-btn';
        applyBtn.textContent = 'Apply to Editor';
        applyBtn.onclick = function() {
            setEditorValue(dotSource);
            schedulePreviewUpdate();
            loadedDotFullPath = '';
            editorBaseline = dotSource;
            editorOrigin = 'generated';
            // Switch to editor tab
            var editorTab = document.querySelector('.ndl-create-tab[data-create-tab="editor"]');
            if (editorTab) editorTab.click();
            showToast('DOT applied to editor', 'success');
        };
        msg.appendChild(applyBtn);
    }

    messagesEl.appendChild(msg);
    scrollChatToBottom();
}

function formatAssistantMessage(text) {
    // Replace ```dot...``` or ```graphviz...``` blocks with styled blocks
    var result = esc(text);

    // Replace code blocks (escaped version)
    result = result.replace(/```(?:dot|graphviz)?\n([\s\S]*?)```/g, function(match, code) {
        return '<div class="ndl-chat-dot-block">' + code + '</div>';
    });

    // Replace inline code
    result = result.replace(/`([^`]+)`/g, '<code>$1</code>');

    // Replace newlines with <br>
    result = result.replace(/\n/g, '<br>');

    return result;
}

function scrollChatToBottom() {
    var el = document.getElementById('ndl-chat-messages');
    if (el) el.scrollTop = el.scrollHeight;
}

function initCodeMirror() {
    if (cmEditor) return;
    if (typeof CodeMirror === 'undefined') {
        // Lazy-load CodeMirror
        loadCSS('https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.css');
        loadScript('https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.js', function() {
            loadScript('https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/clike/clike.min.js', function() {
                createCMEditor();
            });
        });
        return;
    }
    createCMEditor();
}

function createCMEditor() {
    if (cmEditor || typeof CodeMirror === 'undefined') return;

    var textarea = document.getElementById('ndl-editor-textarea');
    if (!textarea) return;

    var val = textarea.value;
    var container = document.getElementById('ndl-editor-container');

    cmEditor = CodeMirror(container, {
        value: val,
        mode: 'text/x-csrc',
        lineNumbers: true,
        theme: NeedleState.theme === 'dark' ? 'default' : 'default',
        tabSize: 4,
        indentUnit: 4
    });

    // Remove the textarea
    if (textarea.parentNode === container) {
        container.removeChild(textarea);
    }

    cmEditor.on('change', function() {
        schedulePreviewUpdate();
    });
}

function getEditorValue() {
    if (cmEditor) return cmEditor.getValue();
    var ta = document.getElementById('ndl-editor-textarea');
    return ta ? ta.value : '';
}

function setEditorValue(val) {
    if (cmEditor) { cmEditor.setValue(val); return; }
    var ta = document.getElementById('ndl-editor-textarea');
    if (ta) ta.value = val;
}

function initVizJs() {
    if (vizInstance) return;
    if (typeof Viz !== 'undefined') {
        Viz.instance().then(function(v) { vizInstance = v; onVizReady(); });
        return;
    }
    loadScript('https://unpkg.com/@viz-js/viz@3.4.0/lib/viz-standalone.js', function() {
        if (typeof Viz !== 'undefined') {
            Viz.instance().then(function(v) { vizInstance = v; onVizReady(); });
        } else {
            showVizFallback();
        }
    });
}

function onVizReady() {
    // viz.js is a fallback for server-side rendering
    // No action needed — server-side /api/v1/render-dot is primary
}

function showVizFallback() {
    var previewEl = document.getElementById('ndl-create-preview');
    if (previewEl && !vizInstance) {
        previewEl.innerHTML = '<div class="ndl-graph-fallback">Graph preview unavailable — viz.js could not be loaded</div>';
    }
}

function schedulePreviewUpdate() {
    clearTimeout(previewDebounceTimer);
    previewDebounceTimer = setTimeout(updateCreatePreview, 500);
}

function updateCreatePreview() {
    var dot = getEditorValue();
    var previewEl = document.getElementById('ndl-create-preview');
    if (!previewEl) return;

    if (!dot.trim()) {
        previewEl.innerHTML = '<div class="ndl-graph-fallback">Edit DOT source to see preview</div>';
        return;
    }

    renderDotIntoContainer(previewEl, dot, 'create');
}

function setResumeButtonEnabled(enabled) {
    var btn = document.getElementById('ndl-resume-btn');
    if (!btn) return;
    btn.disabled = !enabled;
    if (enabled) {
        btn.className = 'ndl-btn-primary';
        btn.style.opacity = '';
    } else {
        btn.className = 'ndl-btn';
        btn.style.opacity = '0.5';
    }
}

function showDotFilePicker(onLoadCallback) {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal';

    var title = document.createElement('h3');
    title.textContent = 'Load DOT File';
    title.style.marginTop = '0';
    modal.appendChild(title);

    var pathRow = document.createElement('div');
    pathRow.style.display = 'flex';
    pathRow.style.gap = '8px';
    pathRow.style.marginBottom = '12px';

    var pathInput = document.createElement('input');
    pathInput.type = 'text';
    pathInput.value = loadedDotDir || '.';
    pathInput.className = 'ndl-param-input';
    pathInput.style.flex = '1';
    pathRow.appendChild(pathInput);
    modal.appendChild(pathRow);

    var listing = document.createElement('div');
    listing.style.maxHeight = '300px';
    listing.style.overflowY = 'auto';
    listing.style.border = '1px solid var(--border)';
    listing.style.borderRadius = 'var(--radius)';
    listing.style.marginBottom = '12px';
    listing.style.background = 'var(--surface)';
    modal.appendChild(listing);

    function loadDir(path) {
        fetch('api/v1/browse?path=' + encodeURIComponent(path) + '&files=.dot,.gv')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                pathInput.value = data.path || path;
                var sep = data.sep || '/';
                listing.innerHTML = '';

                // Directories
                if (data.dirs) {
                    data.dirs.forEach(function(d) {
                        var item = document.createElement('div');
                        item.style.padding = '6px 12px';
                        item.style.cursor = 'pointer';
                        item.style.fontSize = '13px';
                        item.style.borderBottom = '1px solid var(--border)';
                        item.textContent = d === '..' ? '\u2190 ..' : '\uD83D\uDCC1 ' + d;
                        item.onmouseover = function() { this.style.background = 'var(--surface-raised)'; };
                        item.onmouseout = function() { this.style.background = ''; };
                        item.onclick = function() { loadDir(data.path + sep + d); };
                        listing.appendChild(item);
                    });
                }

                // DOT files
                if (data.files) {
                    data.files.forEach(function(f) {
                        var item = document.createElement('div');
                        item.style.padding = '6px 12px';
                        item.style.cursor = 'pointer';
                        item.style.fontSize = '13px';
                        item.style.borderBottom = '1px solid var(--border)';
                        item.style.color = 'var(--accent)';
                        item.textContent = '\u25C6 ' + f;
                        item.onmouseover = function() { this.style.background = 'var(--surface-raised)'; };
                        item.onmouseout = function() { this.style.background = ''; };
                        item.onclick = function() {
                            var fullPath = data.path + sep + f;
                            if (onLoadCallback) {
                                // Monitor mode: fetch content and call back
                                fetch('api/v1/read-file?path=' + encodeURIComponent(fullPath))
                                    .then(function(r) { return r.ok ? r.text() : Promise.reject('read failed'); })
                                    .then(function(content) {
                                        onLoadCallback(content, fullPath, data.path);
                                    });
                            } else {
                                // Create mode: load into editor
                                loadDotFromServer(fullPath, data.path);
                            }
                            document.body.removeChild(overlay);
                        };
                        listing.appendChild(item);
                    });
                }

                if ((!data.dirs || data.dirs.length === 0) && (!data.files || data.files.length === 0)) {
                    listing.innerHTML = '<div style="padding:12px;color:var(--text-muted)">No directories or .dot files here</div>';
                }
            })
            .catch(function() {
                listing.innerHTML = '<div style="padding:12px;color:var(--text-muted)">Failed to browse</div>';
            });
    }

    pathInput.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') loadDir(pathInput.value);
    });

    loadDir(loadedDotDir || '.');

    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-select';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);
    pathInput.focus();
}

function loadDotFromServer(filePath, dirPath) {
    fetch('api/v1/read-file?path=' + encodeURIComponent(filePath))
        .then(function(r) {
            if (!r.ok) throw new Error('Failed to read file');
            return r.text();
        })
        .then(function(content) {
            setEditorValue(content);
            schedulePreviewUpdate();
            loadedDotDir = dirPath;
            loadedDotFullPath = filePath;
            editorBaseline = content;
            editorOrigin = 'file';

            // Switch to editor tab
            var editorTab = document.querySelector('.ndl-create-tab[data-create-tab="editor"]');
            if (editorTab) editorTab.click();

            var fileName = filePath.split('/').pop();
            showToast('Loaded ' + fileName, 'success');

            // Check for checkpoint in this directory
            apiPost('api/v1/check-checkpoint', {project_dir: dirPath})
                .then(function(data) {
                    setResumeButtonEnabled(data && data.exists);
                })
                .catch(function() {
                    setResumeButtonEnabled(false);
                });
        })
        .catch(function(err) {
            showToast('Failed to load: ' + err.message, 'error');
        });
}

// Classify what's in the editor relative to its on-disk home so Run
// can route correctly:
//   'file_clean'      — backed by a file on disk and unedited
//   'file_dirty'      — backed by a file on disk but has unsaved changes
//   'generated_clean' — chat/template output, never edited; safe to stash under .needle/
//   'typed'           — typed/pasted by the user (or edited generated content); needs an explicit save path
function computeRunOrigin(dot) {
    if (loadedDotFullPath) {
        return (dot === editorBaseline) ? 'file_clean' : 'file_dirty';
    }
    if (editorOrigin === 'generated' && dot === editorBaseline) {
        return 'generated_clean';
    }
    return 'typed';
}

function adoptRunResponse(dot, data) {
    if (!data || !data.id) {
        if (data && data.error) showToast(data.error, 'error');
        return;
    }
    if (!NeedleState.runs[data.id]) {
        NeedleState.runs[data.id] = {
            id: data.id, status: 'running', events: [],
            node_statuses: {}, current_node: '', completed_stages: 0,
            total_stages: 0, elapsed_seconds: 0, pending_question: '',
            node_errors: {}, warnings: []
        };
    }
    NeedleState.runs[data.id].dot_source = dot;
    if (data.dot_path) NeedleState.runs[data.id].dot_path = data.dot_path;
    openRunTab(data.id);
    navigate('monitor', data.id);
    showToast('Run ' + data.id + ' started', 'success');
}

// Adopt a server-side dot_path (e.g. .needle/<stem>/source.dot returned
// from a generated-DOT run) as the editor's on-disk home so the next
// Run/Resume goes through dot_path without another round of stashing.
function adoptCanonicalPath(dotPath, dot) {
    if (!dotPath) return;
    loadedDotFullPath = dotPath;
    var slash = dotPath.lastIndexOf('/');
    if (slash > 0) loadedDotDir = dotPath.substring(0, slash);
    editorBaseline = dot;
    editorOrigin = 'file';
}

function handleRunDot() {
    var dot = getEditorValue();
    if (!dot.trim()) {
        showToast('No DOT source to run', 'error');
        return;
    }
    var initialDir = loadedDotDir || '.';
    showDirectoryPicker(function(projectDir) {
        var origin = computeRunOrigin(dot);

        var runWithPath = function(path) {
            apiStartRun({dot_path: path, project_dir: projectDir}).then(function(data) {
                if (data && data.dot_path) adoptCanonicalPath(data.dot_path, dot);
                adoptRunResponse(dot, data);
            }).catch(function(err) {
                showToast('Failed to start run: ' + err.message, 'error');
            });
        };

        var runWithSource = function() {
            apiStartRun({dot_source: dot, project_dir: projectDir}).then(function(data) {
                if (data && data.dot_path) adoptCanonicalPath(data.dot_path, dot);
                adoptRunResponse(dot, data);
            }).catch(function(err) {
                showToast('Failed to start run: ' + err.message, 'error');
            });
        };

        if (origin === 'file_clean') {
            runWithPath(loadedDotFullPath);
        } else if (origin === 'file_dirty') {
            confirmModal(
                'Save and run',
                'Save edits to ' + loadedDotFullPath + ' and run?',
                function() {
                    apiPost('api/v1/write-file',
                            {path: loadedDotFullPath, content: dot})
                        .then(function(d) {
                            if (d && d.error) {
                                showToast('Save failed: ' + d.error, 'error');
                                return;
                            }
                            editorBaseline = dot;
                            runWithPath(loadedDotFullPath);
                        }).catch(function(err) {
                            showToast('Save failed: ' + err.message, 'error');
                        });
                });
        } else if (origin === 'generated_clean') {
            // Server stashes one canonical copy under
            // <project_dir>/.needle/<stem>/source.dot — no project-root
            // duplicate. Its dot_path becomes our editor home.
            runWithSource();
        } else { // 'typed'
            promptSavePath(projectDir, suggestedDotFilename(dot), function(savePath) {
                apiPost('api/v1/write-file', {path: savePath, content: dot})
                    .then(function(d) {
                        if (d && d.error) {
                            showToast('Save failed: ' + d.error, 'error');
                            return;
                        }
                        adoptCanonicalPath(savePath, dot);
                        runWithPath(savePath);
                    }).catch(function(err) {
                        showToast('Save failed: ' + err.message, 'error');
                    });
            });
        }
    }, initialDir);
}

function handleResumeDot() {
    if (!loadedDotFullPath && !loadedDotDir) {
        showToast('Load a DOT file first to resume', 'error');
        return;
    }
    var projectDir = loadedDotDir || loadedDotFullPath;
    var body = {project_dir: projectDir};
    if (loadedDotFullPath) {
        body.dot_path = loadedDotFullPath;
    } else {
        // No on-disk home — fall back to the editor contents so the
        // server can locate the run by stem.
        var dot = getEditorValue();
        if (dot) body.dot_source = dot;
    }
    apiPost('api/v1/resume', body)
        .then(function(data) {
            if (data.error) {
                showToast('Resume failed: ' + data.error, 'error');
                return;
            }
            if (data.id) {
                if (!NeedleState.runs[data.id]) {
                    NeedleState.runs[data.id] = {
                        id: data.id, status: 'running', events: [],
                        node_statuses: {}, current_node: '', completed_stages: 0,
                        total_stages: 0, elapsed_seconds: 0, pending_question: '',
                        node_errors: {}, warnings: []
                    };
                }
                NeedleState.runs[data.id].dot_source = data.dot_source || '';
                openRunTab(data.id);
                navigate('monitor', data.id);
                showToast('Resumed from ' + (data.resumed_from || 'checkpoint'), 'success');
            }
        })
        .catch(function(err) {
            showToast('Resume failed: ' + err.message, 'error');
        });
}

// Suggest a filename based on the graph's label="..." attribute.
function suggestedDotFilename(dot) {
    var m = /label\s*=\s*"([^"]+)"/.exec(dot || '');
    var label = m ? m[1] : 'pipeline';
    var slug = label.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
    return (slug || 'pipeline') + '.dot';
}

function promptSavePath(projectDir, suggestedName, callback) {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal';

    var title = document.createElement('h3');
    title.textContent = 'Save DOT before running';
    title.style.marginTop = '0';
    modal.appendChild(title);

    var desc = document.createElement('p');
    desc.textContent = 'Pick a path to save the editor contents to. The pipeline runs from the saved file — there is no in-memory copy.';
    desc.style.color = 'var(--text-muted)';
    desc.style.fontSize = '13px';
    modal.appendChild(desc);

    var input = document.createElement('input');
    input.type = 'text';
    input.className = 'ndl-param-input';
    input.style.width = '100%';
    input.value = (projectDir && projectDir !== '.') ?
        (projectDir.replace(/\/$/, '') + '/' + suggestedName) :
        suggestedName;
    modal.appendChild(input);

    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';
    actions.style.marginTop = '16px';

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-select';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    var saveBtn = document.createElement('button');
    saveBtn.textContent = 'Save and run';
    saveBtn.className = 'ndl-select';
    saveBtn.style.background = 'var(--accent)';
    saveBtn.style.color = '#fff';
    saveBtn.style.borderColor = 'var(--accent)';
    saveBtn.onclick = function() {
        var path = (input.value || '').trim();
        if (!path) { input.focus(); return; }
        document.body.removeChild(overlay);
        callback(path);
    };
    actions.appendChild(saveBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);
    setTimeout(function() { input.focus(); input.select(); }, 50);
}

function confirmModal(title, message, onConfirm) {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal';

    var h = document.createElement('h3');
    h.textContent = title;
    h.style.marginTop = '0';
    modal.appendChild(h);

    var p = document.createElement('p');
    p.textContent = message;
    modal.appendChild(p);

    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';
    actions.style.marginTop = '16px';

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-select';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    var okBtn = document.createElement('button');
    okBtn.textContent = 'Save and run';
    okBtn.className = 'ndl-select';
    okBtn.style.background = 'var(--accent)';
    okBtn.style.color = '#fff';
    okBtn.style.borderColor = 'var(--accent)';
    okBtn.onclick = function() {
        document.body.removeChild(overlay);
        onConfirm();
    };
    actions.appendChild(okBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);
}

function showDirectoryPicker(callback, initialPath) {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal';

    var title = document.createElement('h3');
    title.textContent = 'Project Directory';
    title.style.marginTop = '0';
    modal.appendChild(title);

    var desc = document.createElement('p');
    desc.textContent = 'Choose where agents will create project files:';
    desc.style.color = 'var(--text-muted)';
    desc.style.fontSize = '13px';
    modal.appendChild(desc);

    // Path input
    var pathRow = document.createElement('div');
    pathRow.style.display = 'flex';
    pathRow.style.gap = '8px';
    pathRow.style.marginBottom = '12px';

    var pathInput = document.createElement('input');
    pathInput.type = 'text';
    pathInput.value = initialPath || '.';
    pathInput.className = 'ndl-param-input';
    pathInput.style.flex = '1';
    pathRow.appendChild(pathInput);
    modal.appendChild(pathRow);

    // Directory listing
    var dirList = document.createElement('div');
    dirList.className = 'ndl-dir-list';
    dirList.style.maxHeight = '250px';
    dirList.style.overflowY = 'auto';
    dirList.style.border = '1px solid var(--border)';
    dirList.style.borderRadius = 'var(--radius)';
    dirList.style.marginBottom = '12px';
    dirList.style.background = 'var(--surface)';
    modal.appendChild(dirList);

    function loadDir(path) {
        fetch('api/v1/browse?path=' + encodeURIComponent(path))
            .then(function(r) { return r.json(); })
            .then(function(data) {
                pathInput.value = data.path || path;
                dirList.innerHTML = '';
                if (data.dirs) {
                    data.dirs.forEach(function(d) {
                        var item = document.createElement('div');
                        item.style.padding = '6px 12px';
                        item.style.cursor = 'pointer';
                        item.style.fontSize = '13px';
                        item.style.borderBottom = '1px solid var(--border)';
                        item.textContent = d === '..' ? '\u2190 ..' : '\uD83D\uDCC1 ' + d;
                        item.onmouseover = function() { this.style.background = 'var(--surface-raised)'; };
                        item.onmouseout = function() { this.style.background = ''; };
                        item.onclick = function() {
                            var newPath = data.path + '/' + d;
                            loadDir(newPath);
                        };
                        dirList.appendChild(item);
                    });
                }
                if (data.error) {
                    dirList.innerHTML = '<div style="padding:12px;color:var(--text-muted)">' + data.error + '</div>';
                }
            })
            .catch(function() {
                dirList.innerHTML = '<div style="padding:12px;color:var(--text-muted)">Failed to browse</div>';
            });
    }

    pathInput.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') loadDir(pathInput.value);
    });

    loadDir(initialPath || '.');

    // Actions
    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';

    var newDirBtn = document.createElement('button');
    newDirBtn.textContent = 'New Directory';
    newDirBtn.className = 'ndl-select';
    newDirBtn.onclick = function() {
        var name = prompt('New directory name:');
        if (name) {
            var newPath = pathInput.value + '/' + name;
            pathInput.value = newPath;
        }
    };
    actions.appendChild(newDirBtn);

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-select';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    var selectBtn = document.createElement('button');
    selectBtn.textContent = 'Select';
    selectBtn.className = 'ndl-select';
    selectBtn.style.background = 'var(--accent)';
    selectBtn.style.color = '#fff';
    selectBtn.style.borderColor = 'var(--accent)';
    selectBtn.onclick = function() {
        document.body.removeChild(overlay);
        callback(pathInput.value);
    };
    actions.appendChild(selectBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);
    pathInput.focus();
    pathInput.select();
}

// ── Settings View ──────────────────────────────────────────────

// Helper: get a value from nested config object by dot-path
function cfgGet(path, fallback) {
    var parts = path.split('.');
    var obj = NeedleState.config;
    for (var i = 0; i < parts.length; i++) {
        if (obj == null || typeof obj !== 'object') return fallback !== undefined ? fallback : '';
        obj = obj[parts[i]];
    }
    return (obj !== undefined && obj !== null) ? obj : (fallback !== undefined ? fallback : '');
}

// Save a config value via PUT /api/v1/config with partial JSON merge
function saveConfigValue(path, value) {
    // Build nested object from dot path
    var parts = path.split('.');
    var obj = {};
    var cur = obj;
    for (var i = 0; i < parts.length - 1; i++) {
        cur[parts[i]] = {};
        cur = cur[parts[i]];
    }
    cur[parts[parts.length - 1]] = value;

    return apiPut('api/v1/config', obj).then(function(data) {
        if (data && !data.error) {
            NeedleState.config = data;
            // Sync legacy NeedleState.settings for backward compat
            NeedleState.settings.autoApprove = cfgGet('server.auto_approve', false);
            NeedleState.settings.chatAgent = cfgGet('defaults.chat_agent', 'claude');
            NeedleState.settings.chatModel = cfgGet('defaults.chat_model', 'claude-sonnet-4-6');
        }
        return data;
    });
}

// Fetch full config from server
function loadConfigFromServer() {
    return apiGet('api/v1/config').then(function(data) {
        if (data && !data.error) {
            NeedleState.config = data;
            NeedleState.configLoaded = true;
            // Sync legacy NeedleState.settings
            NeedleState.settings.autoApprove = cfgGet('server.auto_approve', false);
            NeedleState.settings.chatAgent = cfgGet('defaults.chat_agent', 'claude');
            NeedleState.settings.chatModel = cfgGet('defaults.chat_model', 'claude-sonnet-4-6');
            // Sync theme from config
            var configTheme = cfgGet('ui.theme', '');
            if (configTheme && configTheme !== NeedleState.theme) {
                applyTheme(configTheme);
            }
        }
        return data;
    }).catch(function() {
        // Server may not have config endpoint yet; fall back to localStorage
        loadSettingsLegacy();
    });
}

// Legacy localStorage fallback (for migration)
function loadSettingsLegacy() {
    try {
        var s = JSON.parse(localStorage.getItem('needle-settings') || '{}');
        if (s.autoApprove !== undefined) NeedleState.settings.autoApprove = s.autoApprove;
        if (s.defaultModel) NeedleState.settings.defaultModel = s.defaultModel;
        if (s.chatAgent) NeedleState.settings.chatAgent = s.chatAgent;
        if (s.chatModel) NeedleState.settings.chatModel = s.chatModel;
    } catch(e) { /* ignore */ }
}

function loadSettings() {
    loadConfigFromServer();
}

// Setup settings tab switching
function setupSettingsTabs() {
    var tabs = document.querySelectorAll('.ndl-settings-tab');
    for (var i = 0; i < tabs.length; i++) {
        tabs[i].addEventListener('click', (function(tabName) {
            return function() {
                // Deactivate all
                var allTabs = document.querySelectorAll('.ndl-settings-tab');
                var allPanes = document.querySelectorAll('.ndl-settings-pane');
                for (var j = 0; j < allTabs.length; j++) {
                    allTabs[j].classList.remove('active');
                    allTabs[j].setAttribute('aria-selected', 'false');
                }
                for (var k = 0; k < allPanes.length; k++) {
                    allPanes[k].classList.remove('active');
                }
                // Activate selected
                this.classList.add('active');
                this.setAttribute('aria-selected', 'true');
                var pane = document.getElementById('ndl-settings-' + tabName);
                if (pane) pane.classList.add('active');
            };
        })(tabs[i].getAttribute('data-settings-tab')));
    }
}

function renderSettings() {
    // Make sure we have config loaded
    if (!NeedleState.configLoaded) {
        loadConfigFromServer().then(function() {
            renderSettingsContent();
        });
    } else {
        renderSettingsContent();
    }
}

function renderSettingsContent() {
    renderSettingsApiKeys();
    renderSettingsModels();
    renderSettingsServer();
    renderSettingsAppearance();
}

// ── Tab 1: API Keys ──────────────────────────────────────────
function renderSettingsApiKeys() {
    var el = document.getElementById('ndl-settings-apikeys');
    if (!el) return;

    var providers = [
        {
            id: 'openai', label: 'OpenAI',
            hint: 'Your OpenAI API key. Used for OpenAI-backed llmkit calls and the AI assistant. Set via env var OPENAI_API_KEY or here.',
            configPath: 'providers.openai.api_key'
        },
        {
            id: 'anthropic', label: 'Anthropic',
            hint: 'Your Anthropic API key. Used for Claude-based LLM calls and the AI assistant. Set via env var ANTHROPIC_API_KEY or here.',
            configPath: 'providers.anthropic.api_key'
        },
        {
            id: 'gemini', label: 'Google Gemini',
            hint: 'Your Google Gemini API key. Used for Gemini-backed llmkit calls and the AI assistant. Set via env var GEMINI_API_KEY or here.',
            configPath: 'providers.gemini.api_key'
        },
        {
            id: 'tavily', label: 'Tavily',
            hint: 'Your Tavily API key. Used for web_search nodes. Set via env var TAVILY_API_KEY or here.',
            configPath: 'providers.tavily.api_key'
        }
    ];

    var html = '<div class="ndl-setting-group"><h3>API Keys</h3>' +
        '<div class="ndl-field-hint" style="margin-bottom:16px">API keys are stored in ~/.needle/config.json with restricted permissions (0600). Environment variables take precedence over stored keys.</div>';

    for (var i = 0; i < providers.length; i++) {
        var p = providers[i];
        var currentVal = cfgGet(p.configPath, '');
        html += '<div class="ndl-field">' +
            '<label class="ndl-field-label">' + esc(p.label) + '</label>' +
            '<div class="ndl-apikey-row">' +
            '<input type="password" class="ndl-field-input" id="ndl-apikey-' + p.id + '" ' +
            'data-provider="' + p.id + '" data-path="' + p.configPath + '" ' +
            'value="' + esc(currentVal) + '" placeholder="Enter API key..." autocomplete="off">' +
            '<button class="ndl-apikey-toggle" id="ndl-apikey-toggle-' + p.id + '" title="Show/hide key" data-visible="false">Show</button>' +
            '<button class="ndl-apikey-validate" id="ndl-apikey-validate-' + p.id + '" data-provider="' + p.id + '">Validate</button>' +
            '<span class="ndl-apikey-status" id="ndl-apikey-status-' + p.id + '"></span>' +
            '</div>' +
            '<div class="ndl-field-hint">' + esc(p.hint) + '</div>' +
            '</div>';
    }

    html += '</div>';
    el.innerHTML = html;

    // Bind events
    for (var j = 0; j < providers.length; j++) {
        (function(prov) {
            // Show/hide toggle
            var toggleBtn = document.getElementById('ndl-apikey-toggle-' + prov.id);
            var input = document.getElementById('ndl-apikey-' + prov.id);
            if (toggleBtn && input) {
                toggleBtn.addEventListener('click', function() {
                    var visible = this.getAttribute('data-visible') === 'true';
                    if (visible) {
                        input.type = 'password';
                        this.textContent = 'Show';
                        this.setAttribute('data-visible', 'false');
                    } else {
                        input.type = 'text';
                        this.textContent = 'Hide';
                        this.setAttribute('data-visible', 'true');
                    }
                });
            }

            // Save on change
            if (input) {
                input.addEventListener('change', function() {
                    var val = this.value.trim();
                    // Don't save redacted values back (they look like "sk-ab***")
                    if (val && val.indexOf('***') === -1) {
                        saveConfigValue(prov.configPath, val).then(function() {
                            showToast(prov.label + ' API key saved', 'success');
                        });
                    }
                });
            }

            // Validate button
            var validateBtn = document.getElementById('ndl-apikey-validate-' + prov.id);
            if (validateBtn) {
                validateBtn.addEventListener('click', function() {
                    var statusDot = document.getElementById('ndl-apikey-status-' + prov.id);
                    var keyInput = document.getElementById('ndl-apikey-' + prov.id);
                    if (!statusDot || !keyInput) return;

                    var keyVal = keyInput.value.trim();
                    if (!keyVal) {
                        showToast('No API key entered for ' + prov.label, 'warning');
                        return;
                    }

                    // If the key is redacted, save is not needed — just validate what server has
                    statusDot.className = 'ndl-apikey-status checking';

                    // First save the key if it is not redacted, then validate
                    var savePromise;
                    if (keyVal.indexOf('***') === -1) {
                        savePromise = saveConfigValue(prov.configPath, keyVal);
                    } else {
                        savePromise = Promise.resolve();
                    }

                    savePromise.then(function() {
                        return apiPost('api/v1/config/validate-key', { provider: prov.id });
                    }).then(function(result) {
                        if (result && result.valid) {
                            statusDot.className = 'ndl-apikey-status valid';
                            showToast(prov.label + ' API key is valid', 'success');
                        } else {
                            statusDot.className = 'ndl-apikey-status invalid';
                            showToast(prov.label + ' API key validation failed' + (result && result.error ? ': ' + result.error : ''), 'error');
                        }
                    }).catch(function() {
                        statusDot.className = 'ndl-apikey-status invalid';
                        showToast('Could not validate ' + prov.label + ' key', 'error');
                    });
                });
            }
        })(providers[j]);
    }
}

// ── Tab 2: Models & Defaults ─────────────────────────────────
function renderSettingsModels() {
    var el = document.getElementById('ndl-settings-models');
    if (!el) return;

    var chatAgentVal = cfgGet('defaults.chat_agent', 'claude');
    var chatModelVal = cfgGet('defaults.chat_model', 'claude-sonnet-4-6');
    var codingAgentVal = cfgGet('defaults.coding_agent', 'codex');
    var codingModelVal = cfgGet('defaults.coding_model', 'gpt-5.4');
    var planningAgentVal = cfgGet('defaults.planning_agent', 'claude');
    var planningModelVal = cfgGet('defaults.planning_model', 'claude-opus-4-7');
    var critiqueAgentVal = cfgGet('defaults.critique_agent', 'gemini');
    var critiqueModelVal = cfgGet('defaults.critique_model', 'gemini-2.5-pro');

    function agentDropdown(id, val) {
        return '<select class="ndl-field-select" id="' + id + '">' +
            '<option value="claude"' + (val === 'claude' ? ' selected' : '') + '>Claude (Anthropic)</option>' +
            '<option value="codex"' + (val === 'codex' ? ' selected' : '') + '>Codex (OpenAI)</option>' +
            '<option value="gemini"' + (val === 'gemini' ? ' selected' : '') + '>Gemini (Google)</option>' +
            '</select>';
    }

    function modelDropdown(id, val) {
        return '<select class="ndl-field-select" id="' + id + '">' +
            '<option value="' + esc(val) + '" selected>' + esc(val) + '</option>' +
            '</select>';
    }

    var html = '<div class="ndl-setting-group"><h3>Models &amp; Defaults</h3>' +

        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Coding Agent</label>' +
        agentDropdown('ndl-setting-coding-agent', codingAgentVal) +
        '<div class="ndl-field-hint">Agent for implementation/codergen nodes (class: default). Used in model_stylesheet <code>*</code> selector.</div>' +
        '</div>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Coding Model</label>' +
        modelDropdown('ndl-setting-coding-model', codingModelVal) +
        '<div class="ndl-field-hint">Model for coding tasks.</div>' +
        '</div>' +

        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Planning Agent</label>' +
        agentDropdown('ndl-setting-planning-agent', planningAgentVal) +
        '<div class="ndl-field-hint">Agent for orient/plan nodes (class: planning). Used in model_stylesheet <code>.planning</code> selector.</div>' +
        '</div>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Planning Model</label>' +
        modelDropdown('ndl-setting-planning-model', planningModelVal) +
        '<div class="ndl-field-hint">Model for planning tasks.</div>' +
        '</div>' +

        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Critique Agent</label>' +
        agentDropdown('ndl-setting-critique-agent', critiqueAgentVal) +
        '<div class="ndl-field-hint">Agent for adversarial review nodes (class: critique). A different provider gives better adversarial coverage.</div>' +
        '</div>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Critique Model</label>' +
        modelDropdown('ndl-setting-critique-model', critiqueModelVal) +
        '<div class="ndl-field-hint">Model for critique/review tasks.</div>' +
        '</div>' +

        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Chat Agent</label>' +
        agentDropdown('ndl-setting-chat-agent', chatAgentVal) +
        '<div class="ndl-field-hint">Agent for interactive chat sessions.</div>' +
        '</div>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Chat Model</label>' +
        modelDropdown('ndl-setting-chat-model', chatModelVal) +
        '<div class="ndl-field-hint">Model for chat sessions.</div>' +
        '</div>' +
        '</div>';

    el.innerHTML = html;

    // Wire agent/model pairs: each agent dropdown refreshes its paired model dropdown
    var agentModelPairs = [
        {agent: 'ndl-setting-coding-agent',   model: 'ndl-setting-coding-model',   agentKey: 'defaults.coding_agent',   modelKey: 'defaults.coding_model'},
        {agent: 'ndl-setting-planning-agent', model: 'ndl-setting-planning-model', agentKey: 'defaults.planning_agent', modelKey: 'defaults.planning_model'},
        {agent: 'ndl-setting-critique-agent', model: 'ndl-setting-critique-model', agentKey: 'defaults.critique_agent', modelKey: 'defaults.critique_model'},
        {agent: 'ndl-setting-chat-agent',     model: 'ndl-setting-chat-model',     agentKey: 'defaults.chat_agent',     modelKey: 'defaults.chat_model'}
    ];

    for (var pi = 0; pi < agentModelPairs.length; pi++) {
        (function(pair) {
            var agentSel = document.getElementById(pair.agent);
            var modelSel = document.getElementById(pair.model);
            if (agentSel) {
                agentSel.addEventListener('change', function() {
                    saveConfigValue(pair.agentKey, this.value).then(function() {
                        refreshModelDropdownFor(pair.agent, pair.model, pair.modelKey);
                    });
                });
                refreshModelDropdownFor(pair.agent, pair.model, pair.modelKey);
            }
            if (modelSel) {
                modelSel.addEventListener('change', function() {
                    saveConfigValue(pair.modelKey, this.value);
                });
            }
        })(agentModelPairs[pi]);
    }

}

// Map chat agent name to the provider key used for model fetching
function agentToProvider(agent) {
    var map = { claude: 'anthropic', codex: 'openai', gemini: 'gemini' };
    return map[agent] || agent;
}

function refreshModelDropdown() {
    refreshModelDropdownFor('ndl-setting-chat-agent', 'ndl-setting-chat-model', 'defaults.chat_model');
}

function refreshModelDropdownFor(agentId, modelId, modelConfigKey) {
    var agentSelect = document.getElementById(agentId);
    var modelSelect = document.getElementById(modelId);
    if (!agentSelect || !modelSelect) return;

    var provider = agentToProvider(agentSelect.value);
    var currentModel = cfgGet(modelConfigKey, '');

    apiGet('api/v1/models/' + encodeURIComponent(provider)).then(function(data) {
        var models = (data && data.models) ? data.models : [];
        modelSelect.innerHTML = '';

        if (models.length === 0) {
            // Fallback with just the current value
            var opt = document.createElement('option');
            opt.value = currentModel;
            opt.textContent = currentModel || '(no models available)';
            opt.selected = true;
            modelSelect.appendChild(opt);
            return;
        }

        var foundCurrent = false;
        for (var i = 0; i < models.length; i++) {
            var opt = document.createElement('option');
            opt.value = models[i];
            opt.textContent = models[i];
            if (models[i] === currentModel) {
                opt.selected = true;
                foundCurrent = true;
            }
            modelSelect.appendChild(opt);
        }

        // If current model isn't in the list, prepend it
        if (!foundCurrent && currentModel) {
            var extra = document.createElement('option');
            extra.value = currentModel;
            extra.textContent = currentModel + ' (current)';
            extra.selected = true;
            modelSelect.insertBefore(extra, modelSelect.firstChild);
        }
    }).catch(function() {
        // On error keep what we have
    });
}

// ── Tab 3: Server ────────────────────────────────────────────
function renderSettingsServer() {
    var el = document.getElementById('ndl-settings-server');
    if (!el) return;

    var port = cfgGet('server.port', 8080);
    var bind = cfgGet('server.bind', '127.0.0.1');
    var autoApprove = cfgGet('server.auto_approve', false);

    var html = '<div class="ndl-setting-group"><h3>Server</h3>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Port</label>' +
        '<div class="ndl-setting-row">' +
        '<div class="ndl-toggle-row-info">' +
        '<div class="ndl-field-hint">TCP port the dashboard server listens on. Changes take effect on next server restart.</div>' +
        '</div>' +
        '<input type="number" class="ndl-field-input-short" id="ndl-setting-port" value="' + esc(String(port)) + '" min="1" max="65535">' +
        '</div></div>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Bind Address</label>' +
        '<div class="ndl-setting-row">' +
        '<div class="ndl-toggle-row-info">' +
        '<div class="ndl-field-hint">Network interface to bind. Use 127.0.0.1 for local-only access or 0.0.0.0 to listen on all interfaces. Changes take effect on next server restart.</div>' +
        '</div>' +
        '<input type="text" class="ndl-field-input-short" id="ndl-setting-bind" value="' + esc(bind) + '">' +
        '</div></div>' +
        '<div class="ndl-field">' +
        '<div class="ndl-toggle-row">' +
        '<div class="ndl-toggle-row-info">' +
        '<label class="ndl-field-label">Auto-approve</label>' +
        '<div class="ndl-field-hint">Automatically approve human gate questions without user interaction. Useful for unattended runs, but skips all safety confirmations.</div>' +
        '</div>' +
        '<button class="ndl-toggle' + (autoApprove ? ' on' : '') + '" id="ndl-setting-auto-approve"></button>' +
        '</div></div>' +
        '</div>';

    el.innerHTML = html;

    // Port
    var portInput = document.getElementById('ndl-setting-port');
    if (portInput) {
        portInput.addEventListener('change', function() {
            var v = parseInt(this.value, 10);
            if (v > 0 && v <= 65535) {
                saveConfigValue('server.port', v);
            }
        });
    }

    // Bind
    var bindInput = document.getElementById('ndl-setting-bind');
    if (bindInput) {
        bindInput.addEventListener('change', function() {
            saveConfigValue('server.bind', this.value.trim());
        });
    }

    // Auto-approve toggle
    var autoBtn = document.getElementById('ndl-setting-auto-approve');
    if (autoBtn) {
        autoBtn.addEventListener('click', function() {
            var newVal = !this.classList.contains('on');
            this.classList.toggle('on');
            NeedleState.settings.autoApprove = newVal;
            saveConfigValue('server.auto_approve', newVal);
        });
    }
}

// ── Tab 4: Appearance & Logging ──────────────────────────────
function renderSettingsAppearance() {
    var el = document.getElementById('ndl-settings-appearance');
    if (!el) return;

    var theme = cfgGet('ui.theme', NeedleState.theme || 'dark');
    var logLevel = cfgGet('logging.level', 'info');

    var html = '<div class="ndl-setting-group"><h3>Appearance</h3>' +
        '<div class="ndl-field">' +
        '<div class="ndl-toggle-row">' +
        '<div class="ndl-toggle-row-info">' +
        '<label class="ndl-field-label">Dark Theme</label>' +
        '<div class="ndl-field-hint">Toggle between dark and light color schemes. This setting is applied immediately and saved to config.</div>' +
        '</div>' +
        '<button class="ndl-toggle' + (theme === 'dark' ? ' on' : '') + '" id="ndl-setting-theme"></button>' +
        '</div></div>' +
        '<div class="ndl-field">' +
        '<div class="ndl-toggle-row">' +
        '<div class="ndl-toggle-row-info">' +
        '<label class="ndl-field-label">Gate Sound</label>' +
        '<div class="ndl-field-hint">Play a notification chime when a human gate requires attention.</div>' +
        '</div>' +
        '<button class="ndl-toggle' + (localStorage.getItem('needle-gate-sound') !== 'false' ? ' on' : '') + '" id="ndl-setting-gate-sound"></button>' +
        '</div></div>' +
        '</div>' +
        '<div class="ndl-setting-group"><h3>Logging</h3>' +
        '<div class="ndl-field">' +
        '<label class="ndl-field-label">Log Level</label>' +
        '<select class="ndl-field-select" id="ndl-setting-loglevel">' +
        '<option value="trace"' + (logLevel === 'trace' ? ' selected' : '') + '>Trace</option>' +
        '<option value="debug"' + (logLevel === 'debug' ? ' selected' : '') + '>Debug</option>' +
        '<option value="info"' + (logLevel === 'info' ? ' selected' : '') + '>Info</option>' +
        '<option value="warn"' + (logLevel === 'warn' ? ' selected' : '') + '>Warn</option>' +
        '<option value="error"' + (logLevel === 'error' ? ' selected' : '') + '>Error</option>' +
        '</select>' +
        '<div class="ndl-field-hint">Controls verbosity of ~/.needle/needle.log. Use "debug" for troubleshooting pipeline issues, "trace" for maximum detail. Default is "info".</div>' +
        '</div></div>';

    el.innerHTML = html;

    // Theme toggle
    var themeBtn = document.getElementById('ndl-setting-theme');
    if (themeBtn) {
        themeBtn.addEventListener('click', function() {
            var newTheme = this.classList.contains('on') ? 'light' : 'dark';
            this.classList.toggle('on');
            applyTheme(newTheme);
            saveConfigValue('ui.theme', newTheme);
        });
    }

    // Gate sound toggle
    var gateSoundBtn = document.getElementById('ndl-setting-gate-sound');
    if (gateSoundBtn) {
        gateSoundBtn.addEventListener('click', function() {
            var isOn = this.classList.toggle('on');
            localStorage.setItem('needle-gate-sound', isOn ? 'true' : 'false');
        });
    }

    // Log level
    var logSelect = document.getElementById('ndl-setting-loglevel');
    if (logSelect) {
        logSelect.addEventListener('change', function() {
            saveConfigValue('logging.level', this.value);
        });
    }
}

// ── Logs View ──────────────────────────────────────────────────

var _logsNeedleOffset = 0;
var _logsNeedleTimer = null;
var _logsPipelineRendered = false;

function renderLogs() {
    // Tab switching
    var tabs = document.querySelectorAll('.ndl-logs-tab');
    for (var i = 0; i < tabs.length; i++) {
        tabs[i].onclick = (function(tab) {
            return function() {
                var target = tab.getAttribute('data-logs-tab');
                var allTabs = document.querySelectorAll('.ndl-logs-tab');
                var allPanes = document.querySelectorAll('.ndl-logs-pane');
                for (var j = 0; j < allTabs.length; j++) allTabs[j].classList.remove('active');
                for (var k = 0; k < allPanes.length; k++) allPanes[k].classList.remove('active');
                tab.classList.add('active');
                var pane = document.getElementById('ndl-logs-' + target);
                if (pane) pane.classList.add('active');
                if (target === 'needle') startNeedleLogPolling();
                else stopNeedleLogPolling();
                if (target === 'pipeline') renderPipelineLogs();
            };
        })(tabs[i]);
    }

    // Render initial tab
    var activeTab = document.querySelector('.ndl-logs-tab.active');
    var tabName = activeTab ? activeTab.getAttribute('data-logs-tab') : 'pipeline';
    if (tabName === 'pipeline') renderPipelineLogs();
    else startNeedleLogPolling();
}

function renderPipelineLogs() {
    var pre = document.getElementById('ndl-logs-pipeline-pre');
    if (!pre) return;

    // Collect events from all runs
    var lines = [];
    var eventTypes = {
        'PIPELINE_STARTED': 'started', 'PIPELINE_COMPLETED': 'completed', 'PIPELINE_FAILED': 'failed',
        'STAGE_STARTED': 'started', 'STAGE_COMPLETED': 'completed', 'STAGE_FAILED': 'failed',
        'STAGE_RETRYING': 'started'
    };

    Object.keys(NeedleState.runs).forEach(function(runId) {
        var run = NeedleState.runs[runId];
        if (!run.events) return;
        run.events.forEach(function(e) {
            if (!eventTypes[e.type]) return;
            lines.push({
                ts: e.timestamp || '',
                runId: runId,
                type: e.type,
                nodeId: e.node_id || '',
                message: e.message || '',
                cls: eventTypes[e.type]
            });
        });
    });

    // Sort by timestamp
    lines.sort(function(a, b) { return a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0; });

    var html = '';
    for (var i = 0; i < lines.length; i++) {
        var l = lines[i];
        var ts = l.ts ? l.ts.replace('T', ' ').replace(/\.\d+Z$/, '') : '';
        html += '<span class="ndl-log-line">' +
            '<span class="ndl-log-ts">' + esc(ts) + '</span>' +
            '<span class="ndl-log-event-' + l.cls + '">' + esc(l.type) + '</span>' +
            (l.nodeId ? ' <strong>' + esc(l.nodeId) + '</strong>' : '') +
            ' ' + esc(l.message) +
            '</span>\n';
    }

    if (!html) html = '<span class="ndl-log-line" style="color:var(--text-muted)">No pipeline events yet. Start a run to see events here.</span>\n';

    var wasAtBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 30;
    pre.innerHTML = html;
    if (wasAtBottom) pre.scrollTop = pre.scrollHeight;
    _logsPipelineRendered = true;
}

function startNeedleLogPolling() {
    if (_logsNeedleTimer) return;
    _logsNeedleOffset = 0;
    var pre = document.getElementById('ndl-logs-needle-pre');
    if (pre) pre.innerHTML = '';
    fetchNeedleLogs();
    _logsNeedleTimer = setInterval(fetchNeedleLogs, 3000);
}

function stopNeedleLogPolling() {
    if (_logsNeedleTimer) {
        clearInterval(_logsNeedleTimer);
        _logsNeedleTimer = null;
    }
}

function fetchNeedleLogs() {
    var pre = document.getElementById('ndl-logs-needle-pre');
    if (!pre) return;

    apiGet('api/v1/logs/needle?offset=' + _logsNeedleOffset)
        .then(function(data) {
            if (!data.lines || data.lines.length === 0) {
                if (_logsNeedleOffset === 0) {
                    pre.innerHTML = '<span class="ndl-log-line" style="color:var(--text-muted)">No needle logs found. Logs are written to ~/.needle/needle.log when the server is running.</span>\n';
                }
                _logsNeedleOffset = data.offset || 0;
                return;
            }

            var wasAtBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 30;
            var html = '';
            for (var i = 0; i < data.lines.length; i++) {
                var line = data.lines[i];
                // Detect level for coloring: [INFO], [WARN], [ERROR], [DEBUG]
                var levelCls = '';
                if (line.indexOf('[INFO]') !== -1) levelCls = ' ndl-log-level-info';
                else if (line.indexOf('[WARN]') !== -1) levelCls = ' ndl-log-level-warn';
                else if (line.indexOf('[ERROR]') !== -1) levelCls = ' ndl-log-level-error';
                else if (line.indexOf('[DEBUG]') !== -1 || line.indexOf('[TRACE]') !== -1) levelCls = ' ndl-log-level-debug';
                html += '<span class="ndl-log-line' + levelCls + '">' + esc(line) + '</span>\n';
            }
            pre.innerHTML += html;
            _logsNeedleOffset = data.offset || 0;
            if (wasAtBottom) pre.scrollTop = pre.scrollHeight;
        })
        .catch(function() {});
}

// Update pipeline logs when SSE events arrive (called from onSSEMessage)
function updatePipelineLogs() {
    var hash = location.hash.slice(1) || '';
    if (hash !== 'logs') return;
    var activeTab = document.querySelector('.ndl-logs-tab.active');
    if (activeTab && activeTab.getAttribute('data-logs-tab') === 'pipeline') {
        renderPipelineLogs();
    }
}

// ── Help View ──────────────────────────────────────────────────
function renderHelp(page) {
    var sidebar = document.getElementById('ndl-help-sidebar');
    if (!sidebar) return;

    // If page param provided, switch to that page
    if (page) {
        switchHelpPage(page);
    }

    // Set up sidebar click handlers (only once)
    if (!sidebar.dataset.initialized) {
        sidebar.dataset.initialized = 'true';

        var links = sidebar.querySelectorAll('.ndl-help-link');
        for (var i = 0; i < links.length; i++) {
            links[i].addEventListener('click', (function(link) {
                return function(e) {
                    e.preventDefault();
                    var pageName = link.getAttribute('data-help-page');
                    switchHelpPage(pageName);
                };
            })(links[i]));
        }

        // Set up cross-reference links
        var content = document.getElementById('ndl-help-content');
        if (content) {
            content.addEventListener('click', function(e) {
                var xref = e.target.closest('.ndl-help-xref');
                if (xref) {
                    e.preventDefault();
                    var target = xref.getAttribute('data-help-page');
                    if (target) switchHelpPage(target);
                }
            });
        }
    }
}

function switchHelpPage(pageName) {
    // Update sidebar active state
    var links = document.querySelectorAll('.ndl-help-link');
    for (var i = 0; i < links.length; i++) {
        links[i].classList.toggle('active', links[i].getAttribute('data-help-page') === pageName);
    }

    // Show selected page, hide others
    var pages = document.querySelectorAll('.ndl-help-page');
    for (var j = 0; j < pages.length; j++) {
        pages[j].classList.toggle('active', pages[j].id === 'ndl-help-' + pageName);
    }

    // Scroll content to top
    var content = document.getElementById('ndl-help-content');
    if (content) content.scrollTop = 0;
}

// ── Human Gate Banner (inline in monitor tab) ────────────────
function renderGateBanner(run) {
    var el = document.getElementById('ndl-gate-banner');
    if (!el) return;

    if (!run || !run.pending_question) {
        el.style.display = 'none';
        el.innerHTML = '';
        return;
    }

    var runId = run.pending_gate_run_id || run.id;
    var choices = run.pending_gate_choices || [];

    var choiceHtml = '';
    if (choices.length > 0) {
        choiceHtml = '<div class="ndl-gate-choices">';
        choices.forEach(function(choice, index) {
            choiceHtml += '<button class="ndl-gate-choice-btn" data-gate-run="' + esc(runId) +
                '" data-gate-idx="' + index + '">' + esc(choice) + '</button>';
        });
        choiceHtml += '</div>';
    }

    el.style.display = 'block';
    el.innerHTML =
        '<div class="ndl-gate-banner-header">Waiting for human review</div>' +
        '<div class="ndl-gate-banner-question">' + escMultiline(run.pending_question) + '</div>' +
        choiceHtml +
        '<div class="ndl-gate-banner-input">' +
        '<textarea id="ndl-gate-feedback" rows="3" placeholder="Optional feedback..."></textarea>' +
        (choices.length === 0 ? '<button class="ndl-btn ndl-btn-primary" id="ndl-gate-text-submit" data-gate-run="' + esc(runId) + '">Submit</button>' : '') +
        '</div>';

    // Wire up choice buttons
    var btns = el.querySelectorAll('.ndl-gate-choice-btn');
    for (var i = 0; i < btns.length; i++) {
        btns[i].addEventListener('click', function() {
            var rid = this.getAttribute('data-gate-run');
            var idx = parseInt(this.getAttribute('data-gate-idx'));
            var feedback = document.getElementById('ndl-gate-feedback');
            submitGateChoice(rid, idx, feedback ? feedback.value : '');
        });
    }

    // Wire up text submit button (no-choices fallback)
    var textSubmit = document.getElementById('ndl-gate-text-submit');
    if (textSubmit) {
        textSubmit.addEventListener('click', function() {
            var rid = this.getAttribute('data-gate-run');
            var feedback = document.getElementById('ndl-gate-feedback');
            if (!feedback || !feedback.value.trim()) return;
            submitGateChoice(rid, 0, feedback.value);
        });
    }
}

// Escape text while preserving newlines as <br>
function escMultiline(text) {
    return text.split('\n').map(function(line) { return esc(line); }).join('<br>');
}

function submitGateChoice(runId, selectedIndex, rawInput) {
    var body = JSON.stringify({selected_index: selectedIndex, raw_input: rawInput || ''});
    apiSubmitAnswer(runId, body).then(function() {
        var run = NeedleState.runs[runId];
        if (run) {
            run.pending_question = '';
            run.pending_gate_choices = [];
        }
        NeedleState.pendingGates = NeedleState.pendingGates.filter(function(g) {
            return g.runId !== runId;
        });
        showToast('Answer submitted', 'success');
        refreshCurrentView();
    }).catch(function(err) {
        showToast('Failed to submit answer: ' + (err && err.message ? err.message : 'unknown error'), 'error');
    });
}

// ── Gate Sound Notification ──────────────────────────────────
function playGateNotification() {
    if (localStorage.getItem('needle-gate-sound') === 'false') return;
    try {
        var ctx = new (window.AudioContext || window.webkitAudioContext)();
        // Two-tone chime: C5 then E5
        [523.25, 659.25].forEach(function(freq, i) {
            var osc = ctx.createOscillator();
            var gain = ctx.createGain();
            osc.type = 'sine';
            osc.frequency.value = freq;
            gain.gain.setValueAtTime(0.15, ctx.currentTime + i * 0.15);
            gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + i * 0.15 + 0.3);
            osc.connect(gain);
            gain.connect(ctx.destination);
            osc.start(ctx.currentTime + i * 0.15);
            osc.stop(ctx.currentTime + i * 0.15 + 0.3);
        });
    } catch(e) { /* AudioContext not available */ }
}

// ── Theme ──────────────────────────────────────────────────────
function toggleTheme() {
    NeedleState.theme = NeedleState.theme === 'dark' ? 'light' : 'dark';
    applyTheme(NeedleState.theme);
    localStorage.setItem('needle-theme', NeedleState.theme);
    saveConfigValue('ui.theme', NeedleState.theme);
}

function applyTheme(theme) {
    NeedleState.theme = theme;
    document.documentElement.setAttribute('data-theme', theme);
    var btn = document.getElementById('ndl-theme-toggle');
    if (btn) btn.textContent = theme === 'dark' ? '\u25D0' : '\u25D1';
}

// ── Toast ──────────────────────────────────────────────────────
function showToast(message, type) {
    var el = document.getElementById('ndl-toast');
    if (!el) return;

    el.textContent = message;
    el.className = 'ndl-toast ndl-toast-' + (type || 'success') + ' visible';

    var duration = type === 'warning' ? 6000 : 4000;
    setTimeout(function() {
        el.classList.remove('visible');
    }, duration);
}

// ── Footer ─────────────────────────────────────────────────────
function updateFooter() {
    var el = document.getElementById('ndl-footer-runs');
    if (!el) return;
    var runs = Object.values(NeedleState.runs);
    var running = runs.filter(function(r) { return r.status === 'running'; }).length;
    el.textContent = running > 0 ? running + ' running' : runs.length + ' runs';
}

// ── Utilities ──────────────────────────────────────────────────
function esc(str) {
    if (!str) return '';
    var div = document.createElement('div');
    div.textContent = String(str);
    return div.innerHTML;
}

function fmtDuration(seconds) {
    if (!seconds || seconds < 0) return '0s';
    seconds = Math.floor(seconds);
    if (seconds < 60) return seconds + 's';
    var m = Math.floor(seconds / 60);
    var s = seconds % 60;
    return m + 'm ' + s + 's';
}

function fmtTimestamp(iso) {
    if (!iso) return '';
    try {
        var d = new Date(iso);
        return d.toLocaleTimeString();
    } catch(e) {
        return iso;
    }
}

function countByStatus(statuses, status) {
    var count = 0;
    Object.keys(statuses).forEach(function(k) {
        if (statuses[k] === status) count++;
    });
    return count;
}

function loadScript(url, callback) {
    var script = document.createElement('script');
    script.src = url;
    script.onload = callback || function(){};
    script.onerror = function() {
        console.warn('Failed to load: ' + url);
    };
    document.head.appendChild(script);
}

function loadCSS(url) {
    var link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = url;
    document.head.appendChild(link);
}

// ── Navigation click handlers ──────────────────────────────────
function setupNavigation() {
    var btns = document.querySelectorAll('.ndl-nav-btn');
    for (var i = 0; i < btns.length; i++) {
        btns[i].addEventListener('click', (function(view) {
            return function() { navigate(view); };
        })(btns[i].getAttribute('data-view')));
    }
}

// ── Pause/Play Controls ───────────────────────────────────────
function updatePauseUI() {
    var pauseBtn = document.getElementById('ndl-pause-btn');
    var playBtn = document.getElementById('ndl-play-btn');
    var playAtBtn = document.getElementById('ndl-play-at-btn');
    var pauseLabel = document.getElementById('ndl-pause-label');
    if (!pauseBtn || !playBtn || !playAtBtn || !pauseLabel) return;

    if (NeedleState.paused) {
        pauseBtn.style.display = 'none';
        pauseLabel.style.display = '';
        playBtn.style.display = '';
        playAtBtn.style.display = '';
        if (NeedleState.pauseResumeAt) {
            pauseLabel.textContent = 'Paused (resumes ' + new Date(NeedleState.pauseResumeAt).toLocaleTimeString() + ')';
        } else {
            pauseLabel.textContent = 'Paused';
        }
    } else {
        pauseBtn.style.display = '';
        pauseLabel.style.display = 'none';
        playBtn.style.display = 'none';
        playAtBtn.style.display = 'none';
    }
}

function handlePause() {
    apiPost('api/v1/pause').then(function(data) {
        if (data && !data.error) {
            NeedleState.paused = true;
            updatePauseUI();
        } else {
            showToast('Pause failed: ' + (data.error || 'unknown'), 'error');
        }
    }).catch(function(err) {
        showToast('Pause failed: ' + err.message, 'error');
    });
}

function handlePlay() {
    apiPost('api/v1/pause/resume').then(function(data) {
        if (data && !data.error) {
            NeedleState.paused = false;
            NeedleState.pauseResumeAt = null;
            updatePauseUI();
        } else {
            showToast('Resume failed: ' + (data.error || 'unknown'), 'error');
        }
    }).catch(function(err) {
        showToast('Resume failed: ' + err.message, 'error');
    });
}

function showTimePickerModal() {
    var overlay = document.createElement('div');
    overlay.className = 'ndl-modal-overlay';

    var modal = document.createElement('div');
    modal.className = 'ndl-modal ndl-time-picker-modal';

    var title = document.createElement('h3');
    title.textContent = 'Schedule Resume';
    title.style.marginTop = '0';
    modal.appendChild(title);

    // Absolute time
    var absLabel = document.createElement('label');
    absLabel.style.display = 'block';
    absLabel.style.marginBottom = '12px';
    var absSpan = document.createElement('span');
    absSpan.textContent = 'Resume at (absolute):';
    absSpan.style.display = 'block';
    absSpan.style.fontSize = '12px';
    absSpan.style.fontWeight = 'bold';
    absSpan.style.marginBottom = '4px';
    absLabel.appendChild(absSpan);
    var absInput = document.createElement('input');
    absInput.type = 'datetime-local';
    absInput.className = 'ndl-param-input';
    absInput.style.width = '100%';
    // Default to 30 min from now
    var def = new Date(Date.now() + 30 * 60 * 1000);
    absInput.value = def.getFullYear() + '-' +
        String(def.getMonth() + 1).padStart(2, '0') + '-' +
        String(def.getDate()).padStart(2, '0') + 'T' +
        String(def.getHours()).padStart(2, '0') + ':' +
        String(def.getMinutes()).padStart(2, '0');
    absLabel.appendChild(absInput);
    modal.appendChild(absLabel);

    // Relative duration
    var relLabel = document.createElement('label');
    relLabel.style.display = 'block';
    relLabel.style.marginBottom = '12px';
    var relSpan = document.createElement('span');
    relSpan.textContent = 'Or resume after:';
    relSpan.style.display = 'block';
    relSpan.style.fontSize = '12px';
    relSpan.style.fontWeight = 'bold';
    relSpan.style.marginBottom = '4px';
    relLabel.appendChild(relSpan);
    var relRow = document.createElement('div');
    relRow.style.display = 'flex';
    relRow.style.gap = '8px';
    var relNum = document.createElement('input');
    relNum.type = 'number';
    relNum.min = '1';
    relNum.value = '30';
    relNum.className = 'ndl-param-input';
    relNum.style.width = '80px';
    relRow.appendChild(relNum);
    var relUnit = document.createElement('select');
    relUnit.className = 'ndl-select';
    var optMin = document.createElement('option');
    optMin.value = 'minutes';
    optMin.textContent = 'minutes';
    relUnit.appendChild(optMin);
    var optHr = document.createElement('option');
    optHr.value = 'hours';
    optHr.textContent = 'hours';
    relUnit.appendChild(optHr);
    relRow.appendChild(relUnit);
    relLabel.appendChild(relRow);
    modal.appendChild(relLabel);

    // Buttons
    var actions = document.createElement('div');
    actions.style.display = 'flex';
    actions.style.gap = '8px';
    actions.style.justifyContent = 'flex-end';
    actions.style.marginTop = '16px';

    var cancelBtn = document.createElement('button');
    cancelBtn.textContent = 'Cancel';
    cancelBtn.className = 'ndl-btn';
    cancelBtn.onclick = function() { document.body.removeChild(overlay); };
    actions.appendChild(cancelBtn);

    var schedBtn = document.createElement('button');
    schedBtn.textContent = 'Schedule Resume';
    schedBtn.className = 'ndl-btn-primary';
    schedBtn.onclick = function() {
        // Determine which input was used — prefer relative if modified
        var resumeAt;
        if (relNum.value && parseInt(relNum.value, 10) > 0) {
            var ms = parseInt(relNum.value, 10) * (relUnit.value === 'hours' ? 3600000 : 60000);
            resumeAt = new Date(Date.now() + ms).toISOString();
        } else if (absInput.value) {
            resumeAt = new Date(absInput.value).toISOString();
        }
        if (!resumeAt) {
            showToast('Please set a time', 'warning');
            return;
        }
        apiPost('api/v1/pause/schedule', {resume_at: resumeAt}).then(function(data) {
            if (data && !data.error) {
                NeedleState.pauseResumeAt = resumeAt;
                updatePauseUI();
                showToast('Resume scheduled for ' + new Date(resumeAt).toLocaleTimeString(), 'success');
            } else {
                showToast('Schedule failed: ' + (data.error || 'unknown'), 'error');
            }
        }).catch(function(err) {
            showToast('Schedule failed: ' + err.message, 'error');
        });
        document.body.removeChild(overlay);
    };
    actions.appendChild(schedBtn);

    modal.appendChild(actions);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);
}

function fetchPauseStatus() {
    apiGet('api/v1/pause/status').then(function(data) {
        if (data) {
            NeedleState.paused = !!data.paused;
            NeedleState.pauseResumeAt = data.resume_at || null;
            updatePauseUI();
        }
    }).catch(function() { /* ignore */ });
}

// ── Init ───────────────────────────────────────────────────────
function init() {
    // Load saved settings
    loadSettings();

    // Apply theme
    applyTheme(localStorage.getItem('needle-theme') || 'dark');

    // Setup navigation
    setupNavigation();
    setupSettingsTabs();
    window.addEventListener('hashchange', onHashChange);

    // Theme toggle
    var themeBtn = document.getElementById('ndl-theme-toggle');
    if (themeBtn) themeBtn.addEventListener('click', toggleTheme);

    // Gate sound preference init
    if (localStorage.getItem('needle-gate-sound') === null) {
        localStorage.setItem('needle-gate-sound', 'true');
    }

    // Create view textarea change listener (for before CodeMirror loads)
    var textarea = document.getElementById('ndl-editor-textarea');
    if (textarea) {
        textarea.addEventListener('input', schedulePreviewUpdate);
    }

    // Keyboard shortcuts
    document.addEventListener('keydown', function(e) {
        // Skip if user is typing in an input, textarea, or select
        var tag = (e.target.tagName || '').toLowerCase();
        if (tag === 'input' || tag === 'textarea' || tag === 'select') return;
        // Skip if a CodeMirror editor is focused
        if (e.target.closest && e.target.closest('.CodeMirror')) return;

        var views = ['dashboard', 'monitor', 'create', 'settings', 'logs', 'help'];
        // Number keys 1-6 for view switching
        if (!e.ctrlKey && !e.metaKey && !e.altKey) {
            var num = parseInt(e.key);
            if (num >= 1 && num <= views.length) {
                e.preventDefault();
                navigate(views[num - 1]);
                return;
            }
            // T for theme toggle
            if (e.key === 't' || e.key === 'T') {
                e.preventDefault();
                toggleTheme();
                return;
            }
            if (e.key === 'Escape') {
                return;
            }
            // Graph zoom shortcuts (when on monitor view)
            var hash = location.hash.slice(1) || 'dashboard';
            if (hash.indexOf('monitor') === 0) {
                if (e.key === '+' || e.key === '=') {
                    e.preventDefault();
                    zoomGraph(1.15);
                    return;
                }
                if (e.key === '-') {
                    e.preventDefault();
                    zoomGraph(1 / 1.15);
                    return;
                }
                if (e.key === '0') {
                    e.preventDefault();
                    var container = document.getElementById('ndl-graph-container');
                    if (container) {
                        container._gzZoom = 1;
                        container._gzPanX = 0;
                        container._gzPanY = 0;
                        applyGraphTransformFor(container);
                    }
                    return;
                }
                if (e.key === 'f' || e.key === 'F') {
                    e.preventDefault();
                    fitGraphToView();
                    return;
                }
            }
        }
    });

    // Pause/play controls
    var pauseBtn = document.getElementById('ndl-pause-btn');
    if (pauseBtn) pauseBtn.addEventListener('click', handlePause);
    var playBtn = document.getElementById('ndl-play-btn');
    if (playBtn) playBtn.addEventListener('click', handlePlay);
    var playAtBtn = document.getElementById('ndl-play-at-btn');
    if (playAtBtn) playAtBtn.addEventListener('click', showTimePickerModal);

    // Initialize viz.js early so it's available for both Create and Monitor views
    initVizJs();

    // Connect SSE and load initial state
    connectSSE();
    reconcileState();

    // Fetch pause status on load
    fetchPauseStatus();

    // If no hash is set, check if we have an empty workspace (no graph loaded)
    // and default to Create view
    if (!location.hash) {
        var graphContainer = document.getElementById('ndl-graph-container');
        var hasSvg = graphContainer && graphContainer.querySelector('svg');
        if (!hasSvg) {
            location.hash = '#create';
        }
    }

    // Render initial view
    onHashChange();

    // Update elapsed timers every second
    setInterval(function() {
        Object.values(NeedleState.runs).forEach(function(run) {
            if (run.status === 'running' && run.start_time) {
                run.elapsed_seconds = (Date.now() - new Date(run.start_time).getTime()) / 1000;
            }
        });
        var hash = location.hash.slice(1) || 'dashboard';
        if (hash === 'dashboard') renderDashboard();
    }, 1000);

    updateFooter();
}

// Start
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}

})();
