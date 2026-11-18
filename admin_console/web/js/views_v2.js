import { formatAgeMs, formatTimestampMs } from '/js/render.js';

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) {
        node.className = className;
    }
    if (text !== undefined && text !== null) {
        node.textContent = String(text);
    }
    return node;
}

function makePill(className, text) {
    return el('span', `pill ${className}`, text);
}

function makeMutationLockMessage(meta) {
    if (!meta) {
        return '권한 정보를 불러오는 중입니다.';
    }
    if (meta.read_only) {
        return 'ADMIN_READ_ONLY=1 active — 쓰기 제어 작업이 잠겨 있습니다.';
    }
    if (meta.actor_role === 'viewer') {
        return `Requires operator or admin role (현재 역할: ${meta.actor_role || 'viewer'})`;
    }
    return '';
}

function createReadOnlyCallout(meta) {
    const msg = makeMutationLockMessage(meta);
    if (!msg) {
        return null;
    }
    return el('div', 'callout-banner callout-banner--warn', msg);
}

function createFeedbackBanner(feedback) {
    if (!feedback || !feedback.message) {
        return null;
    }
    const cls = feedback.ok ? 'callout-banner callout-banner--ok' : 'callout-banner callout-banner--error';
    return el('div', cls, feedback.message);
}

function makeTruthRow(key, value, mono = false) {
    const row = el('div', 'truth-row');
    row.append(
        el('span', 'truth-row__k', key),
        el('span', mono ? 'truth-row__v mono' : 'truth-row__v num', value),
    );
    return row;
}

function makeLabeledField(labelText, control) {
    const wrap = el('div', 'form-field');
    wrap.append(el('label', 'form-label', labelText), control);
    return wrap;
}

function createIdempotencyField(value, onChange, onRegenerate) {
    const row = el('div', 'idem-row');
    const input = el('input', 'search-input mono');
    input.type = 'text';
    input.value = value || '';
    input.placeholder = 'idempotency_key';
    input.addEventListener('input', (e) => onChange(e.target.value));

    const regenBtn = el('button', 'btn btn--sm', 'New Key');
    regenBtn.type = 'button';
    regenBtn.addEventListener('click', () => {
        const next = onRegenerate();
        input.value = next;
    });

    row.append(input, regenBtn);
    return row;
}

export function renderRoomsView(container, state, handlers) {
    if (!container) {
        return;
    }

    const fragment = document.createDocumentFragment();
    fragment.appendChild(
        el(
            'div',
            'callout-banner',
            '프라이버시 경계: Chat Rooms는 방 메타데이터·참여 인원·Watermark만 표시하며 일반 채팅 메시지 본문 평문 뷰어를 제공하지 않습니다.',
        ),
    );

    const card = el('section', 'panel-card');
    const header = el('div', 'panel-card__header');
    header.append(
        el('h2', 'panel-card__title', 'Chat Rooms (v2)'),
        el('span', 'panel-card__hint num', `${state.rooms.length} rooms`),
    );
    card.appendChild(header);

    const toolbar = el('div', 'filter-toolbar');
    const leftRow = el('div', 'chip-row');

    const searchInput = el('input', 'search-input');
    searchInput.type = 'search';
    searchInput.placeholder = 'Filter room name...';
    searchInput.value = state.roomQuery || '';
    searchInput.addEventListener('input', (e) => handlers.onRoomQueryChange(e.target.value));
    searchInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            handlers.onRefreshRooms();
        }
    });

    const chkLabel = el('label', 'checkbox-label');
    const chk = document.createElement('input');
    chk.type = 'checkbox';
    chk.checked = Boolean(state.includeClosedRooms);
    chk.addEventListener('change', () => handlers.onToggleIncludeClosed(chk.checked));
    chkLabel.append(chk, document.createTextNode('종료된 방 포함'));

    leftRow.append(searchInput, chkLabel);

    const refreshBtn = el('button', 'btn', '조회 / Refresh');
    refreshBtn.type = 'button';
    refreshBtn.addEventListener('click', handlers.onRefreshRooms);

    toolbar.append(leftRow, refreshBtn);
    card.appendChild(toolbar);

    const wrap = el('div', 'table-wrap');
    const table = el('table', 'inventory-table');
    const thead = document.createElement('thead');
    const htr = document.createElement('tr');
    for (const col of ['Room', 'State', 'Lock', 'Owner', 'Online', 'DB Members', 'Watermark', 'Room ID']) {
        htr.appendChild(el('th', '', col));
    }
    thead.appendChild(htr);

    const tbody = document.createElement('tbody');
    for (const rm of state.rooms) {
        const tr = el(
            'tr',
            state.selectedType === 'room' && state.selectedId === rm.name
                ? 'inventory-row inventory-row--selected'
                : 'inventory-row',
        );
        tr.tabIndex = 0;
        tr.addEventListener('click', () => handlers.onSelectRoom(rm.name));
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                handlers.onSelectRoom(rm.name);
            }
        });

        const tdStatus = document.createElement('td');
        tdStatus.appendChild(
            rm.is_active ? makePill('pill--ok', 'Active') : makePill('pill--danger', 'Closed'),
        );

        const tdLock = document.createElement('td');
        tdLock.appendChild(
            rm.is_locked ? makePill('pill--warn', 'Locked') : makePill('', 'Public'),
        );

        tr.append(
            el('td', 'mono', rm.name || ''),
            tdStatus,
            tdLock,
            el('td', 'mono', rm.owner || '—'),
            el('td', 'num', String(rm.online_count ?? 0)),
            el('td', 'num', String(rm.db_member_count ?? 0)),
            el('td', 'num', String(rm.watermark ?? 0)),
            el('td', 'mono', rm.room_id || ''),
        );
        tbody.appendChild(tr);
    }

    table.append(thead, tbody);
    wrap.appendChild(table);
    card.appendChild(wrap);

    if (state.rooms.length === 0) {
        card.appendChild(el('div', 'empty-box', '조건에 해당하는 채팅방이 없습니다.'));
    }

    fragment.appendChild(card);
    container.replaceChildren(fragment);
}

export function renderRoomDrawer(bodyEl, detail, meta, state, handlers) {
    if (!bodyEl || !detail) {
        return;
    }
    const fragment = document.createDocumentFragment();
    const badges = el('div', 'chip-row');
    badges.append(
        detail.is_active ? makePill('pill--ok', 'Active') : makePill('pill--danger', 'Closed'),
        detail.is_locked ? makePill('pill--warn', 'Locked') : makePill('', 'Public'),
    );
    fragment.appendChild(badges);

    const card = el('div', 'truth-card');
    card.appendChild(el('div', 'truth-card__title', 'Room Authority & Watermark'));
    const list = el('div', 'truth-list');
    list.append(
        makeTruthRow('Room ID', detail.room_id || '—', true),
        makeTruthRow('Owner', detail.owner || '(none)', true),
        makeTruthRow('Watermark (Max Msg ID)', String(detail.watermark ?? 0)),
        makeTruthRow('Online Members (Redis)', Array.isArray(detail.online_members) ? detail.online_members.join(', ') || '—' : '—', true),
        makeTruthRow('Shared Authority Members', Array.isArray(detail.shared_members) ? detail.shared_members.join(', ') || '—' : '—', true),
        makeTruthRow('Created At', formatTimestampMs(detail.created_at_unix_ms), true),
    );
    card.appendChild(list);
    fragment.appendChild(card);

    fragment.appendChild(
        el('div', 'callout-banner', '대화 본문 평문은 개인정보 보호를 위해 어드민 API/UI에서 반환하지 않습니다.'),
    );

    const actionCard = el('div', 'truth-card');
    actionCard.appendChild(el('div', 'truth-card__title', 'Force Close Room'));

    if (String(detail.name || '').toLowerCase() === 'lobby') {
        actionCard.appendChild(el('div', 'callout-banner callout-banner--warn', '기본 방 lobby는 강제 종료할 수 없습니다.'));
    } else if (!detail.is_active) {
        actionCard.appendChild(el('div', 'callout-banner', '이미 종료된 방입니다.'));
    } else {
        const lockCallout = createReadOnlyCallout(meta);
        if (lockCallout) {
            actionCard.appendChild(lockCallout);
        }
        const fb = createFeedbackBanner(state.drawerFeedback);
        if (fb) {
            actionCard.appendChild(fb);
        }

        const reasonInput = el('input', 'search-input');
        reasonInput.type = 'text';
        reasonInput.placeholder = '필수: 감사 사유 (reason)';
        reasonInput.value = state.roomCloseReason || '';
        reasonInput.addEventListener('input', (e) => handlers.onUpdateDraft('roomCloseReason', e.target.value));

        const idemControl = createIdempotencyField(
            state.roomCloseIdem,
            (val) => handlers.onUpdateDraft('roomCloseIdem', val),
            () => handlers.onRegenerateIdem('roomCloseIdem', 'close'),
        );

        const closeBtn = el('button', 'btn btn--danger', 'Close Room & Notify Lobby Fallback');
        closeBtn.type = 'button';
        closeBtn.disabled = !meta || !meta.mutating_allowed;
        if (closeBtn.disabled) {
            closeBtn.title = makeMutationLockMessage(meta);
        }
        closeBtn.addEventListener('click', () => handlers.onCloseRoom(detail.name));

        actionCard.append(
            makeLabeledField('Audit Reason (Required)', reasonInput),
            makeLabeledField('Idempotency Key', idemControl),
            closeBtn,
        );
    }

    fragment.appendChild(actionCard);
    bodyEl.replaceChildren(fragment);
}

export function renderUsersView(container, state, handlers) {
    if (!container) {
        return;
    }

    const fragment = document.createDocumentFragment();
    fragment.appendChild(
        el(
            'div',
            'callout-banner',
            '신원 조회는 표시명(display_name)이 아닌 정규 소문자 ASCII subject(principal_id) 기준으로 동작합니다. 토큰 원문은 절대 표시하지 않습니다.',
        ),
    );

    const card = el('section', 'panel-card');
    const header = el('div', 'panel-card__header');
    header.append(
        el('h2', 'panel-card__title', 'Users & Sessions (v2)'),
        el('span', 'panel-card__hint num', `${state.users.length} users`),
    );
    card.appendChild(header);

    const toolbar = el('div', 'filter-toolbar');
    const searchInput = el('input', 'search-input');
    searchInput.type = 'search';
    searchInput.placeholder = '소문자 ASCII subject (principal_id) 검색...';
    searchInput.value = state.userQuery || '';
    searchInput.addEventListener('input', (e) => handlers.onUserQueryChange(e.target.value));
    searchInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            handlers.onRefreshUsers();
        }
    });

    const refreshBtn = el('button', 'btn', '조회 / Refresh');
    refreshBtn.type = 'button';
    refreshBtn.addEventListener('click', handlers.onRefreshUsers);

    toolbar.append(searchInput, refreshBtn);
    card.appendChild(toolbar);

    const wrap = el('div', 'table-wrap');
    const table = el('table', 'inventory-table');
    const thead = document.createElement('thead');
    const htr = document.createElement('tr');
    for (const col of ['Subject', 'Postgres Lease', 'Redis Presence', 'Sanctions', 'Logical Session ID', 'User ID']) {
        htr.appendChild(el('th', '', col));
    }
    thead.appendChild(htr);

    const tbody = document.createElement('tbody');
    for (const u of state.users) {
        const tr = el(
            'tr',
            state.selectedType === 'user' && state.selectedId === u.subject
                ? 'inventory-row inventory-row--selected'
                : 'inventory-row',
        );
        tr.tabIndex = 0;
        tr.addEventListener('click', () => handlers.onSelectUser(u.subject));
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                handlers.onSelectUser(u.subject);
            }
        });

        const tdLease = el('td', 'num');
        tdLease.appendChild(
            u.lease_active
                ? makePill('pill--ok', `Active (${formatAgeMs(u.lease_ttl_ms)})`)
                : makePill('', 'Inactive'),
        );

        const tdPresence = el('td', 'num');
        if (u.presence_exists) {
            const label = u.presence_ttl_ms >= 0 ? `TTL ${formatAgeMs(u.presence_ttl_ms)}` : 'No Expiry';
            tdPresence.appendChild(makePill('pill--ok', label));
        } else {
            tdPresence.appendChild(makePill('', 'Absent'));
        }

        const tdSanc = document.createElement('td');
        if (u.banned) {
            tdSanc.appendChild(makePill('pill--danger', 'Banned'));
        } else if (u.muted) {
            tdSanc.appendChild(makePill('pill--warn', 'Muted'));
        } else {
            tdSanc.textContent = 'Clean';
        }

        tr.append(
            el('td', 'mono', u.subject || ''),
            tdLease,
            tdPresence,
            tdSanc,
            el('td', 'mono', u.logical_session_id || '—'),
            el('td', 'mono', u.user_id || '—'),
        );
        tbody.appendChild(tr);
    }

    table.append(thead, tbody);
    wrap.appendChild(table);
    card.appendChild(wrap);

    if (state.users.length === 0) {
        card.appendChild(el('div', 'empty-box', '검색 조건과 일치하는 사용자가 없습니다.'));
    }

    fragment.appendChild(card);
    container.replaceChildren(fragment);
}

export function renderUserDrawer(bodyEl, detail, meta, state, handlers) {
    if (!bodyEl || !detail) {
        return;
    }

    const fragment = document.createDocumentFragment();
    const alignment = detail.state_alignment || 'unknown';
    const alignRow = el('div', 'chip-row');
    let alignClass = '';
    if (alignment === 'aligned' || alignment === 'offline_aligned') {
        alignClass = 'pill--ok';
    } else if (alignment === 'orphan_redis_presence' || alignment === 'missing_redis_presence') {
        alignClass = 'pill--warn';
    } else {
        alignClass = 'pill--danger';
    }
    alignRow.append(
        makePill(alignClass, `Truth Alignment: ${alignment}`),
        makePill('mono', `ID: ${detail.user_id || 'guest/transient'}`),
    );
    fragment.appendChild(alignRow);

    const pg = detail.postgres_truth || {};
    const rd = detail.redis_state || {};

    const split = el('div', 'truth-split');

    const pgCard = el('div', 'truth-card');
    pgCard.appendChild(el('div', 'truth-card__title', 'Postgres Login Lease'));
    const pgList = el('div', 'truth-list');
    pgList.append(
        makeTruthRow('Lease Active', pg.lease_active ? 'YES' : 'NO', true),
        makeTruthRow('Lease TTL', pg.lease_active ? formatAgeMs(pg.lease_ttl_ms) : '—'),
        makeTruthRow('Logical Session', pg.logical_session_id || '—', true),
        makeTruthRow('Mute Remaining', pg.muted ? formatAgeMs(pg.mute_remaining_ms) : 'None'),
        makeTruthRow('Ban Remaining', pg.banned ? formatAgeMs(pg.ban_remaining_ms) : 'None'),
    );
    pgCard.appendChild(pgList);

    const rdCard = el('div', 'truth-card');
    rdCard.appendChild(el('div', 'truth-card__title', 'Redis Presence TTL'));
    const rdList = el('div', 'truth-list');
    rdList.append(
        makeTruthRow('Redis Connected', rd.connected ? 'YES' : 'NO', true),
        makeTruthRow('Presence Exists', rd.presence_exists ? 'YES' : 'NO', true),
        makeTruthRow('Presence TTL', rd.presence_ttl_ms >= 0 ? formatAgeMs(rd.presence_ttl_ms) : '—'),
        makeTruthRow('Continuity Room', rd.continuity_room || '—', true),
        makeTruthRow('Gateway Backend', rd.gateway_backend || '—', true),
        makeTruthRow('Rooms Sets', Array.isArray(rd.rooms) && rd.rooms.length > 0 ? rd.rooms.join(', ') : '—', true),
    );
    rdCard.appendChild(rdList);

    split.append(pgCard, rdCard);
    fragment.appendChild(split);

    fragment.appendChild(
        el('div', 'callout-banner', detail.secret_policy || '토큰 원문은 절대 노출되지 않습니다.'),
    );

    const actionCard = el('div', 'truth-card');
    actionCard.appendChild(el('div', 'truth-card__title', 'Quick User Control (Kick / Mute / Ban)'));
    const lockCallout = createReadOnlyCallout(meta);
    if (lockCallout) {
        actionCard.appendChild(lockCallout);
    }
    const fb = createFeedbackBanner(state.drawerFeedback);
    if (fb) {
        actionCard.appendChild(fb);
    }

    const reasonInput = el('input', 'search-input');
    reasonInput.type = 'text';
    reasonInput.placeholder = '필수: 감사 사유 (reason)';
    reasonInput.value = state.userActionReason || '';
    reasonInput.addEventListener('input', (e) => handlers.onUpdateDraft('userActionReason', e.target.value));

    const idemControl = createIdempotencyField(
        state.userActionIdem,
        (val) => handlers.onUpdateDraft('userActionIdem', val),
        () => handlers.onRegenerateIdem('userActionIdem', 'user'),
    );

    const presetRow = el('div', 'chip-row');
    for (const [label, sec] of [['5m', 300], ['30m', 1800], ['1h', 3600], ['24h', 86400]]) {
        const chip = el('button', state.userActionDurationSec === sec ? 'chip chip--active num' : 'chip num', label);
        chip.type = 'button';
        chip.addEventListener('click', () => handlers.onUpdateDraft('userActionDurationSec', sec, true));
        presetRow.appendChild(chip);
    }

    const btnRow = el('div', 'drawer-actions');
    const canMutate = Boolean(meta && meta.mutating_allowed);

    const muteBtn = el('button', 'btn', `Mute (${formatAgeMs((state.userActionDurationSec || 300) * 1000)})`);
    muteBtn.type = 'button';
    muteBtn.disabled = !canMutate;
    muteBtn.addEventListener('click', () => handlers.onQuickUserSanction(detail.subject, 'mute'));

    const banBtn = el('button', 'btn btn--danger', `Ban (${formatAgeMs((state.userActionDurationSec || 300) * 1000)})`);
    banBtn.type = 'button';
    banBtn.disabled = !canMutate;
    banBtn.addEventListener('click', () => handlers.onQuickUserSanction(detail.subject, 'ban'));

    const kickBtn = el('button', 'btn btn--danger', 'Global Kick');
    kickBtn.type = 'button';
    kickBtn.disabled = !canMutate;
    kickBtn.addEventListener('click', () => handlers.onKickUser(detail.subject));

    for (const b of [muteBtn, banBtn, kickBtn]) {
        if (!canMutate) {
            b.title = makeMutationLockMessage(meta);
        }
        btnRow.appendChild(b);
    }

    actionCard.append(
        makeLabeledField('Preset Duration', presetRow),
        makeLabeledField('Audit Reason (Required)', reasonInput),
        makeLabeledField('Idempotency Key', idemControl),
        btnRow,
    );
    fragment.appendChild(actionCard);

    bodyEl.replaceChildren(fragment);
}

export function renderModerationView(container, meta, state, handlers) {
    if (!container) {
        return;
    }

    const canMutate = Boolean(meta && meta.mutating_allowed);
    const fragment = document.createDocumentFragment();

    const lockCallout = createReadOnlyCallout(meta);
    if (lockCallout) {
        fragment.appendChild(lockCallout);
    }
    const modFb = createFeedbackBanner(state.modFeedback);
    if (modFb) {
        fragment.appendChild(modFb);
    }

    const topGrid = el('div', 'two-col-grid');

    // Card 1: Grant Sanction
    const sancCard = el('section', 'panel-card');
    const sancHdr = el('div', 'panel-card__header');
    sancHdr.append(
        el('h2', 'panel-card__title', 'Grant Sanction (Mute / Ban)'),
        el('span', 'panel-card__hint', '프리셋 우선 · 24h 초과 시 별도 확인 필요'),
    );
    sancCard.appendChild(sancHdr);

    const sancBody = el('div', 'form-panel');
    const grid1 = el('div', 'form-grid');

    const subjInput = el('input', 'search-input mono');
    subjInput.type = 'text';
    subjInput.placeholder = 'target subject';
    subjInput.value = state.sanctionSubject || '';
    subjInput.addEventListener('input', (e) => handlers.onUpdateDraft('sanctionSubject', e.target.value));

    const kindSelect = el('select', 'select-input');
    for (const optVal of ['mute', 'ban']) {
        const opt = document.createElement('option');
        opt.value = optVal;
        opt.textContent = optVal.toUpperCase();
        opt.selected = (state.sanctionKind || 'mute') === optVal;
        kindSelect.appendChild(opt);
    }
    kindSelect.addEventListener('change', (e) => handlers.onUpdateDraft('sanctionKind', e.target.value));

    grid1.append(
        makeLabeledField('Subject (Required)', subjInput),
        makeLabeledField('Kind', kindSelect),
    );
    sancBody.appendChild(grid1);

    const presetRow = el('div', 'chip-row');
    for (const [label, sec] of [['5m', 300], ['30m', 1800], ['1h', 3600], ['24h', 86400]]) {
        const chip = el('button', Number(state.sanctionDurationSec) === sec ? 'chip chip--active num' : 'chip num', label);
        chip.type = 'button';
        chip.addEventListener('click', () => handlers.onUpdateDraft('sanctionDurationSec', sec, true));
        presetRow.appendChild(chip);
    }

    const durInput = el('input', 'search-input num');
    durInput.type = 'number';
    durInput.min = '5';
    durInput.max = '31536000';
    durInput.value = String(state.sanctionDurationSec || 300);
    durInput.addEventListener('input', (e) => handlers.onUpdateDraft('sanctionDurationSec', Number(e.target.value) || 0));

    const permLabel = el('label', 'checkbox-label');
    const permChk = document.createElement('input');
    permChk.type = 'checkbox';
    permChk.checked = Boolean(state.sanctionConfirmPermanent);
    permChk.addEventListener('change', () => handlers.onUpdateDraft('sanctionConfirmPermanent', permChk.checked));
    permLabel.append(permChk, document.createTextNode('24시간 초과 장기/준영구 제재 확인'));

    const reasonInput = el('input', 'search-input');
    reasonInput.type = 'text';
    reasonInput.placeholder = '필수: 제재 감사 사유';
    reasonInput.value = state.sanctionReason || '';
    reasonInput.addEventListener('input', (e) => handlers.onUpdateDraft('sanctionReason', e.target.value));

    const sancIdem = createIdempotencyField(
        state.sanctionIdem,
        (val) => handlers.onUpdateDraft('sanctionIdem', val),
        () => handlers.onRegenerateIdem('sanctionIdem', 'sanction'),
    );

    const submitSancBtn = el('button', 'btn btn--primary', 'Apply Sanction');
    submitSancBtn.type = 'button';
    submitSancBtn.disabled = !canMutate;
    if (!canMutate) {
        submitSancBtn.title = makeMutationLockMessage(meta);
    }
    submitSancBtn.addEventListener('click', handlers.onCreateSanction);

    sancBody.append(
        makeLabeledField('Duration Presets', presetRow),
        makeLabeledField('Duration (seconds)', durInput),
        permLabel,
        makeLabeledField('Audit Reason (Required)', reasonInput),
        makeLabeledField('Idempotency Key', sancIdem),
        submitSancBtn,
    );
    sancCard.appendChild(sancBody);

    // Card 2: Write-Behind Pending / DLQ Monitor + Replay
    const dlqCard = el('section', 'panel-card');
    const dlqHdr = el('div', 'panel-card__header');
    dlqHdr.append(
        el('h2', 'panel-card__title', 'Write-Behind Stream & DLQ Replay'),
        el('span', 'panel-card__hint', 'Pending vs DLQ vs Dead 분리 관측'),
    );
    dlqCard.appendChild(dlqHdr);

    const dlqBody = el('div', 'form-panel');
    const dlq = state.dlq || {};
    const statsList = el('div', 'truth-list');
    statsList.append(
        makeTruthRow(`Pending (${dlq.wb_stream || 'session_events'})`, String(dlq.pending_stream_length ?? '—')),
        makeTruthRow(`DLQ (${dlq.dlq_stream || 'session_events_dlq'})`, String(dlq.dlq_length ?? '—')),
        makeTruthRow(`Dead (${dlq.dead_stream || 'session_events_dead'})`, String(dlq.dead_length ?? '—')),
    );
    dlqBody.appendChild(statsList);

    const dlqGrid = el('div', 'form-grid');
    const maxItemsInput = el('input', 'search-input num');
    maxItemsInput.type = 'number';
    maxItemsInput.min = '1';
    maxItemsInput.max = '100';
    maxItemsInput.value = String(state.dlqReplayMax || 25);
    maxItemsInput.addEventListener('input', (e) => handlers.onUpdateDraft('dlqReplayMax', Number(e.target.value) || 25));

    const dlqReasonInput = el('input', 'search-input');
    dlqReasonInput.type = 'text';
    dlqReasonInput.placeholder = '필수: 재처리 사유';
    dlqReasonInput.value = state.dlqReason || '';
    dlqReasonInput.addEventListener('input', (e) => handlers.onUpdateDraft('dlqReason', e.target.value));

    dlqGrid.append(
        makeLabeledField('Batch Limit (1..100)', maxItemsInput),
        makeLabeledField('Audit Reason (Required)', dlqReasonInput),
    );

    const dlqIdem = createIdempotencyField(
        state.dlqIdem,
        (val) => handlers.onUpdateDraft('dlqIdem', val),
        () => handlers.onRegenerateIdem('dlqIdem', 'dlq'),
    );

    const replayBtn = el('button', 'btn btn--primary', 'Replay DLQ Batch');
    replayBtn.type = 'button';
    replayBtn.disabled = !canMutate;
    if (!canMutate) {
        replayBtn.title = makeMutationLockMessage(meta);
    }
    replayBtn.addEventListener('click', handlers.onReplayDlq);

    dlqBody.append(
        dlqGrid,
        makeLabeledField('Idempotency Key', dlqIdem),
        replayBtn,
    );

    const entries = Array.isArray(dlq.entries) ? dlq.entries : [];
    if (entries.length > 0) {
        const previewWrap = el('div', 'table-wrap');
        const ptbl = el('table', 'inventory-table');
        const pth = document.createElement('thead');
        const ptr = document.createElement('tr');
        for (const c of ['Stream ID', 'Type', 'Retry', 'Error']) {
            ptr.appendChild(el('th', '', c));
        }
        pth.appendChild(ptr);
        const ptb = document.createElement('tbody');
        for (const entry of entries) {
            const r = document.createElement('tr');
            r.append(
                el('td', 'mono', entry.stream_id || ''),
                el('td', 'mono', entry.type || ''),
                el('td', 'num', String(entry.retry_count ?? 0)),
                el('td', 'cell-detail', entry.error || '—'),
            );
            ptb.appendChild(r);
        }
        ptbl.append(pth, ptb);
        previewWrap.appendChild(ptbl);
        dlqBody.appendChild(previewWrap);
    } else {
        dlqBody.appendChild(el('div', 'empty-box', 'DLQ 스트림에 대기 중인 항목이 없습니다.'));
    }

    dlqCard.appendChild(dlqBody);
    topGrid.append(sancCard, dlqCard);
    fragment.appendChild(topGrid);

    // Bottom Card: Sanctions Ledger + Inline Revoke Panel
    const ledgerCard = el('section', 'panel-card');
    const ledgerHdr = el('div', 'panel-card__header');
    ledgerHdr.append(
        el('h2', 'panel-card__title', 'Sanctions Ledger'),
        el('span', 'panel-card__hint num', `${state.sanctions.length} records`),
    );
    ledgerCard.appendChild(ledgerHdr);

    const ledgerBar = el('div', 'filter-toolbar');
    const barLeft = el('div', 'chip-row');

    const subjFilter = el('input', 'search-input');
    subjFilter.type = 'search';
    subjFilter.placeholder = 'Filter subject...';
    subjFilter.value = state.sanctionFilterSubject || '';
    subjFilter.addEventListener('input', (e) => handlers.onSanctionFilterChange(e.target.value));
    subjFilter.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            handlers.onRefreshModeration();
        }
    });

    const activeLabel = el('label', 'checkbox-label');
    const activeChk = document.createElement('input');
    activeChk.type = 'checkbox';
    activeChk.checked = Boolean(state.sanctionActiveOnly);
    activeChk.addEventListener('change', () => handlers.onToggleSanctionActiveOnly(activeChk.checked));
    activeLabel.append(activeChk, document.createTextNode('Active Only'));

    const revokeReasonInput = el('input', 'search-input');
    revokeReasonInput.type = 'text';
    revokeReasonInput.placeholder = 'Revoke 사유 (조기 해제 시 필수)';
    revokeReasonInput.value = state.revokeReason || '';
    revokeReasonInput.addEventListener('input', (e) => handlers.onUpdateDraft('revokeReason', e.target.value));

    barLeft.append(subjFilter, activeLabel, revokeReasonInput);

    const refModBtn = el('button', 'btn', 'Refresh');
    refModBtn.type = 'button';
    refModBtn.addEventListener('click', handlers.onRefreshModeration);

    ledgerBar.append(barLeft, refModBtn);
    ledgerCard.appendChild(ledgerBar);

    const sWrap = el('div', 'table-wrap');
    const sTable = el('table', 'inventory-table');
    const sHead = document.createElement('thead');
    const sHtr = document.createElement('tr');
    for (const c of ['Subject', 'Kind', 'Status', 'Expires At', 'Reason', 'Actor Role', 'Action']) {
        sHtr.appendChild(el('th', '', c));
    }
    sHead.appendChild(sHtr);

    const sBody = document.createElement('tbody');
    for (const item of state.sanctions) {
        const tr = document.createElement('tr');
        const tdKind = document.createElement('td');
        tdKind.appendChild(
            item.kind === 'ban' ? makePill('pill--danger', 'BAN') : makePill('pill--warn', 'MUTE'),
        );
        const tdStatus = document.createElement('td');
        if (item.active) {
            tdStatus.appendChild(makePill('pill--ok', 'Active'));
        } else if (item.revoked_at_unix_ms > 0) {
            tdStatus.appendChild(makePill('', 'Revoked'));
        } else {
            tdStatus.appendChild(makePill('', 'Expired'));
        }

        const tdAct = document.createElement('td');
        if (item.active) {
            const revBtn = el('button', 'btn btn--danger btn--sm', 'Revoke');
            revBtn.type = 'button';
            revBtn.disabled = !canMutate;
            if (!canMutate) {
                revBtn.title = makeMutationLockMessage(meta);
            }
            revBtn.addEventListener('click', () => handlers.onRevokeSanction(item.sanction_id));
            tdAct.appendChild(revBtn);
        } else {
            tdAct.textContent = '—';
        }

        tr.append(
            el('td', 'mono', item.subject || ''),
            tdKind,
            tdStatus,
            el('td', 'mono', formatTimestampMs(item.expires_at_unix_ms)),
            el('td', 'cell-detail', item.reason || ''),
            el('td', 'mono', item.actor_role || ''),
            tdAct,
        );
        sBody.appendChild(tr);
    }

    sTable.append(sHead, sBody);
    sWrap.appendChild(sTable);
    ledgerCard.appendChild(sWrap);

    if (state.sanctions.length === 0) {
        ledgerCard.appendChild(el('div', 'empty-box', '표시할 제재 내역이 없습니다.'));
    }

    fragment.appendChild(ledgerCard);
    container.replaceChildren(fragment);
}

export function renderAuditView(container, state, handlers) {
    if (!container) {
        return;
    }

    const fragment = document.createDocumentFragment();
    const card = el('section', 'panel-card');
    const header = el('div', 'panel-card__header');
    header.append(
        el('h2', 'panel-card__title', 'Admin Audit Log (v2)'),
        el('span', 'panel-card__hint num', `${state.auditLogs.length} events`),
    );
    card.appendChild(header);

    const toolbar = el('div', 'filter-toolbar');
    const row = el('div', 'chip-row');

    const targetIn = el('input', 'search-input');
    targetIn.type = 'search';
    targetIn.placeholder = 'Filter target...';
    targetIn.value = state.auditTarget || '';
    targetIn.addEventListener('input', (e) => handlers.onAuditFilterChange('auditTarget', e.target.value));

    const actionIn = el('input', 'search-input');
    actionIn.type = 'search';
    actionIn.placeholder = 'Filter action...';
    actionIn.value = state.auditAction || '';
    actionIn.addEventListener('input', (e) => handlers.onAuditFilterChange('auditAction', e.target.value));

    const statusSel = el('select', 'select-input');
    for (const [val, label] of [['', 'All Status'], ['succeeded', 'Succeeded'], ['denied', 'Denied'], ['conflict', 'Conflict'], ['failed', 'Failed']]) {
        const opt = document.createElement('option');
        opt.value = val;
        opt.textContent = label;
        opt.selected = (state.auditStatus || '') === val;
        statusSel.appendChild(opt);
    }
    statusSel.addEventListener('change', (e) => handlers.onAuditFilterChange('auditStatus', e.target.value, true));

    row.append(targetIn, actionIn, statusSel);

    const refBtn = el('button', 'btn', 'Refresh');
    refBtn.type = 'button';
    refBtn.addEventListener('click', handlers.onRefreshAudit);

    toolbar.append(row, refBtn);
    card.appendChild(toolbar);

    const wrap = el('div', 'table-wrap');
    const table = el('table', 'inventory-table');
    const thead = document.createElement('thead');
    const htr = document.createElement('tr');
    for (const col of ['Time', 'Actor Role', 'Capability', 'Action', 'Target', 'Status', 'Idempotency Key', 'Reason']) {
        htr.appendChild(el('th', '', col));
    }
    thead.appendChild(htr);

    const tbody = document.createElement('tbody');
    for (const item of state.auditLogs) {
        const tr = document.createElement('tr');
        const tdStatus = document.createElement('td');
        if (item.status === 'succeeded') {
            tdStatus.appendChild(makePill('pill--ok', 'succeeded'));
        } else if (item.status === 'denied' || item.status === 'failed') {
            tdStatus.appendChild(makePill('pill--danger', item.status));
        } else if (item.status === 'conflict') {
            tdStatus.appendChild(makePill('pill--warn', 'conflict'));
        } else {
            tdStatus.appendChild(makePill('', item.status || 'unknown'));
        }

        tr.append(
            el('td', 'mono', formatTimestampMs(item.created_at_unix_ms)),
            el('td', 'mono', item.actor_role || ''),
            el('td', 'mono', item.capability || ''),
            el('td', 'mono', item.action || ''),
            el('td', 'mono', item.target || ''),
            tdStatus,
            el('td', 'mono', item.idempotency_key || '—'),
            el('td', 'cell-detail', item.reason || '—'),
        );
        tbody.appendChild(tr);
    }

    table.append(thead, tbody);
    wrap.appendChild(table);
    card.appendChild(wrap);

    if (state.auditLogs.length === 0) {
        card.appendChild(el('div', 'empty-box', '기록된 감사 이력이 없습니다.'));
    }

    fragment.appendChild(card);
    container.replaceChildren(fragment);
}
