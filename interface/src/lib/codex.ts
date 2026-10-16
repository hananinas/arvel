import { get } from 'svelte/store';
import { deviceRequest } from '$lib/agent';
import { ownerAccess, requireOwnerLogin } from '$lib/ownerAccess';

export interface CodexStatus {
	configured: boolean;
	busy: boolean;
	state: 'signed_out' | 'starting' | 'awaiting_approval' | 'connected' | 'error';
	userCode: string;
	expiresIn: number;
	error: string;
}

export class CodexRequestError extends Error {
	constructor(message: string, readonly status?: number) {
		super(message);
		this.name = 'CodexRequestError';
	}
}

function isStatus(value: unknown): value is CodexStatus {
	if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
	const status = value as Record<string, unknown>;
	const fields = ['configured', 'busy', 'state', 'userCode', 'expiresIn', 'error'];
	return Object.keys(status).length === fields.length && fields.every((field) => field in status) &&
		typeof status.configured === 'boolean' && typeof status.busy === 'boolean' &&
		['signed_out', 'starting', 'awaiting_approval', 'connected', 'error'].includes(String(status.state)) &&
		typeof status.userCode === 'string' && status.userCode.length <= 63 &&
		(status.userCode === '' || (status.state === 'awaiting_approval' && /^[A-Za-z0-9-]+$/.test(status.userCode))) &&
		typeof status.expiresIn === 'number' && Number.isInteger(status.expiresIn) && status.expiresIn >= 0 && status.expiresIn <= 900 &&
		typeof status.error === 'string' && status.error.length <= 191;
}

export async function codexRequest(
	action: 'status' | 'connect' | 'cancel' | 'disconnect', signal?: AbortSignal
): Promise<CodexStatus> {
	if (get(ownerAccess).status?.authenticated !== true) {
		throw new CodexRequestError('Log in to Settings to connect Codex.', 401);
	}
	try {
		const status = await deviceRequest<unknown>(`/rest/codex/${action}`, {
			method: action === 'status' ? 'GET' : 'POST', signal,
			...(action === 'status' ? {} : { body: '{}' })
		}, 8000);
		if (get(ownerAccess).status?.authenticated !== true) {
			throw new CodexRequestError('Your browser is no longer signed in. Log in to Settings again.', 401);
		}
		if (!isStatus(status)) throw new CodexRequestError('Arvel returned an unreadable Codex status. Refresh before trying again.');
		return status;
	} catch (error) {
		if (error instanceof CodexRequestError) throw error;
		const status = error && typeof error === 'object' && 'status' in error && typeof error.status === 'number' ? error.status : undefined;
		if (status === 401) requireOwnerLogin();
		const messages: Record<number, string> = {
			400: 'Arvel rejected the account request. Refresh this page and try again.',
			401: 'Your browser is no longer signed in. Log in to Settings again.',
			403: 'Open Arvel’s own address on your trusted home Wi-Fi, then retry.',
			409: 'A Codex sign-in is already in progress. Refresh its status or cancel it first.',
			413: 'Arvel rejected the account request size. Refresh this page and try again.',
			500: 'Arvel could not save the account change. Refresh to check the saved state before retrying.',
			503: 'Codex setup is unavailable on Arvel. Check the device and retry.'
		};
		// Provider bodies and network errors can contain credentials; never display them.
		throw new CodexRequestError(status && messages[status] ? messages[status] :
			'Could not confirm the account request. Check Arvel’s Wi-Fi connection, then refresh its Codex status before retrying.', status);
	}
}
