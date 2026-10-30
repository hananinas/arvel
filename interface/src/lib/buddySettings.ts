import { deviceRequest } from '$lib/agent';
import { arvelScenes } from '$lib/arvelScenes';
import { get } from 'svelte/store';
import { ownerAccess, requireOwnerLogin } from '$lib/ownerAccess';

export interface ProviderSettings {
	endpoint: string;
	model: string;
	apiKeyConfigured: boolean;
}

export type ConversationMode = 'text_pipeline' | 'codex_live' | 'native_audio';
export interface ChatSettings extends ProviderSettings { provider: string }
export interface LiveSettings extends ChatSettings { protocol: 'nemotron_labs_realtime' }
export interface LivePatch extends ProviderPatch {
    provider?: string;
    protocol?: 'nemotron_labs_realtime';
}

export const codexVoices = [
	'arbor', 'breeze', 'cove', 'ember', 'juniper', 'maple', 'sol', 'spruce', 'vale'
] as const;
export type CodexVoice = typeof codexVoices[number];
export type SpeechBackend = 'local' | 'codex';
export interface TtsSettings {
	backend: SpeechBackend;
	voice: CodexVoice;
}
export interface TtsPatch {
	backend?: SpeechBackend;
	voice?: CodexVoice;
}

export interface SearchSettings {
	provider: 'tavily' | 'parallel';
	enabled: boolean;
	tavilyApiKeyConfigured: boolean;
	parallelApiKeyConfigured: boolean;
}

export type VoiceStyle = 'warm' | 'bright' | 'soft';
export type ScreenSleep = '1min' | '5min' | 'never';
export type Persona = 'calm' | 'chatty' | 'focused';

export interface Preferences {
	userName: string;
	voiceStyle: VoiceStyle;
	speechRate: number;
	captions: boolean;
	brightness: number;
	dimAtNight: boolean;
	screenSleep: ScreenSleep;
	persona: Persona;
	volume: number;
	timezone: string;
}

export const defaultPreferences: Preferences = {
	userName: '', voiceStyle: 'warm', speechRate: 10, captions: true, brightness: 70,
	dimAtNight: true, screenSleep: '5min', persona: 'calm', volume: 60, timezone: 'UTC0'
};

export const voiceStyles: { id: VoiceStyle; name: string }[] = [
	{ id: 'warm', name: 'Warm' }, { id: 'bright', name: 'Bright' }, { id: 'soft', name: 'Soft' }
];
export const screenSleeps: { id: ScreenSleep; name: string }[] = [
	{ id: '1min', name: '1 min' }, { id: '5min', name: '5 min' }, { id: 'never', name: 'Never' }
];
export const personas: { id: Persona; name: string }[] = [
	{ id: 'calm', name: 'Calm' }, { id: 'chatty', name: 'Chatty' }, { id: 'focused', name: 'Focused' }
];

export interface BuddySettings {
	chat: ChatSettings;
	voice: ProviderSettings;
	tts: TtsSettings;
	conversationMode: ConversationMode;
	live: LiveSettings;
	search: SearchSettings;
	buddy: { name: string; instructions: string; readAloud: boolean; scene: string };
	preferences: Preferences;
	busy: boolean;
}

export interface ProviderPatch {
	endpoint?: string;
	model?: string;
	apiKey?: string;
	clearApiKey?: boolean;
}

export interface SearchPatch {
	provider?: SearchSettings['provider'];
	enabled?: boolean;
	tavilyApiKey?: string;
	parallelApiKey?: string;
	clearTavilyApiKey?: boolean;
	clearParallelApiKey?: boolean;
}

export interface BuddySettingsPatch {
	chat?: ProviderPatch & { provider?: string };
	voice?: ProviderPatch;
	tts?: TtsPatch;
	conversationMode?: ConversationMode;
	live?: LivePatch;
	search?: SearchPatch;
	buddy?: Partial<BuddySettings['buddy']>;
	preferences?: Partial<Preferences>;
}

export interface SettingsIssue {
	field: string;
	message: string;
}

const encoder = new TextEncoder();
const controls = /[\u0000-\u001f\u007f-\u009f]/;
const tokenCharacters = /[\s\u0000-\u001f\u007f-\u009f]/u;
const instructionControls = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f-\u009f]/;

export function utf8Bytes(value: string): number {
	return encoder.encode(value).length;
}

function record(value: unknown): value is Record<string, unknown> {
	return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function exactFields(value: Record<string, unknown>, fields: string[]): boolean {
	return Object.keys(value).length === fields.length && fields.every((field) => Object.prototype.hasOwnProperty.call(value, field));
}

function isProvider(value: unknown): value is ProviderSettings {
	return record(value) && exactFields(value, ['endpoint', 'model', 'apiKeyConfigured']) &&
		typeof value.endpoint === 'string' && utf8Bytes(value.endpoint) <= 512 && !controls.test(value.endpoint) &&
		typeof value.model === 'string' && utf8Bytes(value.model) <= 128 && !controls.test(value.model) &&
		typeof value.apiKeyConfigured === 'boolean';
}

function validProviderId(value: unknown): value is string {
	return typeof value === 'string' && value.length > 0 && utf8Bytes(value) <= 64 && !tokenCharacters.test(value);
}
function isChat(value: unknown): value is ChatSettings {
	return record(value) && exactFields(value, ['provider', 'endpoint', 'model', 'apiKeyConfigured']) &&
		validProviderId(value.provider) && isProvider({ endpoint: value.endpoint, model: value.model, apiKeyConfigured: value.apiKeyConfigured });
}
function isLive(value: unknown): value is LiveSettings {
	return record(value) && exactFields(value, ['provider', 'model', 'protocol', 'endpoint', 'apiKeyConfigured']) &&
		validProviderId(value.provider) && value.protocol === 'nemotron_labs_realtime' &&
		value.model === 'nvidia/NVIDIA-NemotronLabs-VoiceChat-11B' &&
		typeof value.endpoint === 'string' && (value.endpoint === '' || validLiveEndpoint(value.endpoint)) &&
		typeof value.apiKeyConfigured === 'boolean' && (value.endpoint !== '' || !value.apiKeyConfigured);
}

function isTts(value: unknown): value is TtsSettings {
	return record(value) && exactFields(value, ['backend', 'voice']) &&
		(value.backend === 'local' || value.backend === 'codex') &&
		codexVoices.some((voice) => voice === value.voice);
}

function isSearch(value: unknown): value is SearchSettings {
	return record(value) && exactFields(value, ['provider', 'enabled', 'tavilyApiKeyConfigured', 'parallelApiKeyConfigured']) &&
		(value.provider === 'tavily' || value.provider === 'parallel') &&
		typeof value.enabled === 'boolean' &&
		typeof value.tavilyApiKeyConfigured === 'boolean' &&
		typeof value.parallelApiKeyConfigured === 'boolean';
}

function validHost(host: string): boolean {
	if (!host || host.length > 253 || /[:\/\[\]@?#\\]/.test(host)) return false;
	const bare = host.endsWith('.') ? host.slice(0, -1) : host;
	if (!bare || !bare.split('.').every(label => /^[a-z\d](?:[a-z\d-]{0,61}[a-z\d])?$/i.test(label))) return false;
	if (/^[\d.]+$/.test(bare))
		return bare.split('.').length === 4 && bare.split('.').every(part => /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255);
	return true;
}
const preferenceFields = ['userName', 'voiceStyle', 'speechRate', 'captions', 'brightness', 'dimAtNight', 'screenSleep', 'persona', 'volume', 'timezone'];
const timezonePattern = /^[\x21-\x7e]{1,64}$/;

function stepInRange(value: unknown, min: number, max: number, step: number): boolean {
	return typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max && value % step === 0;
}

function preferenceIssue(key: string, value: unknown): string | null {
	switch (key) {
		case 'userName':
			return typeof value === 'string' && utf8Bytes(value) <= 32 && !controls.test(value) ? null : 'Your name can be empty or up to 32 UTF-8 bytes, without control characters.';
		case 'voiceStyle':
			return voiceStyles.some((entry) => entry.id === value) ? null : 'Choose Warm, Bright or Soft.';
		case 'speechRate':
			return stepInRange(value, 5, 20, 1) ? null : 'Speaking speed must be between 0.5× and 2.0×.';
		case 'brightness':
			return stepInRange(value, 10, 100, 10) ? null : 'Brightness must be 10–100% in steps of 10.';
		case 'volume':
			return stepInRange(value, 0, 100, 10) ? null : 'Volume must be 0–100% in steps of 10.';
		case 'screenSleep':
			return screenSleeps.some((entry) => entry.id === value) ? null : 'Choose 1 min, 5 min or Never.';
		case 'persona':
			return personas.some((entry) => entry.id === value) ? null : 'Choose Calm, Chatty or Focused.';
		case 'timezone':
			return typeof value === 'string' && timezonePattern.test(value) ? null : 'Choose a time zone from the list.';
		case 'captions':
		case 'dimAtNight':
			return typeof value === 'boolean' ? null : 'Choose on or off.';
		default:
			return 'Unknown preference.';
	}
}

export function isPreferences(value: unknown): value is Preferences {
	return record(value) && exactFields(value, preferenceFields) && preferenceFields.every((key) => preferenceIssue(key, value[key]) === null);
}

export function isBuddySettings(value: unknown): value is BuddySettings {
	if (!record(value) || !exactFields(value, ['chat', 'voice', 'tts', 'conversationMode', 'live', 'search', 'buddy', 'preferences', 'busy']) || !isPreferences(value.preferences)) return false;
	const buddy = value.buddy;
	return isChat(value.chat) && isProvider(value.voice) && isTts(value.tts) && isLive(value.live) &&
		['text_pipeline', 'codex_live', 'native_audio'].includes(value.conversationMode as string) &&
		(value.conversationMode !== 'native_audio' || (value.live as LiveSettings).endpoint !== '') &&
		isSearch(value.search) && typeof value.busy === 'boolean' &&
		record(buddy) && exactFields(buddy, ['name', 'instructions', 'readAloud', 'scene']) &&
		typeof buddy.name === 'string' && buddy.name.trim().length > 0 && utf8Bytes(buddy.name) <= 32 && !controls.test(buddy.name) &&
		typeof buddy.instructions === 'string' && utf8Bytes(buddy.instructions) <= 2048 && !instructionControls.test(buddy.instructions) &&
		typeof buddy.readAloud === 'boolean' &&
		typeof buddy.scene === 'string' && arvelScenes.some((scene) => scene.id === buddy.scene);
}

function validEndpoint(endpoint: string, scheme: 'https:' | 'wss:' = 'https:'): boolean {
	if (!endpoint.startsWith(`${scheme}//`) || tokenCharacters.test(endpoint) || utf8Bytes(endpoint) > 512) return false;
	try {
		const url = new URL(endpoint);
		if (url.protocol !== scheme || !url.hostname || url.username || url.password || endpoint.includes('?') || endpoint.includes('#')) return false;
		const authority = endpoint.slice(endpoint.indexOf('://') + 3).split('/')[0];
		if (endpoint.indexOf('/', endpoint.indexOf('://') + 3) < 0) return false;
		if (authority.includes('@') || authority.includes('\\') || endpoint.includes('\\')) return false;
		// URL validates IPv6 syntax and port range; additionally reject empty or zero ports.
		if (authority.endsWith(':') || (url.port && Number(url.port) === 0)) return false;
		if (url.hostname.startsWith('[')) return true;
		const host = url.hostname.endsWith('.') ? url.hostname.slice(0, -1) : url.hostname;
		return host.length <= 253 && host.split('.').every((label) => /^[a-z\d](?:[a-z\d-]{0,61}[a-z\d])?$/i.test(label));
	} catch {
		return false;
	}
}

function validLiveEndpoint(endpoint: string): boolean {
	if (!validEndpoint(endpoint, 'wss:') || endpoint.slice(endpoint.indexOf('/', 6)) !== '/v1/realtime') return false;
	const authority = endpoint.slice(6, endpoint.indexOf('/', 6));
	const [host, port, extra] = authority.split(':');
	return extra === undefined && validHost(host) &&
		(port === undefined || (/^\d{1,5}$/.test(port) && Number(port) > 0 && Number(port) <= 65535));
}


export function validateSettingsPatch(patch: BuddySettingsPatch, current: BuddySettings): SettingsIssue[] {
	const issues: SettingsIssue[] = [];
	// Check the runtime boundary without replacing the typed patch with an index signature.
	if (!record(patch as unknown)) return [{ field: 'conversation-mode', message: 'Settings must be a JSON object.' }];
	const fields: Record<string, readonly string[]> = {
		chat: ['provider', 'endpoint', 'model', 'apiKey', 'clearApiKey'],
		voice: ['endpoint', 'model', 'apiKey', 'clearApiKey'],
		live: ['provider', 'model', 'protocol', 'endpoint', 'apiKey', 'clearApiKey'],
		tts: ['backend', 'voice'],
		search: ['provider', 'enabled', 'tavilyApiKey', 'parallelApiKey', 'clearTavilyApiKey', 'clearParallelApiKey'],
		buddy: ['name', 'instructions', 'readAloud', 'scene'],
		preferences: preferenceFields
	};
	for (const [group, value] of Object.entries(patch)) {
		if (group === 'conversationMode') continue;
		if (!Object.prototype.hasOwnProperty.call(fields, group) || !record(value)) {
			issues.push({ field: 'conversation-mode', message: 'Unknown settings group or non-object settings patch.' });
			continue;
		}
		for (const [field, item] of Object.entries(value)) {
			if (!fields[group].includes(field)) {
				issues.push({ field: 'conversation-mode', message: 'Unknown settings field; reload the owner interface.' });
				continue;
			}
			if (group === 'preferences') continue;
			const boolean = ['clearApiKey', 'enabled', 'clearTavilyApiKey', 'clearParallelApiKey', 'readAloud'].includes(field);
			if (typeof item !== (boolean ? 'boolean' : 'string'))
				issues.push({ field: 'conversation-mode', message: 'Settings field has an invalid value type.' });
		}
	}
	if (issues.length) return issues;
	for (const group of ['chat', 'voice'] as const) {
		const provider = patch[group];
		if (!provider) continue;
		if (provider.endpoint !== undefined && !validEndpoint(provider.endpoint)) {
			issues.push({ field: `${group}-endpoint`, message: 'Use a full HTTPS endpoint URL (up to 512 UTF-8 bytes), with a valid host and port and no login, query, fragment or whitespace.' });
		}
		if (provider.model !== undefined && (!provider.model || tokenCharacters.test(provider.model) || utf8Bytes(provider.model) > 128)) {
			issues.push({ field: `${group}-model`, message: 'Enter a model identifier up to 128 UTF-8 bytes, without whitespace or control characters.' });
		}
		if (provider.apiKey && (/[^\x21-\x7e]/.test(provider.apiKey) || utf8Bytes(provider.apiKey) > 1024)) {
			issues.push({ field: `${group}-key`, message: 'The API key must be at most 1,024 ASCII bytes, without whitespace or control characters.' });
		}
		if (provider.clearApiKey && provider.apiKey) {
			issues.push({ field: `${group}-key`, message: 'Choose either a replacement API key or clearing the saved key, not both.' });
		}
		if ((provider.endpoint !== undefined && provider.endpoint !== current[group].endpoint ||
			group === 'chat' && patch.chat?.provider !== undefined && patch.chat.provider !== current.chat.provider) &&
			current[group].apiKeyConfigured && !provider.apiKey && !provider.clearApiKey) {
			issues.push({ field: `${group}-key`, message: 'This provider or endpoint changed. Enter a fresh key or explicitly clear the saved key; a saved key cannot be forwarded to a replacement.' });
		}
	}
	if (patch.chat?.provider !== undefined && !validProviderId(patch.chat.provider)) {
		issues.push({ field: 'chat-provider-id', message: 'Enter a provider identity up to 64 UTF-8 bytes, without whitespace or controls.' });
	}
	if (patch.conversationMode !== undefined && !['text_pipeline', 'codex_live', 'native_audio'].includes(patch.conversationMode)) {
		issues.push({ field: 'conversation-mode', message: 'Choose text pipeline, Codex live or native audio.' });
	}
	if (patch.live) {
		const live = patch.live;
		if (live.provider !== undefined && !validProviderId(live.provider))
			issues.push({ field: 'live-provider', message: 'Enter a provider identity up to 64 UTF-8 bytes, without whitespace or controls.' });
		if (live.model !== undefined && live.model !== 'nvidia/NVIDIA-NemotronLabs-VoiceChat-11B')
			issues.push({ field: 'live-model', message: 'This adapter supports the released NVIDIA Labs VoiceChat 11B model, not the early-access 12B model.' });
		if (live.protocol !== undefined && live.protocol !== 'nemotron_labs_realtime')
			issues.push({ field: 'live-protocol', message: 'Choose the documented NemotronLabs realtime protocol.' });
		if (live.endpoint !== undefined && live.endpoint !== '' && !validLiveEndpoint(live.endpoint))
			issues.push({ field: 'live-endpoint', message: 'Use the exact wss://host[:port]/v1/realtime endpoint, without credentials, query or fragment.' });
		if (live.apiKey !== undefined && (typeof live.apiKey !== 'string' || /[^\x21-\x7e]/.test(live.apiKey) || utf8Bytes(live.apiKey) > 1024))
			issues.push({ field: 'live-key', message: 'The API key must be at most 1,024 ASCII bytes, without whitespace or controls.' });
		if (live.clearApiKey !== undefined && typeof live.clearApiKey !== 'boolean')
			issues.push({ field: 'live-clear-key', message: 'Choose whether to clear the saved native deployment key.' });
		if (live.clearApiKey && live.apiKey)
			issues.push({ field: 'live-key', message: 'Choose a replacement key or clearing the saved key, not both.' });
		if ((live.endpoint !== undefined && live.endpoint !== current.live.endpoint ||
			live.provider !== undefined && live.provider !== current.live.provider) &&
			current.live.apiKeyConfigured && !live.apiKey && !live.clearApiKey)
			issues.push({ field: 'live-key', message: 'This deployment provider or endpoint changed. Enter a fresh key or explicitly clear the saved key.' });
		if ((live.endpoint ?? current.live.endpoint) === '' && !live.clearApiKey &&
			(live.apiKey || current.live.apiKeyConfigured) && !issues.some((issue) => issue.field === 'live-key'))
			issues.push({ field: 'live-key', message: 'Set the deployment endpoint before storing its key, or explicitly clear the saved key.' });
	}
	if ((patch.conversationMode ?? current.conversationMode) === 'native_audio' && (patch.live?.endpoint ?? current.live.endpoint) === '')
		issues.push({ field: 'live-endpoint', message: 'Set the deployment endpoint before selecting native audio.' });
	if (patch.search) {
		const search = patch.search;
		if (search.provider !== undefined && search.provider !== 'tavily' && search.provider !== 'parallel') {
			issues.push({ field: 'search-provider', message: 'Choose Tavily or Parallel.ai for web search.' });
		}
		if (search.enabled !== undefined && typeof search.enabled !== 'boolean') {
			issues.push({ field: 'search-enabled', message: 'Choose whether web search is enabled.' });
		}
		for (const [name, key, clear] of [
			['tavily', search.tavilyApiKey, search.clearTavilyApiKey],
			['parallel', search.parallelApiKey, search.clearParallelApiKey]
		] as const) {
			if (key !== undefined && (typeof key !== 'string' || /[^\x21-\x7e]/.test(key) || utf8Bytes(key) > 1024)) {
				issues.push({ field: `search-${name}-key`, message: 'The API key must be at most 1,024 ASCII bytes, without whitespace or control characters.' });
			}
			if (clear !== undefined && typeof clear !== 'boolean') {
				issues.push({ field: `search-${name}-clear-key`, message: 'Choose whether to clear the saved key.' });
			}
			if (clear && key) {
				issues.push({ field: `search-${name}-key`, message: 'Choose either a replacement API key or clearing the saved key, not both.' });
			}
		}
	}
	if (patch.tts) {
		const tts = patch.tts;
		const backend = tts.backend ?? current.tts.backend;
		if (backend !== 'local' && backend !== 'codex') {
			issues.push({ field: 'tts-backend', message: 'Choose Local speech or Codex subscription.' });
		}
		if (tts.voice !== undefined && !codexVoices.includes(tts.voice)) {
			issues.push({ field: 'tts-voice', message: 'Choose one of the listed Codex live voices.' });
		}
	}
	if (patch.buddy?.name !== undefined && (!patch.buddy.name.trim() || controls.test(patch.buddy.name) || utf8Bytes(patch.buddy.name) > 32)) {
		issues.push({ field: 'buddy-name', message: 'Enter a nonblank Buddy name up to 32 UTF-8 bytes, without control characters.' });
	}
	if (patch.buddy?.instructions !== undefined && (utf8Bytes(patch.buddy.instructions) > 2048 || instructionControls.test(patch.buddy.instructions))) {
		issues.push({ field: 'buddy-instructions', message: 'Instructions must be at most 2,048 UTF-8 bytes. Line breaks and tabs are allowed; other control characters are not.' });
	}
	if (patch.buddy?.scene !== undefined && !arvelScenes.some((scene) => scene.id === patch.buddy?.scene)) {
		issues.push({ field: 'buddy-scene', message: 'Choose one of the available Arvel scenes.' });
	}
	for (const [key, value] of Object.entries(patch.preferences || {})) {
		const message = preferenceIssue(key, value);
		if (message) issues.push({ field: `pref-${key}`, message });
	}
	return issues;
}

export class BuddySettingsRequestError extends Error {
	constructor(message: string, readonly status?: number) {
		super(message);
		this.name = 'BuddySettingsRequestError';
	}
}

// Settings share the device request helper, but never overlap their own GET/POST requests.
let requestTail: Promise<void> = Promise.resolve();
function serialRequest<T>(request: () => Promise<T>): Promise<T> {
	const result = requestTail.then(request);
	requestTail = result.then(() => undefined, () => undefined);
	return result;
}

async function requestSettings(patch?: BuddySettingsPatch, signal?: AbortSignal): Promise<BuddySettings> {
	if (get(ownerAccess).status?.authenticated !== true) {
		throw new BuddySettingsRequestError('Log in to edit Settings. Your non-secret draft has been kept.', 401);
	}
	try {
		const value = await deviceRequest<unknown>('/rest/agent/settings', {
			method: patch ? 'POST' : 'GET',
			signal,
			...(patch ? { body: JSON.stringify(patch) } : {})
		}, 8000);
		if (!isBuddySettings(value)) {
			throw new BuddySettingsRequestError('The device returned an unexpected settings response. Reload saved values to check what is stored. Your draft has been kept.');
		}
		return value;
	} catch (error) {
		if (error instanceof BuddySettingsRequestError) throw error;
		const status = record(error) && typeof error.status === 'number' ? error.status : undefined;
		if (status === 401) requireOwnerLogin();
		const messages: Record<number, string> = {
			400: 'The device rejected these settings. Check the field limits, endpoint URLs and replacement-key rule. Your draft has been kept.',
			401: 'Your browser is no longer signed in. Log in again to edit Settings. Your non-secret draft has been kept.',
			403: 'This Settings request was blocked by the origin check. Open Arvel’s address directly on your home Wi-Fi and retry. Your draft has been kept.',
			413: 'The settings request is too large for the device. Shorten the fields within their byte limits. Your draft has been kept.',
			409: 'Buddy is busy. Nothing was saved by this request. Wait until the current turn finishes, check device status, then save again.',
			500: 'The device could not store these settings. Your draft has been kept. Check device status and try saving again.',
			507: 'Arvel’s existing NVS partition lacks space for this update and its safety reserve. Nothing was written. Remove unused saved keys or use a smaller complete certificate chain; never truncate a token or certificate. No repartition or erasure was attempted. Your non-secret draft has been kept.'
		};
		// Never display arbitrary server/network error text: it could contain an entered credential.
		throw new BuddySettingsRequestError(status && messages[status] ? messages[status] :
			patch ? 'Could not confirm the save: the device may be unreachable, the request timed out, or its response was unreadable. Your draft has been kept. Reload saved values to check whether the save completed before retrying.' :
			'Could not load device settings. The device may be unreachable, the request timed out, or its response was unreadable. Check your connection to Buddy and retry. Your draft has been kept.', status);
	}
}

export function getBuddySettings(signal?: AbortSignal): Promise<BuddySettings> {
	return serialRequest(() => requestSettings(undefined, signal));
}

export function saveBuddySettings(patch: BuddySettingsPatch, signal?: AbortSignal): Promise<BuddySettings> {
	return serialRequest(() => requestSettings(patch, signal));
}
