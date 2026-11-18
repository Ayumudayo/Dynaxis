import {
    ApiError,
    clearAuthToken,
    fetchOverview,
    fetchServiceDetail,
    getAuthToken,
    setAuthToken,
} from '/js/api.js';
import {
    renderFilterChips,
    renderInspectorDrawer,
    renderInventoryTable,
    renderKpis,
    renderObservedAge,
    renderTopMeta,
    renderTopologyStrip,
} from '/js/render.js';

const state = {
    overview: null,
    selectedServiceId: null,
    selectedDetail: null,
    activeStatusFilter: 'all',
    searchQuery: '',
    pollingEnabled: true,
    pollIntervalMs: 5000,
    staleTtlMs: 15000,
    baseObservedAgeMs: 0,
    lastSyncPerfMs: 0,
    hasSynced: false,
    inFlight: false,
    pollTimerId: null,
};

function currentEffectiveAgeMs() {
    if (!state.hasSynced) {
        return null;
    }
    const elapsed = Math.max(0, window.performance.now() - state.lastSyncPerfMs);
    return state.baseObservedAgeMs + elapsed;
}

function getFilteredServices() {
    const services = Array.isArray(state.overview && state.overview.services)
        ? state.overview.services
        : [];
    const q = state.searchQuery.trim().toLowerCase();

    return services.filter((svc) => {
        if (state.activeStatusFilter !== 'all' && svc.status !== state.activeStatusFilter) {
            return false;
        }
        if (q) {
            const id = String(svc.service_id || '').toLowerCase();
            const name = String(svc.display_name || '').toLowerCase();
            if (!id.includes(q) && !name.includes(q)) {
                return false;
            }
        }
        return true;
    });
}

function findSelectedService() {
    if (!state.selectedServiceId) {
        return null;
    }
    const services = Array.isArray(state.overview && state.overview.services)
        ? state.overview.services
        : [];
    const fromOverview = services.find((s) => s.service_id === state.selectedServiceId) || null;
    if (state.selectedDetail && state.selectedDetail.service_id === state.selectedServiceId) {
        return { ...(fromOverview || {}), ...state.selectedDetail };
    }
    return fromOverview;
}

function isObservabilityEnabled() {
    return Boolean(state.overview && state.overview.meta && state.overview.meta.observability_enabled);
}

function renderAll() {
    const services = Array.isArray(state.overview && state.overview.services)
        ? state.overview.services
        : [];
    const filtered = getFilteredServices();
    const ageMs = currentEffectiveAgeMs();

    renderTopMeta(state.overview && state.overview.meta);
    renderKpis(state.overview, ageMs);
    renderTopologyStrip(services, state.selectedServiceId, handleSelectService);
    renderFilterChips(services, state.activeStatusFilter, handleSelectFilter);
    renderInventoryTable(filtered, services.length, state.selectedServiceId, handleSelectService);
    renderInspectorDrawer(findSelectedService(), isObservabilityEnabled());
}

function showErrorBanner(text, showAuthAction) {
    const banner = document.getElementById('error-banner');
    const textEl = document.getElementById('error-banner-text');
    const retryBtn = document.getElementById('btn-retry-auth');
    if (!banner || !textEl) {
        return;
    }
    textEl.textContent = text;
    if (retryBtn) {
        retryBtn.classList.toggle('hidden', !showAuthAction);
    }
    banner.classList.remove('hidden');
}

function hideErrorBanner() {
    const banner = document.getElementById('error-banner');
    if (banner) {
        banner.classList.add('hidden');
    }
}

function openAuthModal() {
    const modal = document.getElementById('auth-modal');
    const input = document.getElementById('token-input');
    if (!modal) {
        return;
    }
    if (input) {
        input.value = getAuthToken();
    }
    modal.classList.remove('hidden');
    if (input) {
        input.focus();
    }
}

function closeAuthModal() {
    const modal = document.getElementById('auth-modal');
    if (modal) {
        modal.classList.add('hidden');
    }
}

async function handleSelectService(serviceId) {
    state.selectedServiceId = serviceId;
    state.selectedDetail = null;
    renderAll();

    try {
        const res = await fetchServiceDetail(serviceId);
        if (state.selectedServiceId === serviceId && res && res.service) {
            state.selectedDetail = res.service;
            renderInspectorDrawer(findSelectedService(), isObservabilityEnabled());
        }
    } catch (_err) {
        // Overview item remains visible in drawer even if point detail call fails
    }
}

function handleSelectFilter(filterKey) {
    state.activeStatusFilter = filterKey;
    renderAll();
}

function handleCloseDrawer() {
    state.selectedServiceId = null;
    state.selectedDetail = null;
    renderAll();
}

function scheduleNextPoll() {
    if (state.pollTimerId !== null) {
        window.clearTimeout(state.pollTimerId);
        state.pollTimerId = null;
    }
    if (!state.pollingEnabled) {
        return;
    }
    state.pollTimerId = window.setTimeout(() => {
        refreshOverview(false);
    }, state.pollIntervalMs);
}

async function refreshOverview(fromUserAction) {
    if (state.inFlight) {
        return;
    }
    state.inFlight = true;
    try {
        const data = await fetchOverview();
        state.overview = data;
        state.baseObservedAgeMs = typeof data.observed_age_ms === 'number' ? data.observed_age_ms : 0;
        state.lastSyncPerfMs = window.performance.now();
        state.hasSynced = true;

        if (data.meta) {
            if (typeof data.meta.poll_interval_ms === 'number' && data.meta.poll_interval_ms >= 500) {
                state.pollIntervalMs = data.meta.poll_interval_ms;
            }
            if (typeof data.meta.stale_ttl_ms === 'number' && data.meta.stale_ttl_ms >= 1000) {
                state.staleTtlMs = data.meta.stale_ttl_ms;
            }
        }

        if (state.selectedServiceId && Array.isArray(data.services)) {
            const match = data.services.find((s) => s.service_id === state.selectedServiceId);
            if (match) {
                state.selectedDetail = match;
            }
        }

        hideErrorBanner();
        renderAll();
    } catch (err) {
        if (err instanceof ApiError && err.status === 401) {
            showErrorBanner(`인증 필요 (401): ${err.message}`, true);
            if (fromUserAction || !getAuthToken()) {
                openAuthModal();
            }
        } else if (err instanceof ApiError && err.status === 503 && err.code === 'auth_unconfigured') {
            showErrorBanner(`서버 토큰 미설정 (503): ${err.message}`, false);
        } else {
            showErrorBanner(`API 폴링 오류: ${err && err.message ? err.message : 'unknown error'}`, false);
        }
    } finally {
        state.inFlight = false;
        scheduleNextPoll();
    }
}

function bindEvents() {
    const searchEl = document.getElementById('search-input');
    if (searchEl) {
        searchEl.addEventListener('input', (e) => {
            state.searchQuery = e.target.value || '';
            renderAll();
        });
    }

    const refreshBtn = document.getElementById('btn-refresh');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', () => refreshOverview(true));
    }

    const togglePollBtn = document.getElementById('btn-toggle-poll');
    if (togglePollBtn) {
        togglePollBtn.addEventListener('click', () => {
            state.pollingEnabled = !state.pollingEnabled;
            togglePollBtn.textContent = state.pollingEnabled ? 'Auto-Poll: ON' : 'Auto-Poll: PAUSED';
            if (state.pollingEnabled) {
                refreshOverview(false);
            } else if (state.pollTimerId !== null) {
                window.clearTimeout(state.pollTimerId);
                state.pollTimerId = null;
            }
        });
    }

    const closeDrawerBtn = document.getElementById('btn-close-drawer');
    if (closeDrawerBtn) {
        closeDrawerBtn.addEventListener('click', handleCloseDrawer);
    }

    const authBtn = document.getElementById('btn-auth-modal');
    if (authBtn) {
        authBtn.addEventListener('click', openAuthModal);
    }

    const retryAuthBtn = document.getElementById('btn-retry-auth');
    if (retryAuthBtn) {
        retryAuthBtn.addEventListener('click', openAuthModal);
    }

    const cancelTokenBtn = document.getElementById('btn-cancel-token');
    if (cancelTokenBtn) {
        cancelTokenBtn.addEventListener('click', closeAuthModal);
    }

    const clearTokenBtn = document.getElementById('btn-clear-token');
    if (clearTokenBtn) {
        clearTokenBtn.addEventListener('click', () => {
            clearAuthToken();
            closeAuthModal();
            refreshOverview(true);
        });
    }

    const saveTokenBtn = document.getElementById('btn-save-token');
    const tokenInput = document.getElementById('token-input');
    const submitToken = () => {
        setAuthToken(tokenInput ? tokenInput.value : '');
        closeAuthModal();
        refreshOverview(true);
    };
    if (saveTokenBtn) {
        saveTokenBtn.addEventListener('click', submitToken);
    }
    if (tokenInput) {
        tokenInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                submitToken();
            }
        });
    }

    window.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') {
            const modal = document.getElementById('auth-modal');
            if (modal && !modal.classList.contains('hidden')) {
                closeAuthModal();
                return;
            }
            if (state.selectedServiceId) {
                handleCloseDrawer();
            }
        }
    });

    // 1-second local clock tick so Observed Age reflects elapsed time between polls
    window.setInterval(() => {
        const ageMs = currentEffectiveAgeMs();
        if (ageMs !== null) {
            renderObservedAge(ageMs, state.staleTtlMs);
        }
    }, 1000);
}

bindEvents();
refreshOverview(false);
