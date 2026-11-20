const STATUS_META = {
    ready: { symbol: '●', label: 'Ready' },
    degraded: { symbol: '▲', label: 'Degraded' },
    not_ready: { symbol: '✕', label: 'Not Ready' },
    completed: { symbol: '✓', label: 'Completed' },
    unknown: { symbol: '?', label: 'Unknown / Stale' },
};

const LAYER_ORDER = ['edge', 'gateway', 'server', 'worker', 'data', 'observability'];

const LAYER_LABELS = {
    edge: 'Edge',
    gateway: 'Gateway',
    server: 'Server',
    worker: 'Worker',
    data: 'Data',
    observability: 'Observability',
};

const STATUS_FILTERS = [
    { key: 'all', label: 'All' },
    { key: 'not_ready', label: '✕ Not Ready' },
    { key: 'degraded', label: '▲ Degraded' },
    { key: 'unknown', label: '? Unknown' },
    { key: 'ready', label: '● Ready' },
    { key: 'completed', label: '✓ Completed' },
];

export function isSafeHttpUrl(raw) {
    if (typeof raw !== 'string') {
        return false;
    }
    const trimmed = raw.trim();
    if (!trimmed) {
        return false;
    }
    try {
        const parsed = new URL(trimmed, window.location.origin);
        return parsed.protocol === 'http:' || parsed.protocol === 'https:';
    } catch (_err) {
        return false;
    }
}

export function formatAgeMs(rawMs) {
    if (typeof rawMs !== 'number' || Number.isNaN(rawMs) || rawMs < 0) {
        return '--';
    }
    if (rawMs < 1000) {
        return `${Math.round(rawMs)}ms`;
    }
    const totalSec = Math.floor(rawMs / 1000);
    if (totalSec < 60) {
        const tenths = Math.floor((rawMs % 1000) / 100);
        return `${totalSec}.${tenths}s`;
    }
    const mins = Math.floor(totalSec / 60);
    const secs = totalSec % 60;
    const padded = secs < 10 ? `0${secs}` : String(secs);
    return `${mins}m ${padded}s`;
}

export function formatTimestampMs(unixMs) {
    if (typeof unixMs !== 'number' || unixMs <= 0) {
        return '—';
    }
    try {
        const d = new Date(unixMs);
        return d.toLocaleTimeString('ko-KR', {
            hour12: false,
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    } catch (_err) {
        return String(unixMs);
    }
}

export function createStatusBadge(statusKey) {
    const key = Object.prototype.hasOwnProperty.call(STATUS_META, statusKey) ? statusKey : 'unknown';
    const spec = STATUS_META[key];

    const badge = document.createElement('span');
    badge.className = `status-badge status-badge--${key}`;

    const sym = document.createElement('span');
    sym.setAttribute('aria-hidden', 'true');
    sym.textContent = spec.symbol;

    const txt = document.createElement('span');
    txt.textContent = spec.label;

    badge.append(sym, txt);
    return badge;
}

export function formatDomainSignal(svc) {
    const parts = [];
    if (typeof svc.active_sessions === 'number' && svc.active_sessions >= 0) {
        parts.push(`sessions=${svc.active_sessions}`);
    }
    if (typeof svc.wb_pending === 'number' && svc.wb_pending >= 0) {
        parts.push(`pending=${svc.wb_pending}`);
    }
    if (typeof svc.wb_dlq_count === 'number' && svc.wb_dlq_count >= 0) {
        parts.push(`dlq=${svc.wb_dlq_count}`);
    }
    if (typeof svc.active_rooms === 'number' && svc.active_rooms >= 0) {
        parts.push(`rooms=${svc.active_rooms}`);
    }
    if (typeof svc.expected_migrations === 'number' && svc.expected_migrations > 0) {
        const applied = typeof svc.applied_migrations === 'number' && svc.applied_migrations >= 0 ? svc.applied_migrations : '?';
        parts.push(`mig=${applied}/${svc.expected_migrations}`);
    }
    return parts.length > 0 ? parts.join(' · ') : '—';
}

function updateExternalLink(anchorEl, url) {
    if (!anchorEl) {
        return;
    }
    if (isSafeHttpUrl(url)) {
        anchorEl.href = url;
        anchorEl.classList.remove('hidden');
    } else {
        anchorEl.removeAttribute('href');
        anchorEl.classList.add('hidden');
    }
}

export function renderTopMeta(meta) {
    const versionEl = document.getElementById('meta-version');
    const envEl = document.getElementById('meta-env');
    const roEl = document.getElementById('meta-readonly');
    const authEl = document.getElementById('meta-auth');
    const pollEl = document.getElementById('meta-poll-interval');
    const ttlEl = document.getElementById('meta-stale-ttl');
    const obsEl = document.getElementById('meta-obs-flag');

    if (!meta) {
        return;
    }

    if (versionEl) {
        versionEl.textContent = meta.version || 'v1';
    }
    if (envEl) {
        const label = String(meta.env_label || 'LOCAL').toUpperCase();
        envEl.textContent = label;
        envEl.className = 'pill mono';
        if (label === 'PROD') {
            envEl.classList.add('pill--env-prod');
        } else if (label === 'DEV' || label === 'STAGING') {
            envEl.classList.add('pill--env-dev');
        }
    }
    if (roEl) {
        roEl.classList.toggle('hidden', !meta.read_only);
    }
    if (authEl) {
        const mode = meta.auth_mode || 'token';
        const role = meta.actor_role || 'viewer';
        authEl.textContent = `${mode.toUpperCase()} · ${role}`;
    }
    if (pollEl) {
        pollEl.textContent = formatAgeMs(meta.poll_interval_ms);
    }
    if (ttlEl) {
        ttlEl.textContent = formatAgeMs(meta.stale_ttl_ms);
    }
    if (obsEl) {
        obsEl.textContent = meta.observability_enabled ? 'ON' : 'OFF';
    }

    const links = meta.links || {};
    updateExternalLink(document.getElementById('link-grafana'), meta.observability_enabled ? links.grafana : '');
    updateExternalLink(document.getElementById('link-prometheus'), meta.observability_enabled ? links.prometheus : '');
    updateExternalLink(document.getElementById('link-haproxy'), links.haproxy);
}

export function renderObservedAge(effectiveAgeMs, staleTtlMs) {
    const pill = document.getElementById('observed-age-pill');
    const kpiVal = document.getElementById('kpi-freshness');
    const kpiSub = document.getElementById('kpi-freshness-sub');

    const text = formatAgeMs(effectiveAgeMs);
    const stale = typeof staleTtlMs === 'number' && staleTtlMs > 0 && effectiveAgeMs > staleTtlMs;

    if (pill) {
        pill.textContent = stale ? `Observed: ${text} (STALE)` : `Observed: ${text}`;
        pill.className = stale ? 'pill num pill--stale' : 'pill num';
    }
    if (kpiVal) {
        kpiVal.textContent = text;
    }
    if (kpiSub && typeof staleTtlMs === 'number') {
        kpiSub.textContent = stale
            ? `TTL (${formatAgeMs(staleTtlMs)}) 초과 · Unknown 강등 위험`
            : `TTL ${formatAgeMs(staleTtlMs)} 이내 정상 스냅샷`;
    }
}

export function renderKpis(overview, effectiveAgeMs) {
    const kpis = (overview && overview.kpis) || {};
    const meta = (overview && overview.meta) || {};
    const services = Array.isArray(overview && overview.services) ? overview.services : [];

    const readyEl = document.getElementById('kpi-ready');
    const readySubEl = document.getElementById('kpi-ready-sub');
    const sessionsEl = document.getElementById('kpi-sessions');
    const sessionsSubEl = document.getElementById('kpi-sessions-sub');
    const wbEl = document.getElementById('kpi-wb');
    const wbSubEl = document.getElementById('kpi-wb-sub');

    if (readyEl) {
        const r = kpis.ready_services ?? 0;
        const d = kpis.desired_services ?? 0;
        readyEl.textContent = `${r} / ${d}`;
    }
    if (readySubEl) {
        const notReadyCount = services.filter((s) => s.status === 'not_ready').length;
        const degradedCount = services.filter((s) => s.status === 'degraded').length;
        const unknownCount = services.filter((s) => s.status === 'unknown').length;
        readySubEl.textContent = `Not Ready ${notReadyCount} · Degraded ${degradedCount} · Unknown ${unknownCount}`;
    }
    if (sessionsEl) {
        const gw = kpis.gateway_sessions ?? 0;
        const sv = kpis.server_sessions ?? 0;
        sessionsEl.textContent = `${gw} / ${sv}`;
    }
    if (sessionsSubEl) {
        const rooms = typeof kpis.active_rooms === 'number' && kpis.active_rooms >= 0 ? kpis.active_rooms : '—';
        sessionsSubEl.textContent = `Active Rooms (PG): ${rooms}`;
    }
    if (wbEl) {
        const pending = typeof kpis.wb_pending === 'number' && kpis.wb_pending >= 0 ? kpis.wb_pending : '—';
        const dlq = typeof kpis.wb_dlq_count === 'number' && kpis.wb_dlq_count >= 0 ? kpis.wb_dlq_count : '—';
        wbEl.textContent = `${pending} / ${dlq}`;
    }
    if (wbSubEl) {
        const total = typeof kpis.wb_dlq_total === 'number' ? kpis.wb_dlq_total : 0;
        wbSubEl.textContent = `Cumulative DLQ Events: ${total}`;
    }

    renderObservedAge(effectiveAgeMs, meta.stale_ttl_ms);
}

export function renderTopologyStrip(services, selectedServiceId, onSelectService) {
    const container = document.getElementById('topology-strip');
    if (!container) {
        return;
    }

    const byLayer = new Map();
    for (const layer of LAYER_ORDER) {
        byLayer.set(layer, []);
    }
    for (const svc of services) {
        const key = byLayer.has(svc.layer) ? svc.layer : 'server';
        byLayer.get(key).push(svc);
    }

    const fragment = document.createDocumentFragment();
    let visibleLayerCount = 0;

    for (const layer of LAYER_ORDER) {
        const items = byLayer.get(layer) || [];
        if (items.length === 0) {
            continue;
        }
        if (visibleLayerCount > 0) {
            const arrow = document.createElement('div');
            arrow.className = 'topo-arrow';
            arrow.setAttribute('aria-hidden', 'true');
            arrow.textContent = '→';
            fragment.appendChild(arrow);
        }
        visibleLayerCount += 1;

        const col = document.createElement('div');
        col.className = 'topo-layer';

        const title = document.createElement('div');
        title.className = 'topo-layer__title';
        title.textContent = LAYER_LABELS[layer] || layer;
        col.appendChild(title);

        for (const svc of items) {
            const btn = document.createElement('button');
            btn.type = 'button';
            btn.className = 'topo-node';
            if (svc.service_id === selectedServiceId) {
                btn.classList.add('topo-node--selected');
            }
            btn.addEventListener('click', () => onSelectService(svc.service_id));

            const top = document.createElement('div');
            top.className = 'topo-node__top';

            const nameSpan = document.createElement('span');
            nameSpan.className = 'topo-node__name';
            nameSpan.textContent = svc.display_name || svc.service_id;

            top.append(nameSpan, createStatusBadge(svc.status));

            const metaRow = document.createElement('div');
            metaRow.className = 'topo-node__meta num';

            const leftMeta = document.createElement('span');
            leftMeta.textContent = `${svc.ready_count ?? 0}/${svc.desired_count ?? 1}`;

            const rightMeta = document.createElement('span');
            const signal = formatDomainSignal(svc);
            rightMeta.textContent = signal !== '—' ? signal : `${svc.probe_latency_ms ?? 0}ms`;

            metaRow.append(leftMeta, rightMeta);
            btn.append(top, metaRow);
            col.appendChild(btn);
        }

        fragment.appendChild(col);
    }

    if (visibleLayerCount === 0) {
        const empty = document.createElement('div');
        empty.className = 'empty-box';
        empty.textContent = '등록된 토폴로지 대상이 없습니다.';
        fragment.appendChild(empty);
    }

    container.replaceChildren(fragment);
}

export function renderFilterChips(services, activeFilter, onSelectFilter) {
    const container = document.getElementById('status-filter-chips');
    if (!container) {
        return;
    }

    const counts = { all: services.length };
    for (const svc of services) {
        const s = svc.status || 'unknown';
        counts[s] = (counts[s] || 0) + 1;
    }

    const fragment = document.createDocumentFragment();
    for (const f of STATUS_FILTERS) {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = f.key === activeFilter ? 'chip chip--active num' : 'chip num';
        btn.textContent = `${f.label} (${counts[f.key] || 0})`;
        btn.addEventListener('click', () => onSelectFilter(f.key));
        fragment.appendChild(btn);
    }

    container.replaceChildren(fragment);
}

export function renderInventoryTable(filteredServices, totalCount, selectedServiceId, onSelectService) {
    const tbody = document.getElementById('inventory-tbody');
    const emptyEl = document.getElementById('inventory-empty');
    const countEl = document.getElementById('inventory-count');

    if (countEl) {
        countEl.textContent = `${filteredServices.length} / ${totalCount} services`;
    }

    if (!tbody) {
        return;
    }

    const fragment = document.createDocumentFragment();

    for (const svc of filteredServices) {
        const tr = document.createElement('tr');
        tr.className = svc.service_id === selectedServiceId ? 'inventory-row inventory-row--selected' : 'inventory-row';
        tr.tabIndex = 0;
        tr.addEventListener('click', () => onSelectService(svc.service_id));
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                onSelectService(svc.service_id);
            }
        });

        const tdId = document.createElement('td');
        tdId.className = 'mono';
        tdId.textContent = svc.service_id || '';

        const tdName = document.createElement('td');
        tdName.textContent = svc.display_name || '';

        const tdLayer = document.createElement('td');
        tdLayer.className = 'mono';
        tdLayer.textContent = `${svc.layer || '-'} / ${svc.kind || '-'}`;

        const tdStatus = document.createElement('td');
        tdStatus.appendChild(createStatusBadge(svc.status));

        const tdReady = document.createElement('td');
        tdReady.className = 'num';
        tdReady.textContent = `${svc.ready_count ?? 0} / ${svc.desired_count ?? 1}`;

        const tdLatency = document.createElement('td');
        tdLatency.className = 'num';
        tdLatency.textContent = `${svc.probe_latency_ms ?? 0}ms`;

        const tdAge = document.createElement('td');
        tdAge.className = 'num';
        tdAge.textContent = formatAgeMs(svc.observed_age_ms);

        const tdSignal = document.createElement('td');
        tdSignal.className = 'num';
        tdSignal.textContent = formatDomainSignal(svc);

        const tdDetail = document.createElement('td');
        tdDetail.className = 'cell-detail';
        tdDetail.textContent = svc.detail || '—';
        tdDetail.title = svc.detail || '';

        tr.append(tdId, tdName, tdLayer, tdStatus, tdReady, tdLatency, tdAge, tdSignal, tdDetail);
        fragment.appendChild(tr);
    }

    tbody.replaceChildren(fragment);

    if (emptyEl) {
        emptyEl.classList.toggle('hidden', filteredServices.length > 0);
    }
}

function makeKvCard(label, value, mono = false) {
    const card = document.createElement('div');
    card.className = 'kv-card';

    const k = document.createElement('div');
    k.className = 'kv-card__k';
    k.textContent = label;

    const v = document.createElement('div');
    v.className = mono ? 'kv-card__v mono' : 'kv-card__v num';
    v.textContent = String(value);

    card.append(k, v);
    return card;
}

export function renderInspectorDrawer(service, observabilityEnabled = false) {
    const drawer = document.getElementById('inspector-drawer');
    const titleEl = document.getElementById('drawer-title');
    const subtitleEl = document.getElementById('drawer-subtitle');
    const bodyEl = document.getElementById('drawer-body');

    if (!drawer || !bodyEl) {
        return;
    }

    if (!service) {
        drawer.classList.add('hidden');
        bodyEl.replaceChildren();
        return;
    }

    drawer.classList.remove('hidden');
    if (titleEl) {
        titleEl.textContent = service.display_name || service.service_id;
    }
    if (subtitleEl) {
        subtitleEl.textContent = `${service.service_id} · ${service.layer} / ${service.kind}`;
    }

    const fragment = document.createDocumentFragment();

    const badgeWrap = document.createElement('div');
    badgeWrap.appendChild(createStatusBadge(service.status));
    fragment.appendChild(badgeWrap);

    const grid = document.createElement('div');
    grid.className = 'kv-grid';
    grid.append(
        makeKvCard('Ready / Desired', `${service.ready_count ?? 0} / ${service.desired_count ?? 1}`),
        makeKvCard('Probe Latency', `${service.probe_latency_ms ?? 0} ms`),
        makeKvCard('/healthz', service.health_ok ? 'PASS' : 'FAIL', true),
        makeKvCard('/readyz', service.ready_ok ? 'PASS' : 'FAIL', true),
        makeKvCard('Observed At', formatTimestampMs(service.observed_at_unix_ms), true),
        makeKvCard('Observed Age', formatAgeMs(service.observed_age_ms)),
    );

    if (typeof service.active_sessions === 'number' && service.active_sessions >= 0) {
        grid.appendChild(makeKvCard('Active Sessions', service.active_sessions));
    }
    if (typeof service.active_rooms === 'number' && service.active_rooms >= 0) {
        grid.appendChild(makeKvCard('Active Rooms', service.active_rooms));
    }
    if (typeof service.wb_pending === 'number' && service.wb_pending >= 0) {
        grid.appendChild(makeKvCard('Write-Behind Pending', service.wb_pending));
    }
    if (typeof service.wb_dlq_count === 'number' && service.wb_dlq_count >= 0) {
        grid.appendChild(makeKvCard('DLQ Stream Length', service.wb_dlq_count));
    }
    if (typeof service.wb_dlq_total === 'number' && service.wb_dlq_total > 0) {
        grid.appendChild(makeKvCard('DLQ Total Counter', service.wb_dlq_total));
    }
    if (typeof service.expected_migrations === 'number' && service.expected_migrations > 0) {
        const applied = typeof service.applied_migrations === 'number' && service.applied_migrations >= 0 ? service.applied_migrations : '?';
        grid.appendChild(makeKvCard('Schema Migrations', `${applied} / ${service.expected_migrations}`));
    }

    fragment.appendChild(grid);

    const detailSection = document.createElement('div');
    const detailLabel = document.createElement('div');
    detailLabel.className = 'kv-card__k';
    detailLabel.textContent = 'Probe Detail';
    const detailBox = document.createElement('div');
    detailBox.className = 'detail-box mono';
    detailBox.textContent = service.detail || 'No detail message recorded.';
    detailSection.append(detailLabel, detailBox);
    fragment.appendChild(detailSection);

    const actions = document.createElement('div');
    actions.className = 'drawer-actions';
    if (observabilityEnabled && isSafeHttpUrl(service.grafana_url)) {
        const gBtn = document.createElement('a');
        gBtn.className = 'btn btn--primary';
        gBtn.target = '_blank';
        gBtn.rel = 'noopener noreferrer';
        gBtn.href = service.grafana_url;
        gBtn.textContent = 'Open Grafana Dashboard';
        actions.appendChild(gBtn);
    }
    if (isSafeHttpUrl(service.external_url)) {
        const eBtn = document.createElement('a');
        eBtn.className = 'btn';
        eBtn.target = '_blank';
        eBtn.rel = 'noopener noreferrer';
        eBtn.href = service.external_url;
        eBtn.textContent = 'Open Service Console';
        actions.appendChild(eBtn);
    }
    if (actions.childElementCount > 0) {
        fragment.appendChild(actions);
    }

    bodyEl.replaceChildren(fragment);
}
