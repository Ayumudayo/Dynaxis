const TOKEN_STORAGE_KEY = 'dynaxis_admin_bearer_token';

/**
 * HTTP control-plane API error wrapper.
 */
export class ApiError extends Error {
    constructor(status, code, message) {
        super(message || `HTTP ${status}`);
        this.name = 'ApiError';
        this.status = status;
        this.code = code || 'http_error';
    }
}

export function getAuthToken() {
    try {
        return window.sessionStorage.getItem(TOKEN_STORAGE_KEY) || '';
    } catch (_err) {
        return '';
    }
}

export function setAuthToken(token) {
    const trimmed = String(token || '').trim();
    try {
        if (trimmed) {
            window.sessionStorage.setItem(TOKEN_STORAGE_KEY, trimmed);
        } else {
            window.sessionStorage.removeItem(TOKEN_STORAGE_KEY);
        }
    } catch (_err) {
        // Ignore restricted sessionStorage errors
    }
}

export function clearAuthToken() {
    setAuthToken('');
}

export function generateIdempotencyKey(prefix = 'op') {
    if (window.crypto && typeof window.crypto.randomUUID === 'function') {
        return `${prefix}-${window.crypto.randomUUID()}`;
    }
    const rand = Math.floor(Math.random() * 0xffffffff).toString(16).padStart(8, '0');
    return `${prefix}-${Date.now().toString(16)}-${rand}`;
}

async function requestJson(path, { method = 'GET', body, idempotencyKey } = {}) {
    const headers = {
        Accept: 'application/json',
    };
    const token = getAuthToken();
    if (token) {
        headers.Authorization = `Bearer ${token}`;
    }
    if (idempotencyKey) {
        headers['Idempotency-Key'] = idempotencyKey;
    }
    const init = {
        method,
        headers,
        cache: 'no-store',
    };
    if (body !== undefined) {
        headers['Content-Type'] = 'application/json';
        init.body = JSON.stringify(body);
    }

    const response = await window.fetch(path, init);

    let payload = null;
    try {
        payload = await response.json();
    } catch (_err) {
        payload = null;
    }

    if (!response.ok) {
        const code = payload && typeof payload.error === 'string' ? payload.error : 'http_error';
        const message =
            payload && typeof payload.message === 'string'
                ? payload.message
                : `Request failed with status ${response.status}`;
        throw new ApiError(response.status, code, message);
    }

    return payload;
}

export function fetchOverview() {
    return requestJson('/api/v1/overview');
}

export function fetchServiceDetail(serviceId) {
    return requestJson(`/api/v1/services/${encodeURIComponent(serviceId)}`);
}

export function fetchChatRooms({ query = '', includeClosed = false, limit = 100 } = {}) {
    const params = new URLSearchParams();
    if (query) params.set('q', query);
    params.set('include_closed', includeClosed ? '1' : '0');
    params.set('limit', String(limit));
    return requestJson(`/api/v1/chat/rooms?${params.toString()}`);
}

export function fetchChatRoomDetail(roomName) {
    return requestJson(`/api/v1/chat/rooms/${encodeURIComponent(roomName)}`);
}

export function closeChatRoom(roomName, { reason, idempotencyKey }) {
    return requestJson(`/api/v1/chat/rooms/${encodeURIComponent(roomName)}/close`, {
        method: 'POST',
        body: { reason, idempotency_key: idempotencyKey },
        idempotencyKey,
    });
}

export function fetchChatUsers({ query = '', limit = 100 } = {}) {
    const params = new URLSearchParams();
    if (query) params.set('q', query);
    params.set('limit', String(limit));
    return requestJson(`/api/v1/chat/users?${params.toString()}`);
}

export function fetchChatUserDetail(subject) {
    return requestJson(`/api/v1/chat/users/${encodeURIComponent(subject)}`);
}

export function kickChatUser(subject, { reason, idempotencyKey }) {
    return requestJson(`/api/v1/chat/users/${encodeURIComponent(subject)}/kick`, {
        method: 'POST',
        body: { reason, idempotency_key: idempotencyKey },
        idempotencyKey,
    });
}

export function fetchSanctions({ activeOnly = true, subject = '', limit = 100 } = {}) {
    const params = new URLSearchParams();
    params.set('active_only', activeOnly ? '1' : '0');
    if (subject) params.set('subject', subject);
    params.set('limit', String(limit));
    return requestJson(`/api/v1/chat/sanctions?${params.toString()}`);
}

export function createSanction({ subject, kind, durationSec, confirmPermanent = false, reason, idempotencyKey }) {
    return requestJson('/api/v1/chat/sanctions', {
        method: 'POST',
        body: {
            subject,
            kind,
            duration_sec: durationSec,
            confirm_permanent: Boolean(confirmPermanent),
            reason,
            idempotency_key: idempotencyKey,
        },
        idempotencyKey,
    });
}

export function revokeSanction(sanctionId, { reason, idempotencyKey }) {
    return requestJson(`/api/v1/chat/sanctions/${encodeURIComponent(sanctionId)}`, {
        method: 'DELETE',
        body: { reason, idempotency_key: idempotencyKey },
        idempotencyKey,
    });
}

export function fetchWbDlq({ limit = 25 } = {}) {
    return requestJson(`/api/v1/wb/dlq?limit=${encodeURIComponent(String(limit))}`);
}

export function replayWbDlq({ maxItems = 25, reason, idempotencyKey }) {
    return requestJson('/api/v1/wb/dlq/replay', {
        method: 'POST',
        body: {
            max_items: maxItems,
            reason,
            idempotency_key: idempotencyKey,
        },
        idempotencyKey,
    });
}

export function fetchAuditLogs({ actor = '', action = '', target = '', status = '', limit = 100 } = {}) {
    const params = new URLSearchParams();
    if (actor) params.set('actor', actor);
    if (action) params.set('action', action);
    if (target) params.set('target', target);
    if (status) params.set('status', status);
    params.set('limit', String(limit));
    return requestJson(`/api/v1/audit-logs?${params.toString()}`);
}
