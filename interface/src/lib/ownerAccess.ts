import { writable } from 'svelte/store';

export interface OwnerStatus {
	configured: boolean;
	authenticated: boolean;
	approvalOpen: boolean;
	storageAvailable: boolean;
	username?: string;
}

export class OwnerAccessError extends Error {
	constructor(message: string, readonly status?: number, readonly retryAfter = 0) {
		super(message);
		this.name = 'OwnerAccessError';
	}
}

export const ownerAccess = writable<{ status: OwnerStatus | null; checking: boolean; error: string }>({
	status: null, checking: false, error: ''
});

const messages: Record<string, string> = {
	invalid_credentials_format: 'Use a username of 1–32 letters, numbers, dots, underscores or hyphens and a password of 8–128 bytes without control characters.',
	login_required: 'Your browser is no longer signed in. Log in again; your non-secret Settings draft has been kept.',
	invalid_credentials: 'The username or password did not match. Try again, or choose Forgot password to recover access on the device.',
	physical_approval_required: 'On Arvel, open Settings > Device > Web login and generate a new code before saving this login.',
	approval_code_invalid: 'That approval code is incorrect or expired. Check the six-digit code on Arvel, or generate a new one.',
	cross_origin_denied: 'Open this page directly at Arvel’s address on your home Wi-Fi, then try again.',
	too_many_attempts: 'Too many attempts. Wait 60 seconds before trying again. If the approval window closed, generate a new code on Arvel.',
	auth_storage_error: 'Arvel could not read or save web-login storage. Existing access has not been replaced. Retry; creating or recovering access requires a new physical approval code.',
	auth_unavailable: 'Web login is unavailable on Arvel. Check the device and retry.'
};

function isStatus(value: unknown): value is OwnerStatus {
	if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
	const data = value as Record<string, unknown>;
	return ['configured', 'authenticated', 'approvalOpen', 'storageAvailable'].every((key) => typeof data[key] === 'boolean') &&
		(data.username === undefined || (data.authenticated === true && typeof data.username === 'string' && /^[A-Za-z0-9._-]{1,32}$/.test(data.username)));
}

let revision = 0;
let statusRequest: Promise<OwnerStatus> | null = null;

async function request(path: 'status' | 'login' | 'setup' | 'logout', body?: object): Promise<OwnerStatus> {
	const controller = new AbortController();
	const timer = setTimeout(() => controller.abort(), 15000);
	try {
		const response = await fetch(`/rest/auth/${path}`, {
			method: body ? 'POST' : 'GET', credentials: 'same-origin', cache: 'no-store',
			headers: { 'Content-Type': 'application/json' },
			...(body ? { body: JSON.stringify(body) } : {}), signal: controller.signal
		});
		const value: unknown = await response.json();
		if (!response.ok) {
			const code = value && typeof value === 'object' && 'error' in value ? String(value.error) : '';
			const delay = Number(response.headers.get('Retry-After'));
			throw new OwnerAccessError(messages[code] || 'Arvel rejected this web-login request. Check the device and retry.', response.status,
				response.status === 429 ? (Number.isFinite(delay) && delay > 0 ? Math.min(delay, 300) : 60) : 0);
		}
		if (!isStatus(value)) throw new OwnerAccessError('Arvel returned an unexpected web-login status. Retry before editing Settings.');
		if ((path === 'login' || path === 'setup') && !value.authenticated) throw new OwnerAccessError('Arvel did not confirm login. Retry before editing Settings.');
		return value;
	} catch (failure) {
		if (failure instanceof OwnerAccessError) throw failure;
		throw new OwnerAccessError('Could not confirm web access. Check your connection to Arvel’s home Wi-Fi and retry.');
	} finally {
		clearTimeout(timer);
	}
}

export function requireOwnerLogin() {
	revision++;
	ownerAccess.update((value) => ({ ...value, status: value.status ? { ...value.status, authenticated: false, username: undefined } : null,
		checking: false, error: messages.login_required }));
}

export function checkOwnerAccess(): Promise<OwnerStatus> {
	if (statusRequest) return statusRequest;
	const current = ++revision;
	ownerAccess.update((value) => ({ ...value, checking: true, error: '' }));
	statusRequest = request('status').then((status) => {
		if (current === revision) ownerAccess.set({ status, checking: false, error: '' });
		return status;
	}, (failure: OwnerAccessError) => {
		if (current === revision) ownerAccess.set({ status: null, checking: false, error: failure.message });
		throw failure;
	}).finally(() => { statusRequest = null; });
	return statusRequest;
}

export async function authenticateOwner(mode: 'login' | 'setup', credentials: { username: string; password: string; remember: boolean; code?: string }) {
	const current = ++revision;
	const status = await request(mode, credentials);
	if (current === revision) ownerAccess.set({ status, checking: false, error: '' });
	return status;
}

export async function logoutOwner() {
	const current = ++revision;
	const status = await request('logout', {});
	if (status.authenticated) throw new OwnerAccessError('Arvel did not confirm logout. Retry to revoke this browser’s access.');
	if (current === revision) ownerAccess.set({ status, checking: false, error: '' });
}
