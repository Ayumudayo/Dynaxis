import {
    ApiError,
    applyConfigChanges,
    clearAuthToken,
    closeChatRoom,
    createRestartOperation,
    createSanction,
    fetchAuditLogs,
    fetchChatRoomDetail,
    fetchChatRooms,
    fetchChatUserDetail,
    fetchChatUsers,
    fetchConfigOverview,
    fetchOperationDetail,
    fetchOperations,
    fetchOverview,
    fetchSanctions,
    fetchServiceDetail,
    fetchWbDlq,
    generateIdempotencyKey,
    getAuthToken,
    kickChatUser,
    replayWbDlq,
    revokeSanction,
    rollbackConfigRevision,
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
import {
    renderAuditView,
    renderModerationView,
    renderRoomDrawer,
    renderRoomsView,
    renderUserDrawer,
    renderUsersView,
} from '/js/views_v2.js';
import {
    renderConfigView,
    renderOperationStepperDrawer,
    renderOperationsView,
    renderRestartConfirmDrawer,
    renderRollbackDrawer,
} from '/js/views_v3.js';

const state = {
    activeView: 'overview',
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
    v2: {
        rooms: [],
        roomQuery: '',
        includeClosedRooms: false,
        users: [],
        userQuery: '',
        sanctions: [],
        sanctionActiveOnly: true,
        sanctionFilterSubject: '',
        dlq: null,
        auditLogs: [],
        auditTarget: '',
        auditAction: '',
        auditStatus: '',
        selectedType: null,
        selectedId: null,
        selectedRoomDetail: null,
        selectedUserDetail: null,
        drawerFeedback: null,
        modFeedback: null,
        roomCloseReason: '',
        roomCloseIdem: generateIdempotencyKey('close'),
        userActionReason: '',
        userActionIdem: generateIdempotencyKey('user'),
        userActionDurationSec: 300,
        sanctionSubject: '',
        sanctionKind: 'mute',
        sanctionDurationSec: 300,
        sanctionConfirmPermanent: false,
        sanctionReason: '',
        sanctionIdem: generateIdempotencyKey('sanction'),
        dlqReplayMax: 25,
        dlqReason: '',
        dlqIdem: generateIdempotencyKey('dlq'),
        revokeReason: '',
    },
    v3: {
        operations: [],
        activeOperationId: '',
        allowlistedTargets: [],
        drawerKind: null,
        selectedOperationDetail: null,
        restartConfirmTarget: null,
        restartConfirmInput: '',
        restartReason: '',
        restartIdem: generateIdempotencyKey('restart'),
        opFeedback: null,
        configData: null,
        draftValues: null,
        configConfirmInput: '',
        configReason: '',
        configIdem: generateIdempotencyKey('cfg'),
        rollbackTargetRev: null,
        rollbackConfirmInput: '',
        rollbackReason: '',
        rollbackIdem: generateIdempotencyKey('rb'),
        cfgFeedback: null,
        drawerFeedback: null,
        opWatchTimerId: null,
    },
};

function currentEffectiveAgeMs() {
    if (!state.hasSynced) {
        return null;
    }
    const elapsed = Math.max(0, window.performance.now() - state.lastSyncPerfMs);
    return state.baseObservedAgeMs + elapsed;
}

function isObservabilityEnabled() {
    return Boolean(state.overview && state.overview.meta && state.overview.meta.observability_enabled);
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

function openCustomDrawer(title, subtitle, rendererFn) {
    const drawer = document.getElementById('inspector-drawer');
    const titleEl = document.getElementById('drawer-title');
    const subtitleEl = document.getElementById('drawer-subtitle');
    const bodyEl = document.getElementById('drawer-body');
    if (!drawer || !bodyEl) {
        return;
    }
    if (titleEl) {
        titleEl.textContent = title;
    }
    if (subtitleEl) {
        subtitleEl.textContent = subtitle;
    }
    drawer.classList.remove('hidden');
    rendererFn(bodyEl);
}

function renderActiveDrawer() {
    const meta = (state.overview && state.overview.meta) || {};
    if (state.selectedServiceId) {
        renderInspectorDrawer(findSelectedService(), isObservabilityEnabled());
        return;
    }
    if (state.v2.selectedType === 'room' && state.v2.selectedRoomDetail) {
        openCustomDrawer(
            `Room: ${state.v2.selectedRoomDetail.name}`,
            `${state.v2.selectedRoomDetail.room_id} · chat_shared_state`,
            (bodyEl) => renderRoomDrawer(bodyEl, state.v2.selectedRoomDetail, meta, state.v2, v2Handlers),
        );
        return;
    }
    if (state.v2.selectedType === 'user' && state.v2.selectedUserDetail) {
        openCustomDrawer(
            `User: ${state.v2.selectedUserDetail.subject}`,
            `${state.v2.selectedUserDetail.user_id || 'transient'} · Postgres vs Redis`,
            (bodyEl) => renderUserDrawer(bodyEl, state.v2.selectedUserDetail, meta, state.v2, v2Handlers),
        );
        return;
    }
    if (state.v3.drawerKind === 'restart_confirm' && state.v3.restartConfirmTarget) {
        openCustomDrawer(
            `Restart: ${state.v3.restartConfirmTarget.service_id}`,
            '2-Step Blast Radius Confirmation',
            (bodyEl) =>
                renderRestartConfirmDrawer(bodyEl, state.v3.restartConfirmTarget, meta, state.v3, v3Handlers),
        );
        return;
    }
    if (state.v3.drawerKind === 'op_stepper' && state.v3.selectedOperationDetail) {
        openCustomDrawer(
            `Operation: ${state.v3.selectedOperationDetail.target_service || ''}`,
            state.v3.selectedOperationDetail.operation_id || '',
            (bodyEl) => renderOperationStepperDrawer(bodyEl, state.v3.selectedOperationDetail, v3Handlers),
        );
        return;
    }
    if (state.v3.drawerKind === 'rollback_confirm' && state.v3.rollbackTargetRev) {
        openCustomDrawer(
            `Rollback to #${state.v3.rollbackTargetRev.revision}`,
            'Schema Config Revision Rollback',
            (bodyEl) => renderRollbackDrawer(bodyEl, state.v3.rollbackTargetRev, meta, state.v3, v3Handlers),
        );
        return;
    }
    renderInspectorDrawer(null, isObservabilityEnabled());
}

function renderOverviewSection() {
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
}

function renderActiveViewOnly() {
    const viewIds = ['overview', 'rooms', 'users', 'moderation', 'audit', 'operations', 'config'];
    for (const id of viewIds) {
        const el = document.getElementById(`view-${id}`);
        if (el) {
            el.classList.toggle('hidden', state.activeView !== id);
        }
    }
    document.querySelectorAll('.nav-item[data-view]').forEach((btn) => {
        btn.classList.toggle('nav-item--active', btn.dataset.view === state.activeView);
    });

    const meta = (state.overview && state.overview.meta) || {};
    if (state.activeView === 'rooms') {
        renderRoomsView(document.getElementById('view-rooms'), state.v2, v2Handlers);
    } else if (state.activeView === 'users') {
        renderUsersView(document.getElementById('view-users'), state.v2, v2Handlers);
    } else if (state.activeView === 'moderation') {
        renderModerationView(document.getElementById('view-moderation'), meta, state.v2, v2Handlers);
    } else if (state.activeView === 'audit') {
        renderAuditView(document.getElementById('view-audit'), state.v2, v2Handlers);
    } else if (state.activeView === 'operations') {
        renderOperationsView(document.getElementById('view-operations'), meta, state.v3, v3Handlers);
    } else if (state.activeView === 'config') {
        renderConfigView(document.getElementById('view-config'), meta, state.v3, v3Handlers);
    }
    renderActiveDrawer();
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

function handleApiError(err, fromUserAction) {
    if (err instanceof ApiError && err.status === 401) {
        showErrorBanner(`인증 필요 (401): ${err.message}`, true);
        if (fromUserAction || !getAuthToken()) {
            openAuthModal();
        }
    } else if (err instanceof ApiError && err.status === 503 && err.code === 'auth_unconfigured') {
        showErrorBanner(`서버 토큰 미설정 (503): ${err.message}`, false);
    } else {
        showErrorBanner(`API 오류: ${err && err.message ? err.message : 'unknown error'}`, false);
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
    state.v2.selectedType = null;
    state.v2.selectedId = null;
    state.v2.selectedRoomDetail = null;
    state.v2.selectedUserDetail = null;
    state.selectedServiceId = serviceId;
    state.selectedDetail = null;
    renderOverviewSection();
    renderActiveDrawer();

    try {
        const res = await fetchServiceDetail(serviceId);
        if (state.selectedServiceId === serviceId && res && res.service) {
            state.selectedDetail = res.service;
            renderActiveDrawer();
        }
    } catch (_err) {
    }
}

function handleSelectFilter(filterKey) {
    state.activeStatusFilter = filterKey;
    renderOverviewSection();
}

function handleCloseDrawer() {
    state.selectedServiceId = null;
    state.selectedDetail = null;
    state.v2.selectedType = null;
    state.v2.selectedId = null;
    state.v2.selectedRoomDetail = null;
    state.v2.selectedUserDetail = null;
    state.v2.drawerFeedback = null;
    state.v3.drawerKind = null;
    state.v3.restartConfirmTarget = null;
    state.v3.selectedOperationDetail = null;
    state.v3.rollbackTargetRev = null;
    state.v3.drawerFeedback = null;
    if (state.v3.opWatchTimerId !== null) {
        window.clearTimeout(state.v3.opWatchTimerId);
        state.v3.opWatchTimerId = null;
    }
    renderOverviewSection();
    renderActiveViewOnly();
}

async function loadRoomsViewData() {
    try {
        const res = await fetchChatRooms({
            query: state.v2.roomQuery,
            includeClosed: state.v2.includeClosedRooms,
        });
        state.v2.rooms = Array.isArray(res && res.rooms) ? res.rooms : [];
        hideErrorBanner();
        renderActiveViewOnly();
    } catch (err) {
        handleApiError(err, true);
    }
}

async function loadUsersViewData() {
    try {
        const res = await fetchChatUsers({ query: state.v2.userQuery });
        state.v2.users = Array.isArray(res && res.users) ? res.users : [];
        hideErrorBanner();
        renderActiveViewOnly();
    } catch (err) {
        handleApiError(err, true);
    }
}

async function loadModerationViewData() {
    try {
        const [sancRes, dlqRes] = await Promise.all([
            fetchSanctions({
                activeOnly: state.v2.sanctionActiveOnly,
                subject: state.v2.sanctionFilterSubject,
            }),
            fetchWbDlq({ limit: 25 }),
        ]);
        state.v2.sanctions = Array.isArray(sancRes && sancRes.sanctions) ? sancRes.sanctions : [];
        state.v2.dlq = dlqRes || null;
        hideErrorBanner();
        renderActiveViewOnly();
    } catch (err) {
        handleApiError(err, true);
    }
}

async function loadAuditViewData() {
    try {
        const res = await fetchAuditLogs({
            target: state.v2.auditTarget,
            action: state.v2.auditAction,
            status: state.v2.auditStatus,
        });
        state.v2.auditLogs = Array.isArray(res && res.audit_logs) ? res.audit_logs : [];
        hideErrorBanner();
        renderActiveViewOnly();
    } catch (err) {
        handleApiError(err, true);
    }
}

async function loadOperationsViewData() {
    try {
        const res = await fetchOperations({ limit: 50 });
        state.v3.operations = Array.isArray(res && res.operations) ? res.operations : [];
        state.v3.activeOperationId = res && typeof res.active_operation_id === 'string' ? res.active_operation_id : '';
        state.v3.allowlistedTargets = Array.isArray(res && res.allowlisted_targets) ? res.allowlisted_targets : [];
        hideErrorBanner();
        renderActiveViewOnly();
        scheduleOperationWatch();
    } catch (err) {
        handleApiError(err, true);
    }
}

async function loadConfigViewData(resetDraft = false) {
    try {
        const res = await fetchConfigOverview();
        state.v3.configData = res || null;
        if (resetDraft || !state.v3.draftValues) {
            state.v3.draftValues = { ...(res && res.current_values ? res.current_values : {}) };
        }
        hideErrorBanner();
        renderActiveViewOnly();
    } catch (err) {
        handleApiError(err, true);
    }
}

function scheduleOperationWatch() {
    if (state.v3.opWatchTimerId !== null) {
        window.clearTimeout(state.v3.opWatchTimerId);
        state.v3.opWatchTimerId = null;
    }
    const selOp = state.v3.selectedOperationDetail;
    const selActive = selOp && ['queued', 'running', 'verifying'].includes(selOp.status);
    if (!selActive && !state.v3.activeOperationId) {
        return;
    }
    if (state.activeView !== 'operations' && !(state.v3.drawerKind === 'op_stepper' && selActive)) {
        return;
    }
    state.v3.opWatchTimerId = window.setTimeout(async () => {
        try {
            if (state.v3.drawerKind === 'op_stepper' && state.v3.selectedOperationDetail) {
                const detailRes = await fetchOperationDetail(state.v3.selectedOperationDetail.operation_id);
                if (detailRes && detailRes.operation) {
                    state.v3.selectedOperationDetail = detailRes.operation;
                }
            }
            if (state.activeView === 'operations') {
                await loadOperationsViewData();
            } else {
                renderActiveDrawer();
                scheduleOperationWatch();
            }
        } catch (_err) {
        }
    }, 1000);
}

async function refreshActiveView() {
    if (state.activeView === 'rooms') {
        await loadRoomsViewData();
    } else if (state.activeView === 'users') {
        await loadUsersViewData();
    } else if (state.activeView === 'moderation') {
        await loadModerationViewData();
    } else if (state.activeView === 'audit') {
        await loadAuditViewData();
    } else if (state.activeView === 'operations') {
        await loadOperationsViewData();
    } else if (state.activeView === 'config') {
        await loadConfigViewData(false);
    }
}

function regenerateKey(field, prefix) {
    const next = generateIdempotencyKey(prefix);
    state.v2[field] = next;
    return next;
}

const v2Handlers = {
    onRoomQueryChange(q) {
        state.v2.roomQuery = q;
    },
    onToggleIncludeClosed(checked) {
        state.v2.includeClosedRooms = checked;
        loadRoomsViewData();
    },
    onRefreshRooms() {
        loadRoomsViewData();
    },
    async onSelectRoom(roomName) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
        state.v2.selectedType = 'room';
        state.v2.selectedId = roomName;
        state.v2.selectedUserDetail = null;
        state.v2.drawerFeedback = null;
        try {
            const res = await fetchChatRoomDetail(roomName);
            state.v2.selectedRoomDetail = res && res.room ? res.room : null;
            renderActiveViewOnly();
        } catch (err) {
            handleApiError(err, true);
        }
    },
    async onCloseRoom(roomName) {
        try {
            const res = await closeChatRoom(roomName, {
                reason: state.v2.roomCloseReason,
                idempotencyKey: state.v2.roomCloseIdem,
            });
            state.v2.drawerFeedback = {
                ok: true,
                message: `방 종료 완료 (${res.room})${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            regenerateKey('roomCloseIdem', 'close');
            const detailRes = await fetchChatRoomDetail(roomName);
            state.v2.selectedRoomDetail = detailRes && detailRes.room ? detailRes.room : null;
            await loadRoomsViewData();
        } catch (err) {
            state.v2.drawerFeedback = { ok: false, message: err.message };
            renderActiveDrawer();
        }
    },
    onUserQueryChange(q) {
        state.v2.userQuery = q;
    },
    onRefreshUsers() {
        loadUsersViewData();
    },
    async onSelectUser(subject) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
        state.v2.selectedType = 'user';
        state.v2.selectedId = subject;
        state.v2.selectedRoomDetail = null;
        state.v2.drawerFeedback = null;
        try {
            const res = await fetchChatUserDetail(subject);
            state.v2.selectedUserDetail = res || null;
            renderActiveViewOnly();
        } catch (err) {
            handleApiError(err, true);
        }
    },
    async onQuickUserSanction(subject, kind) {
        try {
            const res = await createSanction({
                subject,
                kind,
                durationSec: state.v2.userActionDurationSec,
                confirmPermanent: false,
                reason: state.v2.userActionReason,
                idempotencyKey: state.v2.userActionIdem,
            });
            state.v2.drawerFeedback = {
                ok: true,
                message: `${kind.toUpperCase()} 적용 완료 (${res.subject})${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            regenerateKey('userActionIdem', 'user');
            state.v2.selectedUserDetail = await fetchChatUserDetail(subject);
            await loadUsersViewData();
        } catch (err) {
            state.v2.drawerFeedback = { ok: false, message: err.message };
            renderActiveDrawer();
        }
    },
    async onKickUser(subject) {
        try {
            const res = await kickChatUser(subject, {
                reason: state.v2.userActionReason,
                idempotencyKey: state.v2.userActionIdem,
            });
            state.v2.drawerFeedback = {
                ok: true,
                message: `강제 연결 종료 완료 (${res.subject})${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            regenerateKey('userActionIdem', 'user');
            state.v2.selectedUserDetail = await fetchChatUserDetail(subject);
            await loadUsersViewData();
        } catch (err) {
            state.v2.drawerFeedback = { ok: false, message: err.message };
            renderActiveDrawer();
        }
    },
    onUpdateDraft(field, value, rerender = false) {
        state.v2[field] = value;
        if (rerender) {
            renderActiveViewOnly();
        }
    },
    onRegenerateIdem(field, prefix) {
        return regenerateKey(field, prefix);
    },
    async onCreateSanction() {
        try {
            const res = await createSanction({
                subject: state.v2.sanctionSubject,
                kind: state.v2.sanctionKind,
                durationSec: state.v2.sanctionDurationSec,
                confirmPermanent: state.v2.sanctionConfirmPermanent,
                reason: state.v2.sanctionReason,
                idempotencyKey: state.v2.sanctionIdem,
            });
            state.v2.modFeedback = {
                ok: true,
                message: `제재 적용 완료: ${res.subject} (${res.kind})${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            regenerateKey('sanctionIdem', 'sanction');
            await loadModerationViewData();
        } catch (err) {
            state.v2.modFeedback = { ok: false, message: err.message };
            renderActiveViewOnly();
        }
    },
    async onRevokeSanction(sanctionId) {
        try {
            const key = generateIdempotencyKey('revoke');
            const res = await revokeSanction(sanctionId, {
                reason: state.v2.revokeReason,
                idempotencyKey: key,
            });
            state.v2.modFeedback = {
                ok: true,
                message: `제재 해제 완료: ${res.subject} (${res.kind})`,
            };
            await loadModerationViewData();
        } catch (err) {
            state.v2.modFeedback = { ok: false, message: err.message };
            renderActiveViewOnly();
        }
    },
    async onReplayDlq() {
        try {
            const res = await replayWbDlq({
                maxItems: state.v2.dlqReplayMax,
                reason: state.v2.dlqReason,
                idempotencyKey: state.v2.dlqIdem,
            });
            state.v2.modFeedback = {
                ok: true,
                message: `DLQ 재처리 완료: ok=${res.replayed_ok}, retried=${res.retried_dlq}, dead=${res.moved_dead}${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            regenerateKey('dlqIdem', 'dlq');
            await loadModerationViewData();
        } catch (err) {
            state.v2.modFeedback = { ok: false, message: err.message };
            renderActiveViewOnly();
        }
    },
    onSanctionFilterChange(q) {
        state.v2.sanctionFilterSubject = q;
    },
    onToggleSanctionActiveOnly(checked) {
        state.v2.sanctionActiveOnly = checked;
        loadModerationViewData();
    },
    onRefreshModeration() {
        loadModerationViewData();
    },
    onAuditFilterChange(field, value, refreshNow = false) {
        state.v2[field] = value;
        if (refreshNow) {
            loadAuditViewData();
        }
    },
    onRefreshAudit() {
        loadAuditViewData();
    },
};

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
        renderOverviewSection();
        if (state.selectedServiceId) {
            renderActiveDrawer();
        }
        if (fromUserAction && state.activeView !== 'overview') {
            await refreshActiveView();
        }
    } catch (err) {
        handleApiError(err, fromUserAction);
    } finally {
        state.inFlight = false;
        scheduleNextPoll();
    }
}

function regenerateV3Key(field, prefix) {
    const next = generateIdempotencyKey(prefix);
    state.v3[field] = next;
    return next;
}

const v3Handlers = {
    onRegenerateV3Key(field, prefix) {
        return regenerateV3Key(field, prefix);
    },
    onRefreshOperations() {
        loadOperationsViewData();
    },
    onOpenRestartConfirm(targetObj) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
        state.v2.selectedType = null;
        state.v3.drawerKind = 'restart_confirm';
        state.v3.restartConfirmTarget = targetObj;
        state.v3.restartConfirmInput = '';
        state.v3.drawerFeedback = null;
        renderActiveDrawer();
    },
    async onSubmitRestart(serviceId) {
        try {
            const res = await createRestartOperation({
                targetService: serviceId,
                confirmationTarget: state.v3.restartConfirmInput,
                reason: state.v3.restartReason,
                idempotencyKey: state.v3.restartIdem,
            });
            regenerateV3Key('restartIdem', 'restart');
            state.v3.restartConfirmInput = '';
            state.v3.opFeedback = {
                ok: true,
                message: `재시작 작업 접수(202): ${serviceId}${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            if (res && res.operation) {
                state.v3.drawerKind = 'op_stepper';
                state.v3.selectedOperationDetail = res.operation;
            }
            await loadOperationsViewData();
        } catch (err) {
            state.v3.drawerFeedback = { ok: false, message: err.message };
            renderActiveDrawer();
        }
    },
    async onSelectOperation(opId) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
        state.v2.selectedType = null;
        state.v3.drawerFeedback = null;
        try {
            const res = await fetchOperationDetail(opId);
            if (res && res.operation) {
                state.v3.drawerKind = 'op_stepper';
                state.v3.selectedOperationDetail = res.operation;
                renderActiveViewOnly();
                scheduleOperationWatch();
            }
        } catch (err) {
            handleApiError(err, true);
        }
    },
    onUpdateConfigDraft(key, val) {
        if (!state.v3.draftValues) {
            state.v3.draftValues = {};
        }
        state.v3.draftValues[key] = val;
        renderActiveViewOnly();
    },
    onResetConfigDraft() {
        if (state.v3.configData && state.v3.configData.current_values) {
            state.v3.draftValues = { ...state.v3.configData.current_values };
        }
        state.v3.configConfirmInput = '';
        state.v3.cfgFeedback = null;
        renderActiveViewOnly();
    },
    async onApplyConfig() {
        try {
            const cfg = state.v3.configData;
            const res = await applyConfigChanges({
                expectedRevision: cfg ? cfg.current_revision : 0,
                values: state.v3.draftValues || {},
                confirmationTarget: state.v3.configConfirmInput,
                reason: state.v3.configReason,
                idempotencyKey: state.v3.configIdem,
            });
            regenerateV3Key('configIdem', 'cfg');
            state.v3.configConfirmInput = '';
            state.v3.cfgFeedback = {
                ok: true,
                message: `설정 리비전 #${res.revision} 적용 접수 (${res.apply_mode})${res.idempotent_replay ? ' [idempotent replay]' : ''}`,
            };
            if (res && res.operation) {
                state.v3.drawerKind = 'op_stepper';
                state.v3.selectedOperationDetail = res.operation;
            }
            await refreshOverview(false);
            await loadConfigViewData(true);
            scheduleOperationWatch();
        } catch (err) {
            state.v3.cfgFeedback = { ok: false, message: err.message };
            renderActiveViewOnly();
        }
    },
    onOpenRollbackDrawer(revObj) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
        state.v2.selectedType = null;
        state.v3.drawerKind = 'rollback_confirm';
        state.v3.rollbackTargetRev = revObj;
        state.v3.rollbackConfirmInput = '';
        state.v3.drawerFeedback = null;
        renderActiveDrawer();
    },
    async onSubmitRollback(targetRevision) {
        try {
            const res = await rollbackConfigRevision({
                targetRevision,
                confirmationTarget: state.v3.rollbackConfirmInput,
                reason: state.v3.rollbackReason,
                idempotencyKey: state.v3.rollbackIdem,
            });
            regenerateV3Key('rollbackIdem', 'rb');
            state.v3.rollbackConfirmInput = '';
            state.v3.cfgFeedback = {
                ok: true,
                message: `롤백 리비전 #${res.revision} 생성 (대상 #${targetRevision}, mode=${res.apply_mode})`,
            };
            if (res && res.operation) {
                state.v3.drawerKind = 'op_stepper';
                state.v3.selectedOperationDetail = res.operation;
            }
            await refreshOverview(false);
            await loadConfigViewData(true);
            scheduleOperationWatch();
        } catch (err) {
            state.v3.drawerFeedback = { ok: false, message: err.message };
            renderActiveDrawer();
        }
    },
};

function switchView(viewName) {
    state.activeView = viewName;
    if (viewName !== 'overview' && state.selectedServiceId) {
        state.selectedServiceId = null;
        state.selectedDetail = null;
    } else if (viewName === 'overview') {
        state.v2.selectedType = null;
        state.v2.selectedId = null;
        state.v2.selectedRoomDetail = null;
        state.v2.selectedUserDetail = null;
        state.v3.drawerKind = null;
        state.v3.restartConfirmTarget = null;
        state.v3.selectedOperationDetail = null;
        state.v3.rollbackTargetRev = null;
    }
    renderActiveViewOnly();
    refreshActiveView();
}

function bindEvents() {
    document.querySelectorAll('.nav-item[data-view]').forEach((btn) => {
        btn.addEventListener('click', () => switchView(btn.dataset.view));
    });

    const searchEl = document.getElementById('search-input');
    if (searchEl) {
        searchEl.addEventListener('input', (e) => {
            state.searchQuery = e.target.value || '';
            renderOverviewSection();
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
            if (state.selectedServiceId || state.v2.selectedType || state.v3.drawerKind) {
                handleCloseDrawer();
            }
        }
    });

    window.setInterval(() => {
        const ageMs = currentEffectiveAgeMs();
        if (ageMs !== null) {
            renderObservedAge(ageMs, state.staleTtlMs);
        }
    }, 1000);
}

bindEvents();
refreshOverview(false);
