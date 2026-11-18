import { createStatusBadge, formatTimestampMs } from '/js/render.js';

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

function makeV3LockMessage(meta) {
    if (!meta) {
        return '권한 정보를 불러오는 중입니다.';
    }
    if (meta.read_only) {
        return 'ADMIN_READ_ONLY=1 active — v3 라이프사이클 및 설정 변경이 잠겨 있습니다.';
    }
    if (meta.actor_role !== 'admin') {
        return `Requires admin role (현재 역할: ${meta.actor_role || 'viewer'})`;
    }
    return '';
}

function createV3SafetyBanner(meta) {
    const msg = makeV3LockMessage(meta);
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
        input.value = onRegenerate();
    });

    row.append(input, regenBtn);
    return row;
}

function makeOperationStatusPill(status) {
    const st = String(status || 'unknown').toLowerCase();
    if (st === 'succeeded') {
        return makePill('pill--ok', '✓ Succeeded');
    }
    if (st === 'failed' || st === 'timed_out') {
        return makePill('pill--danger', st === 'timed_out' ? '✕ Timed Out' : '✕ Failed');
    }
    if (st === 'running' || st === 'verifying') {
        return makePill('pill--warn', st === 'verifying' ? '▲ Verifying' : '▲ Running');
    }
    return makePill('', st === 'queued' ? '● Queued' : st);
}

function describeTargetBlockReason(code) {
    if (code === 'operation_in_progress') {
        return '선행 작업 진행 중 (동시 재시작 잠금)';
    }
    if (code === 'peer_not_ready') {
        return '동일 계층 동료 인스턴스 미준비 (연쇄 중단 방지)';
    }
    return code || 'Blocked';
}

export function renderOperationsView(container, meta, v3State, handlers) {
    if (!container) {
        return;
    }

    const canAdminMutate = Boolean(meta && meta.v3_mutating_allowed);
    const lockMsg = makeV3LockMessage(meta);
    const fragment = document.createDocumentFragment();

    const safetyBanner = createV3SafetyBanner(meta);
    if (safetyBanner) {
        fragment.appendChild(safetyBanner);
    }
    fragment.appendChild(
        el(
            'div',
            'callout-banner',
            'Docker 소켓 비마운트 격리: Admin API는 작업 의도를 PostgreSQL 큐에 접수(202 Accepted)하며, 호스트 실행 어댑터(tools/admin_host_adapter.py)가 허용된 인자만 호출해 처리합니다.',
        ),
    );
    const opFb = createFeedbackBanner(v3State.opFeedback);
    if (opFb) {
        fragment.appendChild(opFb);
    }

    // Panel 1: Allowlisted Services
    const targetCard = el('section', 'panel-card');
    const targetHdr = el('div', 'panel-card__header');
    targetHdr.append(
        el('h2', 'panel-card__title', 'Allowlisted Lifecycle Targets (v3 · Drain & Restart)'),
        el('span', 'panel-card__hint', '카탈로그 허용 서비스만 재시작 가능 · 선행 /readyz 완료 전 후속 노드 잠금'),
    );
    targetCard.appendChild(targetHdr);

    const grid = el('div', 'ops-target-grid');
    const targets = Array.isArray(v3State.allowlistedTargets) ? v3State.allowlistedTargets : [];
    for (const t of targets) {
        const card = el('article', 'ops-target-card');
        const top = el('div', 'ops-target-card__top');
        top.append(el('span', 'mono', t.service_id || ''), createStatusBadge(t.status || 'unknown'));

        const metaBox = el('div', 'ops-target-card__meta num');
        const sibText =
            t.total_siblings > 0
                ? `Siblings Ready: ${t.ready_siblings ?? 0} / ${t.total_siblings}`
                : 'Singleton Worker (Peer lock N/A)';
        metaBox.append(
            el('div', '', `Layer: ${t.display_name || ''} (${t.layer || ''})`),
            el('div', '', `Active Sessions: ${t.active_sessions ?? 0}`),
            el('div', '', sibText),
        );

        const btn = el('button', 'btn btn--danger btn--sm', 'Restart Service...');
        btn.type = 'button';
        const allowed = canAdminMutate && Boolean(t.restart_allowed);
        btn.disabled = !allowed;
        if (!canAdminMutate) {
            btn.title = lockMsg;
        } else if (!t.restart_allowed) {
            btn.title = describeTargetBlockReason(t.block_reason);
        }
        btn.addEventListener('click', () => handlers.onOpenRestartConfirm(t));

        card.append(top, metaBox, btn);
        if (canAdminMutate && !t.restart_allowed && t.block_reason) {
            card.append(el('div', 'drawer-note', describeTargetBlockReason(t.block_reason)));
        }
        grid.appendChild(card);
    }
    targetCard.appendChild(grid);
    if (targets.length === 0) {
        targetCard.appendChild(el('div', 'empty-box', '허용 목록 대상 정보를 불러오는 중입니다.'));
    }
    fragment.appendChild(targetCard);

    // Panel 2: Operations Ledger
    const opsCard = el('section', 'panel-card');
    const opsHdr = el('div', 'panel-card__header');
    const opsList = Array.isArray(v3State.operations) ? v3State.operations : [];
    opsHdr.append(
        el('h2', 'panel-card__title', 'Async Operations Ledger (4-Stage Stepper)'),
        el('span', 'panel-card__hint num', `${opsList.length} operations · 행 클릭 시 스테퍼 열기`),
    );
    opsCard.appendChild(opsHdr);

    const toolbar = el('div', 'filter-toolbar');
    const hintText = v3State.activeOperationId
        ? `진행 중 작업: ${v3State.activeOperationId}`
        : '현재 대기/실행 중인 작업 없음';
    const refBtn = el('button', 'btn', 'Refresh');
    refBtn.type = 'button';
    refBtn.addEventListener('click', handlers.onRefreshOperations);
    toolbar.append(el('span', 'panel-card__hint mono', hintText), refBtn);
    opsCard.appendChild(toolbar);

    const wrap = el('div', 'table-wrap');
    const table = el('table', 'inventory-table');
    const thead = document.createElement('thead');
    const htr = document.createElement('tr');
    for (const col of ['Time', 'Type', 'Target', 'Status', 'Actor', 'Idempotency Key', 'Reason']) {
        htr.appendChild(el('th', '', col));
    }
    thead.appendChild(htr);

    const tbody = document.createElement('tbody');
    const selectedOpId = v3State.selectedOperationDetail ? v3State.selectedOperationDetail.operation_id : '';
    for (const op of opsList) {
        const isSelected = v3State.drawerKind === 'op_stepper' && selectedOpId === op.operation_id;
        const tr = el('tr', isSelected ? 'inventory-row inventory-row--selected' : 'inventory-row');
        tr.tabIndex = 0;
        tr.addEventListener('click', () => handlers.onSelectOperation(op.operation_id));
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                handlers.onSelectOperation(op.operation_id);
            }
        });

        const tdStatus = document.createElement('td');
        tdStatus.appendChild(makeOperationStatusPill(op.status));
        const tdIdem = el('td', 'mono cell-clip', op.idempotency_key || '—');
        tdIdem.title = op.idempotency_key || '';
        const tdReason = el('td', 'cell-detail', op.reason || '');
        tdReason.title = op.reason || '';

        tr.append(
            el('td', 'mono', formatTimestampMs(op.created_at_unix_ms)),
            el('td', 'mono', op.operation_type || ''),
            el('td', 'mono cell-clip', op.target_service || ''),
            tdStatus,
            el('td', 'mono', op.actor_role || ''),
            tdIdem,
            tdReason,
        );
        tbody.appendChild(tr);
    }

    table.append(thead, tbody);
    wrap.appendChild(table);
    opsCard.appendChild(wrap);
    if (opsList.length === 0) {
        opsCard.appendChild(el('div', 'empty-box', '아직 기록된 비동기 운영 작업이 없습니다.'));
    }

    fragment.appendChild(opsCard);
    container.replaceChildren(fragment);
}

export function renderRestartConfirmDrawer(bodyEl, targetObj, meta, v3State, handlers) {
    if (!bodyEl || !targetObj) {
        return;
    }
    const canAdminMutate = Boolean(meta && meta.v3_mutating_allowed);
    const fragment = document.createDocumentFragment();

    const fb = createFeedbackBanner(v3State.drawerFeedback);
    if (fb) {
        fragment.appendChild(fb);
    }

    const blast = el('div', 'blast-card');
    blast.append(
        el('div', 'drawer-section__title', 'Step 1 · Blast Radius Preview'),
        makeTruthRow('Target Service', targetObj.service_id || '', true),
        makeTruthRow('Topology Layer', targetObj.layer || '', true),
        makeTruthRow('Estimated Active Sessions', String(targetObj.active_sessions ?? 0)),
        makeTruthRow(
            'Ready Siblings',
            targetObj.total_siblings > 0 ? `${targetObj.ready_siblings ?? 0} / ${targetObj.total_siblings}` : 'Worker singleton',
        ),
        el('div', 'drawer-note', '재시작 후 /readyz 회복 확인 완료 시에만 작업이 Succeeded 로 마감됩니다.'),
    );
    fragment.appendChild(blast);

    const step2 = el('div', 'drawer-section');
    step2.appendChild(el('div', 'drawer-section__title', 'Step 2 · Explicit Target Confirmation'));

    const confirmInput = el('input', 'search-input mono');
    confirmInput.type = 'text';
    confirmInput.placeholder = `Type ${targetObj.service_id}`;
    confirmInput.value = v3State.restartConfirmInput || '';
    confirmInput.addEventListener('input', (e) => {
        v3State.restartConfirmInput = e.target.value;
        submitBtn.disabled = !canSubmit();
    });

    const reasonInput = el('input', 'search-input');
    reasonInput.type = 'text';
    reasonInput.placeholder = '재시작 감사 사유 입력 (필수)';
    reasonInput.value = v3State.restartReason || '';
    reasonInput.addEventListener('input', (e) => {
        v3State.restartReason = e.target.value;
        submitBtn.disabled = !canSubmit();
    });

    const idemField = createIdempotencyField(
        v3State.restartIdem,
        (v) => {
            v3State.restartIdem = v;
        },
        () => handlers.onRegenerateV3Key('restartIdem', 'restart'),
    );

    const submitBtn = el('button', 'btn btn--danger', `Queue Restart: ${targetObj.service_id}`);
    submitBtn.type = 'button';
    const canSubmit = () =>
        canAdminMutate &&
        Boolean(targetObj.restart_allowed) &&
        String(v3State.restartConfirmInput || '').trim() === targetObj.service_id &&
        String(v3State.restartReason || '').trim().length > 0;
    submitBtn.disabled = !canSubmit();
    if (!canAdminMutate) {
        submitBtn.title = makeV3LockMessage(meta);
    }
    submitBtn.addEventListener('click', () => handlers.onSubmitRestart(targetObj.service_id));

    const actions = el('div', 'drawer-actions');
    actions.appendChild(submitBtn);

    step2.append(
        makeLabeledField(`Confirm Service ID (${targetObj.service_id})`, confirmInput),
        makeLabeledField('Audit Reason (Required)', reasonInput),
        makeLabeledField('Idempotency Key', idemField),
        actions,
    );
    fragment.appendChild(step2);
    bodyEl.replaceChildren(fragment);
}

export function renderOperationStepperDrawer(bodyEl, op, handlers) {
    if (!bodyEl || !op) {
        return;
    }
    const fragment = document.createDocumentFragment();

    const metaSec = el('div', 'drawer-section');
    metaSec.append(
        el('div', 'drawer-section__title', 'Operation Metadata'),
        makeTruthRow('Operation ID', op.operation_id || '', true),
        makeTruthRow('Type / Target', `${op.operation_type || ''} -> ${op.target_service || ''}`, true),
        makeTruthRow('Current Status', op.status || '', true),
        makeTruthRow('Actor Role', op.actor_role || '', true),
        makeTruthRow('Idempotency Key', op.idempotency_key || '', true),
        makeTruthRow('Reason', op.reason || ''),
    );
    if (op.error_message) {
        metaSec.appendChild(el('div', 'callout-banner callout-banner--error', op.error_message));
    }
    fragment.appendChild(metaSec);

    // 4-Stage Async Stepper
    const st = String(op.status || 'queued');
    const stageOrder = { queued: 0, running: 1, verifying: 2, succeeded: 3, failed: 3, timed_out: 3 };
    const idx = Object.prototype.hasOwnProperty.call(stageOrder, st) ? stageOrder[st] : 0;

    const stepperSec = el('div', 'drawer-section');
    stepperSec.appendChild(el('div', 'drawer-section__title', '4-Stage Execution Stepper'));

    const stages = [
        {
            title: '1. Queued',
            ts: formatTimestampMs(op.created_at_unix_ms),
            detail: '작업 원장에 202 접수 완료',
            cls: st === 'queued' ? 'step-card--active' : 'step-card--done',
        },
        {
            title: '2. Running (deploy_docker.ps1)',
            ts: formatTimestampMs(op.started_at_unix_ms),
            detail: '호스트 어댑터가 허용 인자로 컨테이너 제어 실행',
            cls:
                st === 'running'
                    ? 'step-card--active'
                    : idx > 1 || st === 'failed'
                      ? st === 'failed' && !op.verifying_at_unix_ms
                          ? 'step-card--error'
                          : 'step-card--done'
                      : '',
        },
        {
            title: '3. Verifying (/readyz)',
            ts: formatTimestampMs(op.verifying_at_unix_ms),
            detail: '어댑터 종료 후 대상 노드의 최신 /readyz 200 회복 대기',
            cls:
                st === 'verifying'
                    ? 'step-card--active'
                    : st === 'succeeded'
                      ? 'step-card--done'
                      : st === 'timed_out'
                        ? 'step-card--error'
                        : '',
        },
        {
            title:
                st === 'timed_out'
                    ? '4. Timed Out'
                    : st === 'failed'
                      ? '4. Failed'
                      : '4. Succeeded',
            ts: formatTimestampMs(op.finished_at_unix_ms),
            detail:
                st === 'succeeded'
                    ? '모든 대상 서비스 /readyz 정상 확인'
                    : st === 'failed' || st === 'timed_out'
                      ? op.error_message || '실행 또는 사후 검증 실패'
                      : '완료 대기 중',
            cls:
                st === 'succeeded'
                    ? 'step-card--done'
                    : st === 'failed' || st === 'timed_out'
                      ? 'step-card--error'
                      : '',
        },
    ];

    const list = el('div', 'stepper-list');
    for (const s of stages) {
        const card = el('div', `step-card ${s.cls}`.trim());
        const left = el('div', '');
        left.append(el('div', 'step-card__title', s.title), el('div', 'step-card__detail', s.detail));
        card.append(left, el('span', 'mono num', s.ts));
        list.appendChild(card);
    }
    stepperSec.appendChild(list);

    const refreshBtn = el('button', 'btn btn--sm', 'Probe & Refresh State');
    refreshBtn.type = 'button';
    refreshBtn.addEventListener('click', () => handlers.onSelectOperation(op.operation_id));
    stepperSec.appendChild(refreshBtn);
    fragment.appendChild(stepperSec);

    if (Array.isArray(op.step_log) && op.step_log.length > 0) {
        const logSec = el('div', 'drawer-section');
        logSec.appendChild(el('div', 'drawer-section__title', 'Step Timeline Events'));
        for (const item of op.step_log) {
            logSec.appendChild(
                makeTruthRow(
                    `${formatTimestampMs(item.at_unix_ms)} · ${item.stage || ''}`,
                    item.detail || '',
                ),
            );
        }
        fragment.appendChild(logSec);
    }

    bodyEl.replaceChildren(fragment);
}

function computeConfigDiff(schema, baseValues, draftValues) {
    const diff = [];
    for (const field of schema) {
        const before = baseValues ? baseValues[field.key] : undefined;
        const after = draftValues ? draftValues[field.key] : undefined;
        if (before !== after) {
            diff.push({
                key: field.key,
                before,
                after,
                apply_mode: field.apply_mode,
                scope: field.scope,
            });
        }
    }
    return diff;
}

export function renderConfigView(container, meta, v3State, handlers) {
    if (!container) {
        return;
    }

    const canAdminMutate = Boolean(meta && meta.v3_mutating_allowed);
    const lockMsg = makeV3LockMessage(meta);
    const cfg = v3State.configData || {
        current_revision: 0,
        current_values: {},
        schema: [],
        secret_refs: [],
        revisions: [],
    };
    const schema = Array.isArray(cfg.schema) ? cfg.schema : [];
    const baseValues = cfg.current_values || {};
    const draftValues = v3State.draftValues || { ...baseValues };
    const diff = computeConfigDiff(schema, baseValues, draftValues);

    const fragment = document.createDocumentFragment();
    const safetyBanner = createV3SafetyBanner(meta);
    if (safetyBanner) {
        fragment.appendChild(safetyBanner);
    }
    const fb = createFeedbackBanner(v3State.cfgFeedback);
    if (fb) {
        fragment.appendChild(fb);
    }

    // Panel 1: Schema-Driven Config Form + Mandatory Diff Box
    const formCard = el('section', 'panel-card');
    const hdr = el('div', 'panel-card__header');
    hdr.append(
        el('h2', 'panel-card__title', `Schema-Driven Configuration (Active Revision #${cfg.current_revision ?? 0})`),
        el('span', 'panel-card__hint', '.env 직접 편집 차단 · [runtime] 즉시 반영 vs [restart_required] 재기동 연동'),
    );
    formCard.appendChild(hdr);

    const schemaList = el('div', 'schema-list');
    for (const field of schema) {
        const row = el('div', 'schema-row');
        const info = el('div', '');
        const titleRow = el('div', 'schema-row__title');
        const modePill =
            field.apply_mode === 'runtime'
                ? makePill('pill--ok', '[runtime]')
                : makePill('pill--warn', '[restart_required]');
        titleRow.append(el('span', 'mono', field.key), modePill, el('span', 'pill', field.scope));
        const descText =
            field.type === 'integer'
                ? `${field.description} (범위: ${field.min} ~ ${field.max})`
                : `${field.description} (boolean)`;
        info.append(titleRow, el('div', 'schema-row__desc', descText));

        const ctrlWrap = el('div', '');
        if (field.type === 'boolean') {
            const label = el('label', 'checkbox-label');
            const chk = document.createElement('input');
            chk.type = 'checkbox';
            chk.checked = Boolean(draftValues[field.key]);
            chk.addEventListener('change', () => handlers.onUpdateConfigDraft(field.key, chk.checked));
            label.append(chk, el('span', 'mono', chk.checked ? 'Enabled (1)' : 'Disabled (0)'));
            ctrlWrap.appendChild(label);
        } else {
            const numInput = el('input', 'search-input num');
            numInput.type = 'number';
            numInput.min = String(field.min);
            numInput.max = String(field.max);
            numInput.value = String(draftValues[field.key] ?? '');
            numInput.addEventListener('input', (e) => {
                const parsed = Number.parseInt(e.target.value, 10);
                handlers.onUpdateConfigDraft(field.key, Number.isNaN(parsed) ? e.target.value : parsed);
            });
            ctrlWrap.appendChild(numInput);
        }
        row.append(info, ctrlWrap);
        schemaList.appendChild(row);
    }
    formCard.appendChild(schemaList);

    // Before / After Diff Preview + 2-step confirmation
    const diffBox = el('div', 'diff-box');
    const diffHdr = el('div', 'safety-bar__group');
    const resetBtn = el('button', 'btn btn--sm', 'Reset Draft');
    resetBtn.type = 'button';
    resetBtn.addEventListener('click', handlers.onResetConfigDraft);
    diffHdr.append(el('span', 'drawer-section__title', `Before / After Diff Preview (${diff.length} change(s))`), resetBtn);
    diffBox.appendChild(diffHdr);

    if (diff.length === 0) {
        diffBox.appendChild(el('div', 'drawer-note', '변경된 설정 항목이 없습니다. 위 스키마 값을 수정하면 적용 전 Diff가 표시됩니다.'));
    } else {
        const diffWrap = el('div', 'table-wrap');
        const diffTable = el('table', 'inventory-table');
        const dHead = document.createElement('thead');
        const dHtr = document.createElement('tr');
        for (const col of ['Key', 'Mode', 'Before', 'After']) {
            dHtr.appendChild(el('th', '', col));
        }
        dHead.appendChild(dHtr);
        const dBody = document.createElement('tbody');
        for (const d of diff) {
            const tr = document.createElement('tr');
            tr.append(
                el('td', 'mono', d.key),
                el('td', '', d.apply_mode),
                el('td', 'mono', String(d.before)),
                el('td', 'mono', String(d.after)),
            );
            dBody.appendChild(tr);
        }
        diffTable.append(dHead, dBody);
        diffWrap.appendChild(diffTable);
        diffBox.appendChild(diffWrap);

        const confirmGrid = el('div', 'form-grid');
        const confirmInput = el('input', 'search-input mono');
        confirmInput.type = 'text';
        confirmInput.placeholder = 'Type stack-config';
        confirmInput.value = v3State.configConfirmInput || '';
        confirmInput.addEventListener('input', (e) => {
            v3State.configConfirmInput = e.target.value;
            applyBtn.disabled = !canSubmitConfig();
        });

        const reasonInput = el('input', 'search-input');
        reasonInput.type = 'text';
        reasonInput.placeholder = '설정 변경 감사 사유 입력 (필수)';
        reasonInput.value = v3State.configReason || '';
        reasonInput.addEventListener('input', (e) => {
            v3State.configReason = e.target.value;
            applyBtn.disabled = !canSubmitConfig();
        });
        confirmGrid.append(
            makeLabeledField('Confirm Target ("stack-config")', confirmInput),
            makeLabeledField('Audit Reason (Required)', reasonInput),
        );

        const idemCtrl = createIdempotencyField(
            v3State.configIdem,
            (v) => {
                v3State.configIdem = v;
            },
            () => handlers.onRegenerateV3Key('configIdem', 'cfg'),
        );

        const applyBtn = el('button', 'btn btn--primary', 'Apply Configuration');
        applyBtn.type = 'button';
        const canSubmitConfig = () =>
            canAdminMutate &&
            diff.length > 0 &&
            String(v3State.configConfirmInput || '').trim() === 'stack-config' &&
            String(v3State.configReason || '').trim().length > 0;
        applyBtn.disabled = !canSubmitConfig();
        if (!canAdminMutate) {
            applyBtn.title = lockMsg;
        }
        applyBtn.addEventListener('click', handlers.onApplyConfig);

        const btnRow = el('div', 'drawer-actions');
        btnRow.appendChild(applyBtn);
        diffBox.append(confirmGrid, makeLabeledField('Idempotency Key', idemCtrl), btnRow);
    }
    formCard.appendChild(diffBox);
    fragment.appendChild(formCard);

    // Panel 2: Masked Secret References
    const secCard = el('section', 'panel-card');
    const secHdr = el('div', 'panel-card__header');
    secHdr.append(
        el('h2', 'panel-card__title', 'Secret References (Read-Only Masked)'),
        el('span', 'panel-card__hint', '비밀 원문은 API 및 UI 어디에도 출력되지 않습니다'),
    );
    secCard.appendChild(secHdr);
    const secWrap = el('div', 'table-wrap');
    const secTable = el('table', 'inventory-table');
    const secHead = document.createElement('thead');
    const secHtr = document.createElement('tr');
    for (const c of ['Secret Key', 'Configured', 'Value', 'Editable', 'Purpose']) {
        secHtr.appendChild(el('th', '', c));
    }
    secHead.appendChild(secHtr);
    const secBody = document.createElement('tbody');
    for (const ref of Array.isArray(cfg.secret_refs) ? cfg.secret_refs : []) {
        const tr = document.createElement('tr');
        tr.append(
            el('td', 'mono', ref.key || ''),
            el('td', 'mono', ref.configured ? 'YES' : 'NO'),
            el('td', 'mono', ref.masked_value || '—'),
            el('td', 'mono', 'false'),
            el('td', 'cell-detail', ref.description || ''),
        );
        secBody.appendChild(tr);
    }
    secTable.append(secHead, secBody);
    secWrap.appendChild(secTable);
    secCard.appendChild(secWrap);
    fragment.appendChild(secCard);

    // Panel 3: Revisions History & Rollback
    const revCard = el('section', 'panel-card');
    const revHdr = el('div', 'panel-card__header');
    revHdr.append(
        el('h2', 'panel-card__title', 'Configuration Revision History'),
        el('span', 'panel-card__hint num', `Active: #${cfg.current_revision ?? 0}`),
    );
    revCard.appendChild(revHdr);
    const revWrap = el('div', 'table-wrap');
    const revTable = el('table', 'inventory-table');
    const revHead = document.createElement('thead');
    const revHtr = document.createElement('tr');
    for (const c of ['Rev', 'Mode', 'Actor', 'Time', 'Reason', 'Rollback']) {
        revHtr.appendChild(el('th', '', c));
    }
    revHead.appendChild(revHtr);
    const revBody = document.createElement('tbody');
    for (const rev of Array.isArray(cfg.revisions) ? cfg.revisions : []) {
        const tr = document.createElement('tr');
        const tdAction = document.createElement('td');
        const isCurrent = rev.revision === cfg.current_revision;
        if (isCurrent) {
            tdAction.appendChild(makePill('pill--ok', 'Current'));
        } else {
            const rbBtn = el('button', 'btn btn--sm btn--danger', `Rollback to #${rev.revision}`);
            rbBtn.type = 'button';
            rbBtn.disabled = !canAdminMutate;
            if (!canAdminMutate) {
                rbBtn.title = lockMsg;
            }
            rbBtn.addEventListener('click', () => handlers.onOpenRollbackDrawer(rev));
            tdAction.appendChild(rbBtn);
        }
        const tdReason = el('td', 'cell-detail', rev.reason || '');
        tdReason.title = rev.reason || '';
        tr.append(
            el('td', 'mono num', `#${rev.revision}`),
            el('td', 'mono', rev.apply_mode || ''),
            el('td', 'mono', rev.actor_role || ''),
            el('td', 'mono', formatTimestampMs(rev.created_at_unix_ms)),
            tdReason,
            tdAction,
        );
        revBody.appendChild(tr);
    }
    revTable.append(revHead, revBody);
    revWrap.appendChild(revTable);
    revCard.appendChild(revWrap);
    fragment.appendChild(revCard);

    container.replaceChildren(fragment);
}

export function renderRollbackDrawer(bodyEl, revObj, meta, v3State, handlers) {
    if (!bodyEl || !revObj) {
        return;
    }
    const canAdminMutate = Boolean(meta && meta.v3_mutating_allowed);
    const fragment = document.createDocumentFragment();

    const fb = createFeedbackBanner(v3State.drawerFeedback);
    if (fb) {
        fragment.appendChild(fb);
    }

    const blast = el('div', 'blast-card');
    blast.append(
        el('div', 'drawer-section__title', 'Rollback Target Revision'),
        makeTruthRow('Target Revision', `#${revObj.revision}`),
        makeTruthRow('Original Mode', revObj.apply_mode || '', true),
        makeTruthRow('Original Reason', revObj.reason || ''),
        el('div', 'drawer-note', 'restart_required 설정이 복원되면 영향 서비스 재기동 작업이 큐에 등록됩니다.'),
    );
    fragment.appendChild(blast);

    const sec = el('div', 'drawer-section');
    const confirmIn = el('input', 'search-input mono');
    confirmIn.type = 'text';
    confirmIn.placeholder = 'Type stack-config';
    confirmIn.value = v3State.rollbackConfirmInput || '';
    confirmIn.addEventListener('input', (e) => {
        v3State.rollbackConfirmInput = e.target.value;
        btn.disabled = !canSubmit();
    });

    const reasonIn = el('input', 'search-input');
    reasonIn.type = 'text';
    reasonIn.placeholder = '롤백 감사 사유 입력 (필수)';
    reasonIn.value = v3State.rollbackReason || '';
    reasonIn.addEventListener('input', (e) => {
        v3State.rollbackReason = e.target.value;
        btn.disabled = !canSubmit();
    });

    const idemCtrl = createIdempotencyField(
        v3State.rollbackIdem,
        (v) => {
            v3State.rollbackIdem = v;
        },
        () => handlers.onRegenerateV3Key('rollbackIdem', 'rb'),
    );

    const btn = el('button', 'btn btn--danger', `Queue Rollback to #${revObj.revision}`);
    btn.type = 'button';
    const canSubmit = () =>
        canAdminMutate &&
        String(v3State.rollbackConfirmInput || '').trim() === 'stack-config' &&
        String(v3State.rollbackReason || '').trim().length > 0;
    btn.disabled = !canSubmit();
    if (!canAdminMutate) {
        btn.title = makeV3LockMessage(meta);
    }
    btn.addEventListener('click', () => handlers.onSubmitRollback(revObj.revision));

    const actions = el('div', 'drawer-actions');
    actions.appendChild(btn);
    sec.append(
        makeLabeledField('Confirm Target ("stack-config")', confirmIn),
        makeLabeledField('Audit Reason (Required)', reasonIn),
        makeLabeledField('Idempotency Key', idemCtrl),
        actions,
    );
    fragment.appendChild(sec);
    bodyEl.replaceChildren(fragment);
}
