import { arvelScenes } from '$lib/arvelScenes';
import { isPreferences, type Preferences, type ConversationMode } from '$lib/buddySettings';
import { get } from 'svelte/store';
import { ownerAccess, requireOwnerLogin } from '$lib/ownerAccess';

export type ToolState = 'running' | 'succeeded' | 'failed' | 'unknown' | 'cancelled' | 'partial';
export interface AgentTool { id: string; kind: string | number; verb: string; target: string; state: ToolState; error: string }
export interface AgentSource {
    sourceId: string; url: string; title: string; excerpt: string; provider: string;
    requestId: string | null; fetchedUtcMs: number; fetchedMonotonicUs: number;
    callId: string; providerSession: string; conversationId: string;
}
export interface NativeReadiness { state: 'untested' | 'checking' | 'ready' | 'failed'; error: string; exercised: false }
export interface StorageStatus {
    state: string; conversationId: string; sqliteBytes: number; sqliteHighwater: number; databaseBytes: number; durable: boolean;
}

export type AgentState = 'idle' | 'clock_sync' | 'recording' | 'preview' | 'transcribing' | 'thinking' | 'speaking' | 'error' | 'listening' | 'muted' | 'voice_connecting';
export type VoicePhase = 'off' | 'connecting' | 'listening' | 'speaking' | 'working' | 'muted' | 'error';

export interface VoiceTranscript {
	role: 'user' | 'assistant';
	text: string;
	final: boolean;
}

export interface AgentMessage {
	id: number;
	role: 'user' | 'assistant';
	text: string;
	time: string;
    tools?: AgentTool[];
    sources?: AgentSource[];
    interrupted?: boolean;
}

export interface AgentSnapshot {
	source: 'NATIVE';
	mode: 'native_embedded';
	model: string;
	provider: string;
	conversationMode: ConversationMode;
	state: AgentState;
	status: string;
	busy: boolean;
	wifiConnected: boolean;
	clockReady: boolean;
    chatConfigured: boolean;
    buddyName: string;
	scene: string;
    readAloud: boolean;
	voiceConfigured: boolean;
	voicePreview: boolean;
	voiceLiveAvailable: boolean;
	voiceActive: boolean;
	voiceMuted: boolean;
	voiceConnecting: boolean;
	voicePhase: VoicePhase;
	voiceTranscript: VoiceTranscript | null;
	lastError: string;
	messageRevision: number;
	messages: AgentMessage[];
    tools: AgentTool[];
    sources: AgentSource[];
    storage: StorageStatus;
    conversationId: string;
    nativeReadiness: NativeReadiness;
	preferences: Preferences;
	ssid: string;
	ip: string;
	firmware: string;
	localTime: string;
	screenOff?: boolean;
	internalFree?: number;
	internalLargest?: number;
	psramFree?: number;
}

export class DeviceRequestError extends Error {
    constructor(public readonly status: number, message: string) {
        super(message);
        this.name = 'DeviceRequestError';
    }
}

export async function deviceRequest<T>(path: string, init: RequestInit = {}, timeout = 8000): Promise<T> {
	const controller = new AbortController();
	const abort = () => controller.abort();
	if (init.signal?.aborted) abort();
	init.signal?.addEventListener('abort', abort, { once: true });
	const timer = setTimeout(abort, timeout);
	try {
		const response = await fetch(path, {
			...init,
			credentials: 'same-origin',
			cache: 'no-store',
			headers: { 'Content-Type': 'application/json', ...init.headers },
			signal: controller.signal
		});
        const reader = response.body?.getReader();
        if (!reader) throw new Error('The device returned an empty response.');
        const bound = path.startsWith('/rest/device/history') ? 48 * 1024 : 128 * 1024;
        const decoder = new TextDecoder();
        let encoded = '', bytes = 0;
        try {
            for (;;) {
                const chunk = await reader.read();
                if (chunk.done) break;
                bytes += chunk.value.byteLength;
                if (bytes > bound) { await reader.cancel(); throw new Error('The device response exceeds its bounded page limit.'); }
                encoded += decoder.decode(chunk.value,{stream:true});
            }
            encoded += decoder.decode();
        } finally { reader.releaseLock(); }
        const body = JSON.parse(encoded);
        if (!object(body)) throw new Error('The device returned an unreadable response.');
		if (!response.ok) {
            const settingsErrors: Record<number, string> = {
                400: 'Check the settings fields, byte limits and endpoint replacement-key rule.',
                401: 'Log in to Settings to edit Arvel on home Wi-Fi. Your non-secret draft has been kept.',
                403: 'Open Arvel’s address directly on your home Wi-Fi. The Settings request was blocked by the origin check; your draft has been kept.',
                409: 'Buddy is busy. Wait for the current operation to finish, then save again.',
                413: 'Settings exceed the device’s size limit. Shorten the entered values.',
                500: 'The device could not save settings. Existing settings are unchanged.',
                503: 'The device is unavailable. Reconnect and try again when idle.'
            };
            const deviceErrors: Record<number, string> = {
                400: 'Arvel could not read that command. Refresh this page before trying again.',
                401: 'Owner login is required. Log in, then return Home; your draft has been kept.',
                403: 'Arvel blocked this request’s origin. Open the device directly, or check the localhost device proxy configuration.',
                409: 'Arvel is busy or voice is not ready. Check the device status and try again when idle.',
                413: 'This message exceeds the device’s size limit. Shorten it before sending.',
                503: 'Arvel cannot accept this request right now. Check its status and try again.'
            };
            const message = path.startsWith('/rest/device')
                ? deviceErrors[response.status] || `Device request failed (${response.status}). Check device status before retrying.`
                : response.status === 403 && body.error === 'physical_local_access_required'
                    ? 'On Arvel, open Settings → Device → Wi-Fi setup. Join Unravel-Agent with the displayed eight-digit password, then open http://192.168.4.1/. The old LAN address is not allowed, even while setup is open.'
                    : path === '/rest/agent/settings'
                        ? settingsErrors[response.status] || 'The settings request was rejected. Your draft has been kept.'
                        : `Device request failed (${response.status}). Check Arvel’s status before retrying.`;
            throw new DeviceRequestError(response.status, message);
		}
        if ((init.method === 'POST' || init.method === 'DELETE') && (path.startsWith('/rest/device/') || path === '/rest/agent/chat' || path === '/rest/agent/reset' || path === '/rest/agent/voice' || path === '/rest/agent/readiness') &&
			(response.status !== 202 || body.accepted !== true)) {
			throw new Error('The device did not accept this request. Your draft has been kept.');
		}
		return body as T;
	} catch (error) {
		if (controller.signal.aborted) {
			throw new Error('Request timed out or was interrupted. Check the device conversation before sending again.');
		}
		if (error instanceof TypeError) {
			throw new Error('Cannot reach your Buddy. Check that this phone or computer is on the device’s network, then try again.');
		}
		if (error instanceof SyntaxError) {
			throw new Error('The device returned an unreadable response. Reconnect to the device and try again.');
		}
		throw error;
	} finally {
		clearTimeout(timer);
		init.signal?.removeEventListener('abort', abort);
	}
}

export function isAgentSnapshot(value: unknown): value is AgentSnapshot {
	if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
	const data = value as Record<string, unknown>;
	const states: string[] = ['idle', 'clock_sync', 'recording', 'preview', 'transcribing', 'thinking', 'speaking', 'error', 'listening', 'muted', 'voice_connecting'];
	const phases: string[] = ['off', 'connecting', 'listening', 'speaking', 'working', 'muted', 'error'];
	const transcript = data.voiceTranscript as Record<string, unknown> | null | undefined;
	const validTranscript = transcript === null || (!!transcript && typeof transcript === 'object' &&
		(transcript.role === 'user' || transcript.role === 'assistant') &&
        typeof transcript.text === 'string' && typeof transcript.final === 'boolean' &&
        hasOnly(transcript,['role','text','final']));
    if (!hasOnly(data, ['source','mode','model','provider','conversationMode','state','status','busy','wifiConnected','clockReady',
        'chatConfigured','buddyName','scene','readAloud','voiceConfigured','voicePreview','voiceLiveAvailable','voiceActive',
        'voiceMuted','voiceConnecting','voicePhase','voiceTranscript','lastError','messageRevision','messages','preferences',
        'ssid','ip','firmware','localTime','screenOff','internalFree','internalLargest','psramFree','sessionId','configRevision',
        'tools','sources','storage','conversationId','nativeReadiness'])) return false;
    if (!isToolList(data.tools) || !isSourceList(data.sources) || !isStorage(data.storage) ||
        typeof data.conversationId !== 'string' || !isReadiness(data.nativeReadiness)) return false;
	const valid = data.source === 'NATIVE' && data.mode === 'native_embedded' &&
		['model', 'buddyName', 'status', 'lastError'].every((key) => typeof data[key] === 'string') &&
		['busy', 'wifiConnected', 'clockReady', 'chatConfigured', 'readAloud', 'voiceConfigured', 'voicePreview',
			'voiceLiveAvailable', 'voiceActive', 'voiceMuted', 'voiceConnecting'].every((key) => typeof data[key] === 'boolean') &&
		typeof data.provider === 'string' &&
		['text_pipeline', 'codex_live', 'native_audio'].includes(data.conversationMode as string) &&
		typeof data.voicePhase === 'string' && phases.includes(data.voicePhase) && validTranscript &&
		typeof data.state === 'string' && states.includes(data.state) &&
		typeof data.scene === 'string' && arvelScenes.some((scene) => scene.id === data.scene) &&
		typeof data.messageRevision === 'number' && Number.isFinite(data.messageRevision) &&
		Array.isArray(data.messages) && data.messages.length <= 8 && data.messages.every((message) =>
			message && typeof message.id === 'number' && Number.isInteger(message.id) &&
            (message.role === 'user' || message.role === 'assistant') && typeof message.text === 'string' &&
            typeof message.time === 'string' && hasOnly(message, ['id','role','text','time','tools','sources','interrupted']) &&
            (message.tools === undefined || isToolList(message.tools)) &&
            (message.sources === undefined || isSourceList(message.sources)) &&
            (message.interrupted === undefined || typeof message.interrupted === 'boolean')) &&
		isPreferences(data.preferences) &&
		(data.screenOff === undefined || typeof data.screenOff === 'boolean') &&
		['internalFree', 'internalLargest', 'psramFree'].every((key) => data[key] === undefined ||
			(typeof data[key] === 'number' && Number.isSafeInteger(data[key]) && data[key] >= 0)) &&
		['ssid', 'ip', 'firmware', 'localTime'].every((key) => typeof data[key] === 'string');
	return valid;
}

type Obj = Record<string, unknown>;
const object = (v: unknown): v is Obj => !!v && typeof v === 'object' && !Array.isArray(v);
const integer = (v: unknown): v is number => typeof v === 'number' && Number.isSafeInteger(v) && v >= 0;
const text = (v: unknown): v is string => typeof v === 'string';
const nullableTime = (v: unknown) => v === null || integer(v);
const hasOnly = (v: Obj, fields: string[]) => Object.keys(v).every((k) => fields.includes(k));
const fieldsText = (v: Obj, fields: string[]) => fields.every((k) => text(v[k]));
export const toolStates: ToolState[] = ['running', 'succeeded', 'failed', 'unknown', 'cancelled', 'partial'];
export function safeSourceUrl(value: unknown): string | null {
    if (!text(value) || value.length > 2048 || /[\u0000-\u0020\u007f\\]/.test(value)) return null;
    try {
        const url = new URL(value);
        return ['https:', 'http:'].includes(url.protocol) && !!url.hostname && !url.username && !url.password ? url.href : null;
    } catch { return null; }
}
function isToolList(v: unknown): v is AgentTool[] {
    return Array.isArray(v) && v.length <= 16 && v.every((t) => object(t) &&
        hasOnly(t, ['id','kind','verb','target','state','error']) && fieldsText(t, ['id','verb','target','error']) &&
        (text(t.kind) || integer(t.kind)) && toolStates.includes(t.state as ToolState));
}
function isSourceList(v: unknown): v is AgentSource[] {
    return Array.isArray(v) && v.length <= 8 && v.every((s) => object(s) &&
        hasOnly(s, ['sourceId','url','title','excerpt','provider','requestId','fetchedUtcMs','fetchedMonotonicUs','callId','providerSession','conversationId']) &&
        fieldsText(s, ['sourceId','url','title','excerpt','provider','callId','providerSession','conversationId']) &&
        !!s.sourceId && (s.requestId === null || text(s.requestId)) &&
        integer(s.fetchedUtcMs) && integer(s.fetchedMonotonicUs) && new TextEncoder().encode(s.excerpt as string).length <= 512);
}
function isReadiness(v: unknown): v is NativeReadiness {
    return object(v) && hasOnly(v, ['state','error','exercised']) &&
        ['untested','checking','ready','failed'].includes(v.state as string) && text(v.error) && v.exercised === false;
}
function isStorage(v: unknown): v is StorageStatus {
    return object(v) && hasOnly(v, ['state','conversationId','sqliteBytes','sqliteHighwater','databaseBytes','durable']) &&
        ['stopped','mounting','missing','mount_failed','ready','full','error'].includes(v.state as string) &&
        text(v.conversationId) && ['sqliteBytes','sqliteHighwater','databaseBytes'].every((k) => integer(v[k])) && typeof v.durable === 'boolean';
}

export interface HistoryQuery { after?: number; count?: number; literal?: string; utcFrom?: number; utcTo?: number; signal?: AbortSignal }
export interface ConversationSummary { id: number; uuid: string; title: string; closed: boolean; utc_ms: number | null }
export interface HistorySource { id: string; provider: string; request_id: string | null; url: string; title: string; excerpt: string; utc_ms: number | null; monotonic_us: number; boot_uuid: string; call_id: string; provider_session: string; conversation: string }
export interface HistoryMessage {
    id: number; conversation: string; provider_session: string; item_id: string; role: 'user' | 'assistant' | 'tool' | 'tool_unknown';
    text: string; interrupted: boolean; utc_ms: number | null; monotonic_us: number; boot_uuid: string;
    call_id: string; tool_name: string; sources: HistorySource[]; tools: AgentTool[];
}
export interface CursorPage { next_cursor: number; has_more: boolean }
export interface ConversationPage extends CursorPage { conversations: ConversationSummary[] }
export interface HistoryPage extends CursorPage { untrusted: true; messages: HistoryMessage[] }
export const historyExportKinds = ['messages','session_boundaries','tool_calls','sources','tool_evidence'] as const;
export type HistoryExportKind = typeof historyExportKinds[number];
export interface HistoryRowsPage extends CursorPage { untrusted: true; kind: 'sessions' | 'tool_calls' | 'sources' | 'tool_evidence'; rows: Obj[] }
const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
function uuidPath(uuid: string) {
    if (!uuidPattern.test(uuid)) throw new Error('Choose a valid archived conversation.');
    return `/rest/device/history/${encodeURIComponent(uuid)}`;
}
function queryString(q: HistoryQuery, filters = false) {
    const after = q.after ?? 0, count = q.count ?? 20;
    if (!integer(after) || !integer(count) || count < 1 || count > 20) throw new Error('History pages must contain 1–20 rows with a valid cursor.');
    const params = new URLSearchParams({ after: String(after), count: String(count) });
    if (filters) {
        if (q.literal !== undefined) {
            if (new TextEncoder().encode(q.literal).length > 256) throw new Error('History search is limited to 256 UTF-8 bytes.');
            if (q.literal) params.set('literal', q.literal);
        }
        for (const key of ['utcFrom','utcTo'] as const) if (q[key] !== undefined) {
            if (!integer(q[key])) throw new Error('Choose a valid history time range.');
            params.set(key, String(q[key]));
        }
    }
    return params.toString();
}
async function ownerRequest<T>(path: string, init: RequestInit = {}): Promise<T> {
    if (get(ownerAccess).status?.authenticated !== true) throw new DeviceRequestError(401, 'Owner login is required.');
    let revoked = false;
    const unsubscribe = ownerAccess.subscribe((value) => { if (!value.status?.authenticated) revoked = true; });
    try {
        const value = await deviceRequest<T>(path, init);
        if (revoked || !get(ownerAccess).status?.authenticated) throw new DeviceRequestError(401, 'Owner access changed. Log in again.');
        return value;
    } catch (error) {
        if (error instanceof DeviceRequestError && error.status === 401) requireOwnerLogin();
        throw error;
    } finally { unsubscribe(); }
}
function cursorPage(v: unknown, q: HistoryQuery): v is Obj & CursorPage {
    return object(v) && integer(v.next_cursor) && typeof v.has_more === 'boolean' &&
        (!v.has_more || v.next_cursor > (q.after ?? 0));
}
export async function fetchHistory(q: HistoryQuery = {}): Promise<ConversationPage> {
    const v = await ownerRequest<unknown>(`/rest/device/history?${queryString(q)}`, { signal: q.signal });
    if (!cursorPage(v,q) || !hasOnly(v,['conversations','next_cursor','has_more']) ||
        !Array.isArray(v.conversations) || v.conversations.length > (q.count ?? 20) ||
        !v.conversations.every((c) => object(c) && hasOnly(c,['id','uuid','title','closed','utc_ms']) &&
            integer(c.id) && text(c.uuid) && uuidPattern.test(c.uuid) && text(c.title) && typeof c.closed === 'boolean' && nullableTime(c.utc_ms)))
        throw new Error('The device returned an unreadable conversation page.');
    return v as unknown as ConversationPage;
}
function isHistoryPage(v: unknown, q: HistoryQuery): v is HistoryPage {
    return cursorPage(v,q) && hasOnly(v,['untrusted','messages','next_cursor','has_more']) && v.untrusted === true &&
        Array.isArray(v.messages) && v.messages.length <= (q.count ?? 20) && v.messages.every((m) => object(m) &&
            hasOnly(m,['id','conversation','provider_session','item_id','role','text','interrupted','utc_ms','monotonic_us','boot_uuid','call_id','tool_name','sources','tools']) &&
            integer(m.id) && fieldsText(m,['conversation','provider_session','item_id','role','text','boot_uuid','call_id','tool_name']) &&
            ['user','assistant','tool','tool_unknown'].includes(m.role as string) && typeof m.interrupted === 'boolean' &&
            nullableTime(m.utc_ms) && integer(m.monotonic_us) && isToolList(m.tools) && Array.isArray(m.sources) && m.sources.length <= 4 &&
            m.sources.every((s) => object(s) && hasOnly(s,['id','provider','request_id','url','title','excerpt','utc_ms','monotonic_us','boot_uuid','call_id','provider_session','conversation']) &&
                fieldsText(s,['id','provider','url','title','excerpt','boot_uuid','call_id','provider_session','conversation']) && (s.request_id === null || text(s.request_id)) &&
                nullableTime(s.utc_ms) && integer(s.monotonic_us)));
}
export async function fetchHistoryPage(uuid: string, q: HistoryQuery = {}): Promise<HistoryPage> {
    const v = await ownerRequest<unknown>(`${uuidPath(uuid)}?${queryString(q,true)}`, { signal: q.signal });
    if (!isHistoryPage(v,q)) throw new Error('The device returned an unreadable history page.');
    return v;
}
const exportColumns: Record<Exclude<HistoryExportKind,'messages'>, { strings: string[]; numbers: string[]; nullableNumbers: string[]; nullableStrings: string[] }> = {
    session_boundaries: { strings:['conversation','session','provider','model','reason','boot'], numbers:['cursor','id','mono_us'], nullableNumbers:['utc_ms'], nullableStrings:[] },
    tool_calls: { strings:['conversation','session','call_id','name','argument_hash','arguments','run_id','state','result','error','boot'], numbers:['cursor','id','generation','mono_us'], nullableNumbers:['utc_ms','finished_utc_ms','finished_mono_us'], nullableStrings:['finished_boot'] },
    sources: { strings:['conversation','session','call_id','source_id','provider','request_id','url','title','excerpt','boot'], numbers:['cursor','id','mono_us'], nullableNumbers:['utc_ms'], nullableStrings:[] },
    tool_evidence: { strings:['conversation','session','call_id','name','arguments'], numbers:['cursor','id','message_id'], nullableNumbers:[], nullableStrings:[] }
};
export async function fetchHistoryExportPage(uuid: string, kind: HistoryExportKind, q: HistoryQuery = {}): Promise<HistoryPage | HistoryRowsPage> {
    if (!historyExportKinds.includes(kind)) throw new Error('Choose a supported history export kind.');
    const v = await ownerRequest<unknown>(`${uuidPath(uuid)}/export?kind=${kind}&${queryString(q)}`, { signal:q.signal });
    if (kind === 'messages') {
        if (!isHistoryPage(v,q)) throw new Error('The device returned an unreadable message export page.');
        return v;
    }
    const columns = exportColumns[kind];
    if (!cursorPage(v,q) || !hasOnly(v,['untrusted','kind','rows','next_cursor','has_more']) || v.untrusted !== true ||
        v.kind !== (kind === 'session_boundaries' ? 'sessions' : kind) || !Array.isArray(v.rows) || v.rows.length > (q.count ?? 20) ||
        !v.rows.every((row) => object(row) && hasOnly(row,[...columns.strings,...columns.numbers,...columns.nullableNumbers,...columns.nullableStrings]) &&
            fieldsText(row,columns.strings) && columns.numbers.every((k) => integer(row[k])) &&
            columns.nullableNumbers.every((k) => nullableTime(row[k])) && columns.nullableStrings.every((k) => row[k] === null || text(row[k])) &&
            (kind !== 'tool_calls' || toolStates.includes(row.state as ToolState))))
        throw new Error('The device returned an unreadable evidence export page.');
    return v as unknown as HistoryRowsPage;
}
export async function memoryAction(action: 'new' | 'resume' | 'close' | 'delete', uuid?: string, signal?: AbortSignal) {
    const path = action === 'new' ? '/rest/device/history/new' : uuidPath(uuid ?? '') + (action === 'delete' ? '' : `/${action}`);
    const v = await ownerRequest<{accepted:boolean}>(path, { method:action === 'delete' ? 'DELETE' : 'POST', body:'{}', signal });
    if (v.accepted !== true) throw new Error('The device did not accept the memory action.');
    return v;
}
export async function fetchNativeReadiness(signal?: AbortSignal) {
    const v = await ownerRequest<{accepted:boolean}>('/rest/agent/readiness', {method:'POST',body:'{}',signal});
    if (v.accepted !== true) throw new Error('The device did not accept the readiness check.');
    return v;
}

export interface NikoProperty { name: string; value: string; definition: string; hasStatus: boolean; canControl: boolean; observedMonotonicUs: number }
export interface NikoDevice {
    id: string; name: string; type: string; technology: string; model: string; locationId: string; locationName: string;
    online: boolean; safeLightingClass: boolean; properties: NikoProperty[];
}
export interface NikoSnapshot {
    status: {
        enabled: boolean; configured: boolean; authenticated: boolean; connected: boolean; inventoryReady: boolean; stale: boolean;
        exercised: boolean; lastObservedMonotonicUs: number; deviceCount: number; allowlistCount: number; writeScope: string;
        tokenExpiresUtcSeconds: number; tokenExpired: boolean; error: string;
    };
    inventory: { ok: boolean; stale: boolean; error: string; devices: NikoDevice[]; nextCursor: number | null; inventoryRevision: number };
}
export interface NikoControlArgs { device_id: string; status?: 'On' | 'Off'; brightness?: number }
export async function fetchNiko(after = 0, count = 20, signal?: AbortSignal): Promise<NikoSnapshot> {
    const v = await ownerRequest<unknown>(`/rest/device/niko?${queryString({after,count})}`,{signal});
    if (!object(v) || !hasOnly(v,['status','inventory']) || !object(v.status) || !object(v.inventory)) throw new Error('Unreadable Niko snapshot.');
    const s = v.status, i = v.inventory;
    if (!hasOnly(s,['enabled','configured','authenticated','connected','inventoryReady','stale','exercised','lastObservedMonotonicUs','deviceCount','allowlistCount','writeScope','tokenExpiresUtcSeconds','tokenExpired','error']) ||
        !['enabled','configured','authenticated','connected','inventoryReady','stale','tokenExpired'].every((k) => typeof s[k] === 'boolean') ||
        typeof s.exercised !== 'boolean' || !fieldsText(s,['writeScope','error']) ||
        !['lastObservedMonotonicUs','deviceCount','allowlistCount','tokenExpiresUtcSeconds'].every((k) => integer(s[k])) ||
        !hasOnly(i,['ok','stale','error','devices','nextCursor','inventoryRevision']) || typeof i.ok !== 'boolean' || typeof i.stale !== 'boolean' ||
        !text(i.error) || !(i.nextCursor === null || integer(i.nextCursor) && i.nextCursor > after) || !integer(i.inventoryRevision) ||
        !Array.isArray(i.devices) || i.devices.length > count || !i.devices.every((d) => object(d) &&
            hasOnly(d,['id','name','type','technology','model','locationId','locationName','online','safeLightingClass','properties']) &&
            fieldsText(d,['id','name','type','technology','model','locationId','locationName']) && typeof d.online === 'boolean' &&
            typeof d.safeLightingClass === 'boolean' && Array.isArray(d.properties) && d.properties.length <= 16 &&
            d.properties.every((p) => object(p) && hasOnly(p,['name','value','definition','hasStatus','canControl','observedMonotonicUs']) &&
                fieldsText(p,['name','value','definition']) && typeof p.hasStatus === 'boolean' && typeof p.canControl === 'boolean' && integer(p.observedMonotonicUs))))
        throw new Error('The device returned an unreadable Niko status or inventory page.');
    return v as unknown as NikoSnapshot;
}
export async function submitNikoControl(args: NikoControlArgs, signal?: AbortSignal) {
    if (!uuidPattern.test(args.device_id) || !hasOnly(args as unknown as Obj,['device_id','status','brightness']) ||
        args.status === undefined && args.brightness === undefined || args.status !== undefined && !['On','Off'].includes(args.status) ||
        args.brightness !== undefined && (!integer(args.brightness) || args.brightness > 100)) throw new Error('Choose one discovered light and valid absolute desired properties.');
    return ownerRequest<{accepted:true}>('/rest/device/niko/control',{method:'POST',body:JSON.stringify(args),signal});
}
