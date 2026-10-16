export interface CatalogModel {
	providerId: string;
	id: string;
	name: string;
	status: string | null;
	tools: boolean | null;
	input: string[] | null;
	output: string[] | null;
	context: number | null;
	maxOutput: number | null;
	pricing: { input: number | null; output: number | null; cacheRead: number | null };
}

export interface CatalogProvider {
	id: string;
	name: string;
	api: string | null;
	models: CatalogModel[];
}

export interface CatalogSnapshot {
	fetchedAt: number;
	providers: CatalogProvider[];
}

export type CatalogSelection = {
	kind: 'text_pipeline' | 'codex_live' | 'native_audio';
	provider: string;
	model: string;
	endpoint: string;
	protocol: 'openai_text' | 'codex_live' | 'nemotron_labs_realtime';
};

export const OPENAI_TEXT_PROVIDER_IDS = [
	'openai', 'nebius', 'ollama', 'openrouter', 'groq', 'togetherai',
	'fireworks-ai', 'deepinfra', 'xai', 'nvidia', 'custom'
] as const;
export const CODEX_LIVE_PROVIDER_ID = 'openai';
export const CODEX_LIVE_MODEL_ID = 'gpt-live-1-codex';
// The account integration owns its fixed trusted endpoints; no catalog endpoint is used.
export const CODEX_LIVE_ENDPOINT = '';
export const NEMOTRON_LABS_MODEL_ID = 'nvidia/NVIDIA-NemotronLabs-VoiceChat-11B';
export const NEMOTRON_LABS_PROVIDER_IDS = ['nebius', 'nvidia', 'custom'] as const;

const CATALOG_URL = 'https://models.dev/api.json';
const RESPONSE_BYTES = 16 * 1024 * 1024;
const CACHE_BYTES = 4 * 1024 * 1024;
const CACHE_KEY = 'arvel.modelCatalog.v1';
const CACHE_VERSION = 1;
const TIMEOUT_MS = 10_000;
const controls = /[\u0000-\u001f\u007f]/;
const encoder = new TextEncoder();

type CatalogErrorCode = 'invalid_catalog' | 'empty_catalog' | 'no_valid_entries' |
	'http_error' | 'too_large' | 'invalid_response' | 'browser_only';

export class ModelCatalogError extends Error {
	constructor(public readonly code: CatalogErrorCode, message: string) {
		super(message);
		this.name = 'ModelCatalogError';
	}
}

function record(value: unknown): value is Record<string, unknown> {
	return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function text(value: unknown, max = 512): string | null {
	return typeof value === 'string' && value.trim().length > 0 &&
		value.length <= max && !controls.test(value) ? value : null;
}

function identity(value: unknown): string | null {
	const result = text(value);
	return result !== null && !/\s/.test(result) ? result : null;
}

function number(value: unknown, integer = false): number | null {
	return typeof value === 'number' && Number.isFinite(value) && value >= 0 &&
		(!integer || (Number.isSafeInteger(value) && value > 0)) ? value : null;
}

function modalities(value: unknown): string[] | null {
	if (!Array.isArray(value) || value.length > 32 || value.some((item) => !identity(item))) return null;
	return [...new Set(value as string[])];
}

function safeApi(value: unknown): string | null {
	if (!text(value, 2048) || /\s/.test(value as string)) return null;
	try {
		const url = new URL(value as string);
		return url.protocol === 'https:' && !!url.hostname && !url.username && !url.password &&
			!url.search && !url.hash ? url.href : null;
	} catch {
		return null;
	}
}

/** Metadata only. Provider map keys own identity, as in the sibling harness. */
export function parseModelCatalog(value: unknown, fetchedAt = Date.now()): CatalogSnapshot {
	if (!record(value) || number(fetchedAt) === null) {
		throw new ModelCatalogError('invalid_catalog', 'The model catalog is not a provider map.');
	}
	const entries = Object.entries(value).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0);
	if (!entries.length) throw new ModelCatalogError('empty_catalog', 'The model catalog is empty.');
	const providers: CatalogProvider[] = [];
	for (const [providerId, raw] of entries) {
		if (!identity(providerId) || !record(raw) || !record(raw.models)) continue;
		const models: CatalogModel[] = [];
		const seen = new Set<string>();
		for (const [key, entry] of Object.entries(raw.models).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)) {
			if (!record(entry)) continue;
			const id = entry.id === undefined || entry.id === null || entry.id === '' ? identity(key) : identity(entry.id);
			if (!id || seen.has(id)) continue;
			if (entry.status !== undefined && entry.status !== null && !text(entry.status)) continue;
			const limit = record(entry.limit) ? entry.limit : {};
			const cost = record(entry.cost) ? entry.cost : {};
			const modes = record(entry.modalities) ? entry.modalities : {};
			models.push({
				providerId, id, name: text(entry.name) ?? id,
				status: text(entry.status),
				tools: typeof entry.tool_call === 'boolean' ? entry.tool_call : null,
				input: modalities(modes.input), output: modalities(modes.output),
				context: number(limit.context, true), maxOutput: number(limit.output, true),
				pricing: { input: number(cost.input), output: number(cost.output), cacheRead: number(cost.cache_read) }
			});
			seen.add(id);
		}
		if (models.length) providers.push({ id: providerId, name: text(raw.name) ?? providerId, api: safeApi(raw.api), models });
	}
	if (!providers.length) throw new ModelCatalogError('no_valid_entries', 'The model catalog contains no valid model entries.');
	return { fetchedAt, providers };
}

/** Only explicitly implemented OpenAI-compatible text adapters can consume a catalog API URL. */
export function catalogTextEndpoint(provider: CatalogProvider): string | null {
	if (!(OPENAI_TEXT_PROVIDER_IDS as readonly string[]).includes(provider.id)) return null;
	const api = safeApi(provider.api);
	if (!api) return null;
	const url = new URL(api);
	const path = url.pathname.replace(/\/+$/, '');
	url.pathname = path.endsWith('/chat/completions') ? path : `${path}/chat/completions`;
	return url.href;
}

/** An adapter is a protocol contract, never a guess based on a model's name or audio input. */
export function implementedCapability(
	providerId: string,
	model: CatalogModel | null,
	protocol?: CatalogSelection['protocol']
): { usable: boolean; reason: string; speech: boolean } {
	const no = (reason: string) => ({ usable: false, reason, speech: false });
	const transport = protocol ?? 'openai_text';
	if (!identity(providerId)) return no('A valid provider identity is required.');
	if (model && model.providerId !== providerId) return no('This model belongs to a different provider.');
	if (model?.status === 'deprecated') return no('This model is deprecated. Choose a supported model or an explicit custom deployment.');
	if (model?.tools === false) return no('This model explicitly does not support the tool calls Arvel uses.');
	if (transport === 'codex_live') {
		if (providerId !== CODEX_LIVE_PROVIDER_ID || model?.id !== CODEX_LIVE_MODEL_ID) {
			return no('Codex live is a separate OpenAI account integration, not a catalog realtime adapter.');
		}
		return { usable: true, speech: true, reason: 'Codex live provides speech through the separate account integration. Account eligibility is checked separately.' };
	}
	if (transport === 'nemotron_labs_realtime') {
		if (!(NEMOTRON_LABS_PROVIDER_IDS as readonly string[]).includes(providerId) || model?.id !== NEMOTRON_LABS_MODEL_ID) {
			return no('Only the exact released NVIDIA-NemotronLabs-VoiceChat-11B model has the documented Labs realtime adapter.');
		}
		return { usable: true, speech: true, reason: 'Self-hosted Labs 11B deployment: speech and tools use the documented NVIDIA-NeMo/Speech nemotron-labs-voicechat protocol. This explicit override is not an official Nebius Token Factory listing or the different 12B models.dev artifact. Configure and verify the exact deployment endpoint.' };
	}
	if (model?.id === NEMOTRON_LABS_MODEL_ID) {
		return no('The released Labs native model does not support ordinary typed conversation. Choose the documented native realtime protocol.');
	}
	// A null catalog model plus an explicit owner-selected protocol is a custom deployment,
	// not permission to load an unknown provider's catalog SDK.
	const customText = model === null && protocol === 'openai_text';
	if (transport !== 'openai_text' || (!customText && !(OPENAI_TEXT_PROVIDER_IDS as readonly string[]).includes(providerId))) {
		return no('No implemented text adapter for this provider. Catalog listing does not enable its SDK or realtime protocol.');
	}
	if (model?.input !== null && model?.input !== undefined && !model.input.includes('text')) {
		return no('The catalog does not report text input for this model. Audio input alone does not enable a conversation adapter.');
	}
	if (model?.output !== null && model?.output !== undefined && !model.output.includes('text')) {
		return no('The catalog does not report text output for this model. Audio modalities alone do not establish an implemented realtime protocol.');
	}
	return { usable: true, speech: false, reason: model === null ?
		'Custom or saved text deployment. Verify its OpenAI-compatible chat endpoint and tool support; catalog access is not required.' :
		model.tools === null || model.input === null || model.output === null ?
			'OpenAI-compatible text adapter available; some catalog capabilities are unknown. Verify text and tool support on this deployment. Speech uses the separate text/STT/TTS pipeline.' :
			'OpenAI-compatible text and tools. Speech uses the separate text/STT/TTS pipeline, not native audio.' };
}

function exact(value: Record<string, unknown>, keys: string[]): boolean {
	return Object.keys(value).length === keys.length && keys.every((key) => Object.hasOwn(value, key));
}

function cachedModalities(value: unknown): boolean {
	const parsed = modalities(value);
	return value === null || (parsed !== null && parsed.length === (value as unknown[]).length);
}

// Strictly validate the sanitized shape instead of treating browser storage as trusted metadata.
function cachedSnapshot(value: unknown): value is CatalogSnapshot {
	if (!record(value) || !exact(value, ['fetchedAt', 'providers']) || number(value.fetchedAt) === null ||
		!Array.isArray(value.providers) || !value.providers.length) return false;
	const providers = new Set<string>();
	for (const provider of value.providers) {
		if (!record(provider) || !exact(provider, ['id', 'name', 'api', 'models']) || !identity(provider.id) ||
			!text(provider.name) || (provider.api !== null && safeApi(provider.api) !== provider.api) ||
			!Array.isArray(provider.models) || !provider.models.length || providers.has(provider.id as string)) return false;
		providers.add(provider.id as string);
		const models = new Set<string>();
		for (const model of provider.models) {
			if (!record(model) || !exact(model, ['providerId', 'id', 'name', 'status', 'tools', 'input', 'output', 'context', 'maxOutput', 'pricing']) ||
				model.providerId !== provider.id || !identity(model.id) || models.has(model.id as string) || !text(model.name) ||
				(model.status !== null && !text(model.status)) || (model.tools !== null && typeof model.tools !== 'boolean') ||
				!cachedModalities(model.input) || !cachedModalities(model.output) ||
				(model.context !== null && number(model.context, true) === null) ||
				(model.maxOutput !== null && number(model.maxOutput, true) === null) || !record(model.pricing) ||
				!exact(model.pricing, ['input', 'output', 'cacheRead']) ||
				Object.values(model.pricing).some((cost) => cost !== null && number(cost) === null)) return false;
			models.add(model.id as string);
		}
	}
	return true;
}

export function loadCachedModelCatalog(): CatalogSnapshot | null {
	if (typeof window === 'undefined') return null;
	try {
		const saved = window.localStorage.getItem(CACHE_KEY);
		if (!saved || saved.length > CACHE_BYTES || encoder.encode(saved).byteLength > CACHE_BYTES) return null;
		const value: unknown = JSON.parse(saved);
		return record(value) && exact(value, ['version', 'snapshot']) && value.version === CACHE_VERSION &&
			cachedSnapshot(value.snapshot) ? value.snapshot : null;
	} catch {
		// Disabled storage and malformed/outdated cache do not block saved configuration.
		return null;
	}
}

function cache(snapshot: CatalogSnapshot): void {
	try {
		const saved = JSON.stringify({ version: CACHE_VERSION, snapshot });
		if (saved.length <= CACHE_BYTES && encoder.encode(saved).byteLength <= CACHE_BYTES) {
			window.localStorage.setItem(CACHE_KEY, saved);
		}
	} catch {
		// Quota/privacy errors are nonfatal. Nothing except sanitized metadata is persisted.
	}
}

export async function fetchModelCatalog(signal?: AbortSignal): Promise<CatalogSnapshot> {
	if (typeof window === 'undefined') throw new ModelCatalogError('browser_only', 'Fetch the model catalog from the browser, not the device.');
	const controller = new AbortController();
	const abort = () => controller.abort(signal?.reason);
	if (signal?.aborted) abort();
	else signal?.addEventListener('abort', abort, { once: true });
	const timer = setTimeout(() => controller.abort(new DOMException('Model catalog request timed out.', 'TimeoutError')), TIMEOUT_MS);
	let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
	try {
		const response = await fetch(CATALOG_URL, {
			method: 'GET', credentials: 'omit', redirect: 'error', referrerPolicy: 'no-referrer', signal: controller.signal
		});
		if (!response.ok || response.redirected || response.type === 'opaqueredirect') {
			throw new ModelCatalogError('http_error', `Could not fetch the model catalog (HTTP ${response.status}).`);
		}
		const advertised = response.headers.get('content-length');
		if (advertised !== null && /^\d+$/.test(advertised) && Number(advertised) > RESPONSE_BYTES) {
			throw new ModelCatalogError('too_large', 'The model catalog exceeds the 16 MiB download limit.');
		}
		if (!response.body) throw new ModelCatalogError('invalid_response', 'The model catalog response has no readable body.');
		reader = response.body.getReader();
		const decoder = new TextDecoder('utf-8', { fatal: true });
		const chunks: string[] = [];
		let bytes = 0;
		while (true) {
			controller.signal.throwIfAborted();
			const chunk = await reader.read();
			controller.signal.throwIfAborted();
			if (chunk.done) break;
			bytes += chunk.value.byteLength;
			if (bytes > RESPONSE_BYTES) throw new ModelCatalogError('too_large', 'The model catalog exceeds the 16 MiB download limit.');
			chunks.push(decoder.decode(chunk.value, { stream: true }));
		}
		chunks.push(decoder.decode());
		let parsed: unknown;
		try {
			parsed = JSON.parse(chunks.join(''));
		} catch {
			throw new ModelCatalogError('invalid_catalog', 'The model catalog is not valid JSON.');
		}
		const snapshot = parseModelCatalog(parsed);
		controller.signal.throwIfAborted();
		cache(snapshot);
		return snapshot;
	} finally {
		clearTimeout(timer);
		signal?.removeEventListener('abort', abort);
		controller.abort();
		if (reader) {
			try { await reader.cancel(); } catch { /* The stream may already have failed or been aborted. */ }
			reader.releaseLock();
		}
	}
}
