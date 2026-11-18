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

async function fetchJson(path) {
    const headers = {
        Accept: 'application/json',
    };
    const token = getAuthToken();
    if (token) {
        headers.Authorization = `Bearer ${token}`;
    }

    const response = await window.fetch(path, {
        method: 'GET',
        headers,
        cache: 'no-store',
    });

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
    return fetchJson('/api/v1/overview');
}

export function fetchServiceDetail(serviceId) {
    return fetchJson(`/api/v1/services/${encodeURIComponent(serviceId)}`);
}
